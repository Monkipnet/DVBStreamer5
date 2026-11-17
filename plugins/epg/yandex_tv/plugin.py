#!/usr/bin/env python3
"""Isolated, opt-in HTML-to-XMLTV EPG provider for DVBStreamer5.

Python 3.6+ standard library, loopback-only. Never accesses media or CAM.
No CAPTCHA bypass, cookies, login emulation or automated browser.
"""
import argparse
import datetime as dt
from html.parser import HTMLParser
import json
import logging
import os
import re
import socketserver
import tempfile
import threading
import time
import xml.etree.ElementTree as ET
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.error import HTTPError, URLError
from urllib.parse import urlparse, urljoin, parse_qs
from urllib.request import Request, HTTPRedirectHandler, build_opener

LOG = logging.getLogger("epg-yandex-tv")
HOST, PORT = "127.0.0.1", 19785
MAX_HTML, MAX_CHANNELS, MAX_EVENTS = 4 * 1024 * 1024, 32, 512
CAPTCHA = ("smartcaptcha", "captcha.yandex", "showcaptcha", "g-recaptcha")
HHMM = re.compile(r"^\s*(\d\d):(\d\d)")
ISO = re.compile(
    r"^(\d{4}-\d\d-\d\d)[T ](\d\d):(\d\d)(?::(\d\d))?(Z|[+-]\d\d:?\d\d)?$"
)


def allowed_url(url):
    if not isinstance(url, str) or len(url) > 2048:
        return False
    try:
        p = urlparse(url)
        return (p.scheme == "https" and p.hostname == "tv.yandex.by"
                and p.port in (None, 443) and not p.username and
                not p.password and not p.fragment)
    except ValueError:
        return False


def timestamp(day, hour, minute, offset, explicit=""):
    seconds = 0
    if explicit:
        match = ISO.fullmatch(explicit)
        if not match:
            raise ValueError("invalid ISO datetime")
        day = match.group(1)
        hour, minute = int(match.group(2)), int(match.group(3))
        seconds = int(match.group(4) or "0")
        zone = match.group(5)
        if zone == "Z":
            offset = 0
        elif zone:
            digits = zone[1:].replace(":", "")
            zh, zm = int(digits[:2]), int(digits[2:])
            if zh > 14 or zm > 59:
                raise ValueError("invalid UTC offset")
            offset = (1 if zone[0] == "+" else -1) * (zh * 60 + zm)
    if not (0 <= hour <= 23 and 0 <= minute <= 59 and 0 <= seconds <= 59):
        raise ValueError("invalid clock")
    local = dt.datetime.strptime(day, "%Y-%m-%d").replace(
        hour=hour, minute=minute, second=seconds,
        tzinfo=dt.timezone(dt.timedelta(minutes=offset)))
    return int(local.timestamp())


def default_day(url, offset):
    values = parse_qs(urlparse(url).query).get("date", [])
    if values and re.fullmatch(r"\d{4}-\d\d-\d\d", values[0]):
        return values[0]
    zone = dt.timezone(dt.timedelta(minutes=offset))
    return dt.datetime.now(zone).date().isoformat()


class GuideParser(HTMLParser):
    VOID = {"br", "img", "input", "link", "meta", "hr", "source"}

    def __init__(self, url, ident="", name="", offset=180):
        HTMLParser.__init__(self, convert_charrefs=True)
        # HTMLParser changes self.offset while scanning HTML columns.
        # Keep our timezone separate from its internal parser position.
        self.ident, self.name, self.timezone_minutes = ident, name, offset
        self.base_url = url
        self.fixed_channel = bool(ident)
        self.date = default_day(url, offset)
        self.stack, self.heading, self.row, self.anchor = [], None, None, None
        self.h3, self.nodes = False, 0
        self.programmes, self.channels, self.links = {}, {}, []
        if self.ident:
            self.channels[self.ident] = self.name or self.ident

    def handle_starttag(self, tag, pairs):
        self.nodes += 1
        if self.nodes > 100000:
            raise ValueError("HTML node count exceeds safety limit")
        a = dict(pairs)
        if tag not in self.VOID:
            self.stack.append((tag, a.get("data-date", "")))
        if tag == "h2" and not self.fixed_channel:
            self.heading = ([], a.get("data-channel-id", ""))
        elif tag == "li":
            date = next((d for _, d in reversed(self.stack) if d), self.date)
            self.row = {"body": [], "h3": [], "date": date, "datetime": ""}
            self.h3 = False
        elif tag == "h3" and self.row is not None:
            self.h3 = True
        elif tag == "time" and self.row is not None:
            self.row["datetime"] = a.get("datetime", "")
        elif tag == "a":
            self.anchor = ([], a.get("href", ""))

    def handle_data(self, value):
        if self.heading is not None:
            self.heading[0].append(value)
        if self.row is not None:
            self.row["body"].append(value)
            if self.h3:
                self.row["h3"].append(value)
        if self.anchor is not None:
            self.anchor[0].append(value)

    def handle_endtag(self, tag):
        if tag == "h2" and self.heading is not None:
            title = " ".join("".join(self.heading[0]).split())
            ident = self.heading[1] or title
            if title and 0 < len(ident) <= 128:
                self.name, self.ident = title, ident
                self.channels[self.ident] = title
            self.heading = None
        elif tag == "h3":
            self.h3 = False
        elif tag == "li" and self.row is not None:
            self.append_row()
            self.row = None
        elif tag == "a" and self.anchor is not None:
            self.links.append((urljoin(self.base_url, self.anchor[1]),
                               " ".join("".join(self.anchor[0]).split())))
            self.anchor = None
        for i in range(len(self.stack) - 1, -1, -1):
            if self.stack[i][0] == tag:
                del self.stack[i:]
                break

    def append_row(self):
        if not self.ident:
            return
        line = " ".join("".join(self.row["body"]).split())
        match = HHMM.match(line)
        if not match:
            return
        title = " ".join("".join(self.row["h3"]).split())
        title = title or line[match.end():].strip()
        if not title or len(title) > 512:
            return
        try:
            when = timestamp(self.row["date"], int(match.group(1)),
                             int(match.group(2)), self.timezone_minutes,
                             self.row["datetime"])
        except (ValueError, OverflowError):
            return
        items = self.programmes.setdefault(self.ident, [])
        if items and not self.row["datetime"]:
            # The source may omit data-date. Derive the next occurrence
            # of HH:MM from the previous event's LOCAL day, not a stale
            # page date; this handles 23:55 -> 00:20 across midnight.
            last = items[-1][0]
            day_utc = last - ((last + self.timezone_minutes * 60) % 86400)
            candidate = day_utc + int(match.group(1)) * 3600 + int(match.group(2)) * 60
            if candidate <= last:
                candidate += 86400
            if 0 < candidate - last <= 12 * 3600:
                when = candidate
        if items and when <= items[-1][0]:
            return
        if len(items) < MAX_EVENTS:
            items.append((when, title))
            self.channels[self.ident] = self.name or self.ident


def parse_html(markup, url, ident="", name="", offset=180):
    if len(markup.encode("utf-8")) > MAX_HTML:
        raise ValueError("HTML body exceeds 4 MiB")
    if any(word in markup.lower() for word in CAPTCHA):
        raise ValueError("CAPTCHA detected; no bypass attempted")
    parsed = GuideParser(url, ident, name, offset)
    parsed.feed(markup)
    parsed.close()
    return parsed


def intervals(parsed, now=None):
    now = time.time() if now is None else now
    for cid, items in parsed.programmes.items():
        for i in range(len(items) - 1):
            start, title = items[i]
            stop = items[i + 1][0]
            if (0 < stop - start <= 12 * 3600 and
                    stop >= now - 21600 and start <= now + 7 * 86400):
                yield cid, start, stop, title


class SameOriginRedirect(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        if not allowed_url(newurl):
            raise ValueError("unsafe cross-origin redirect blocked")
        return HTTPRedirectHandler.redirect_request(
            self, req, fp, code, msg, headers, newurl)


def fetch_html(url):
    if not allowed_url(url):
        raise ValueError("only HTTPS tv.yandex.by is allowed")
    request = Request(url, headers={
        "User-Agent": "DVBStreamer5-EPG-plugin/1.0",
        "Accept": "text/html", "Accept-Language": "ru-RU,ru;q=0.9"})
    with build_opener(SameOriginRedirect()).open(request, timeout=15) as response:
        if response.getcode() != 200:
            raise ValueError("HTTP status %s" % response.getcode())
        data = response.read(MAX_HTML + 1)
    if len(data) > MAX_HTML:
        raise ValueError("HTML download exceeds 4 MiB")
    return data.decode("utf-8", errors="replace")


def discover_pages_from_links(links, limit, base_url="https://tv.yandex.by/"):
    """Only same-origin /channel/ URLs; preserve configured day query."""
    found, seen = [], set()
    for href, name in links:
        url = urljoin(base_url, href)
        if not allowed_url(url):
            continue
        path = urlparse(url).path.rstrip("/")
        if "/channel/" not in path or path in seen:
            continue
        slug = path.rsplit("/", 1)[-1]
        if not slug or len(slug) > 128:
            continue
        seen.add(path)
        found.append({"id": slug, "name": (name or slug)[:256], "url": url})
        if len(found) >= min(limit, MAX_CHANNELS):
            break
    return found


def discover_pages(markup, limit, base_url="https://tv.yandex.by/"):
    if not allowed_url(base_url):
        raise ValueError("index URL must be HTTPS tv.yandex.by")
    parsed = parse_html(markup, base_url)
    return discover_pages_from_links(parsed.links, limit, base_url)


def validate_config(config):
    if not isinstance(config, dict) or config.get("enabled") is not True:
        raise ValueError("EPG plugin disabled (enabled=false)")
    if config.get("permission_confirmed") is not True:
        raise ValueError("data-use permission not confirmed")
    offset = config.get("timezone_minutes", 180)
    if type(offset) is not int or not -720 <= offset <= 840:
        raise ValueError("timezone_minutes must be -720..840")
    index_url = config.get("index_url", "https://tv.yandex.by/")
    if not allowed_url(index_url):
        raise ValueError("index_url must be HTTPS tv.yandex.by")
    delay = config.get("crawl_delay", 2.0)
    if (isinstance(delay, bool) or not isinstance(delay, (int, float))
            or not 0.5 <= delay <= 30):
        raise ValueError("crawl_delay must be 0.5..30 seconds")
    if type(config.get("crawl_links", True)) is not bool:
        raise ValueError("crawl_links must be boolean")
    ttl = config.get("cache_ttl_seconds", 3600)
    if type(ttl) is not int or not 60 <= ttl <= 86400:
        raise ValueError("cache_ttl_seconds must be 60..86400")
    pages = config.get("channel_pages", [])
    if not isinstance(pages, list) or len(pages) > MAX_CHANNELS:
        raise ValueError("channel_pages must contain up to 32 channels")
    for page in pages:
        if (not isinstance(page, dict) or
                not allowed_url(page.get("url", "")) or
                not isinstance(page.get("id"), str) or
                not 0 < len(page["id"]) <= 128 or
                not isinstance(page.get("name"), str) or
                not 0 < len(page["name"]) <= 256):
            raise ValueError("invalid channel_pages entry")
    return offset


def build_xmltv(config, downloader=fetch_html, sleeper=time.sleep, now=None):
    """Merge index-page programmes with discovered/per-channel schedules."""
    offset = validate_config(config)
    pages = config.get("channel_pages", [])
    channels, shows = {}, []
    delay = float(config.get("crawl_delay", 2.0))

    def collect(parsed, ident=None, name=None):
        for cid, start, stop, title in intervals(parsed, now):
            cid = ident or cid
            if cid not in channels and len(channels) >= MAX_CHANNELS:
                continue
            channels[cid] = name or parsed.channels.get(cid, cid)
            if len(shows) < MAX_CHANNELS * MAX_EVENTS:
                shows.append((cid, start, stop, title))

    if not pages:
        index_url = config.get("index_url", "https://tv.yandex.by/")
        guide = parse_html(downloader(index_url), index_url, offset=offset)
        collect(guide)
        if config.get("crawl_links", True):
            limit = config.get("max_channels", 16)
            if type(limit) is not int or not 1 <= limit <= MAX_CHANNELS:
                limit = 16
            pages = discover_pages_from_links(guide.links, limit, index_url)

    for i, page in enumerate(pages):
        if i or not config.get("channel_pages", []):
            sleeper(delay)
        try:
            guide = parse_html(downloader(page["url"]), page["url"],
                               page["id"], page["name"], offset)
            collect(guide, page["id"], page["name"])
        except (ValueError, IOError, OSError, HTTPError, URLError,
                UnicodeError) as exc:
            LOG.warning("Skipping unreadable channel %s: %s", page.get("id"), exc)
    if not shows:
        raise ValueError("no valid HTML schedule: markup or dates unavailable")

    root = ET.Element("tv", {"generator-info-name": "DVBStreamer5 separate EPG plugin"})
    for cid, name in sorted(channels.items()):
        ET.SubElement(ET.SubElement(root, "channel", id=cid),
                      "display-name", lang="ru").text = name

    def xmltv_time(when):
        return dt.datetime.fromtimestamp(
            when, dt.timezone.utc).strftime("%Y%m%d%H%M%S +0000")

    for cid, start, stop, title in sorted(set(shows)):
        p = ET.SubElement(root, "programme", {
            "channel": cid, "start": xmltv_time(start), "stop": xmltv_time(stop)})
        ET.SubElement(p, "title", lang="ru").text = title
    return ET.tostring(root, encoding="utf-8", xml_declaration=True)


class GuideState:
    def __init__(self, output_path):
        self.output_path = output_path
        self.lock = threading.Lock()
        self.last_good = None
        self.last_error = ""
        self.last_success = 0
        self.last_attempt = 0
        self.last_refresh_request = 0
        self.wake = threading.Event()
        self.channels = 0
        self.programmes = 0
        try:
            with open(output_path, "rb") as file:
                previous = file.read(12 * 1024 * 1024)
            if previous.startswith(b"<?xml") and b"<tv" in previous:
                try:
                    doc = ET.fromstring(previous)
                    self.last_good = previous
                    self.last_success = int(os.path.getmtime(output_path))
                    self.channels = len(doc.findall("channel"))
                    self.programmes = len(doc.findall("programme"))
                except ET.ParseError:
                    pass
        except (OSError, IOError):
            pass

    def replace(self, xml):
        folder = os.path.dirname(self.output_path)
        os.makedirs(folder, exist_ok=True)
        fd, temp = tempfile.mkstemp(prefix=".epg-", dir=folder)
        try:
            with os.fdopen(fd, "wb") as file:
                file.write(xml)
                file.flush()
                os.fsync(file.fileno())
            os.chmod(temp, 0o600)
            os.replace(temp, self.output_path)
        finally:
            if os.path.exists(temp):
                os.unlink(temp)
        with self.lock:
            self.last_good, self.last_error = xml, ""
            self.last_success = int(time.time())
            self.channels = xml.count(b"<channel ")
            self.programmes = xml.count(b"<programme ")

    def request_refresh(self, ttl=None):
        # Queue asynchronous work so an EPG client never waits for scraping.
        with self.lock:
            now = time.time()
            if ttl is not None and self.last_success and now - self.last_success < ttl:
                return False
            if now - self.last_refresh_request < 60:
                return False
            self.last_refresh_request = now
            self.wake.set()
            return True


class LoopbackHttpServer(socketserver.ThreadingMixIn, HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        parsed = urlparse(self.path)
        if parsed.path not in ("/health", "/epg.xml", "/xmltv", "/", "/refresh"):
            self.send_error(404)
            return
        state = self.server.state
        query = parse_qs(parsed.query)
        refresh_queued = False
        if parsed.path == "/refresh" or query.get("refresh", ["0"])[0] in (
                "1", "true", "yes"):
            refresh_queued = state.request_refresh()
        elif parsed.path not in ("/health", "/refresh"):
            refresh_queued = state.request_refresh(self.server.cache_ttl_seconds)
        with state.lock:
            data = state.last_good
            status = {
                "ready": bool(data), "last_error": state.last_error,
                "last_attempt": state.last_attempt,
                "last_success": state.last_success,
                "channel_count": state.channels, "event_count": state.programmes,
                "refresh_requested": state.last_refresh_request,
                "refresh_queued": refresh_queued
            }
        if parsed.path in ("/health", "/refresh"):
            body = json.dumps(status).encode("utf-8")
            code = 202 if parsed.path == "/refresh" and refresh_queued else 200
            mime = "application/json"
        else:
            body = data or b"EPG not available"
            code, mime = (200, "application/xml") if data else (503, "text/plain")
        self.send_response(code)
        self.send_header("Content-Type", mime)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        LOG.debug(fmt, *args)


def serve(config_path, output_path):
    with open(config_path, encoding="utf-8") as file:
        config = json.load(file)
    validate_config(config)
    state = GuideState(output_path)
    server = LoopbackHttpServer((HOST, PORT), Handler)
    server.state = state
    server.cache_ttl_seconds = config.get("cache_ttl_seconds", 3600)
    hours = config.get("refresh_hours", 6)
    if type(hours) is not int or not 1 <= hours <= 24:
        hours = 6

    def update_loop():
        while True:
            with state.lock:
                state.last_attempt = int(time.time())
            try:
                state.replace(build_xmltv(config))
                LOG.info("EPG HTML parsed successfully: %d channels / %d events",
                         state.channels, state.programmes)
            except (ValueError, OSError, IOError, URLError, HTTPError,
                    UnicodeError) as exc:
                with state.lock:
                    state.last_error = str(exc)[:300]
                LOG.warning("EPG refresh failed; last guide retained: %s", exc)
            state.wake.wait(hours * 3600)
            state.wake.clear()

    threading.Thread(target=update_loop, daemon=True).start()
    LOG.info("Serving EPG on http://127.0.0.1:19785/epg.xml")
    server.serve_forever()


def main():
    args = argparse.ArgumentParser()
    args.add_argument("--config", required=True)
    args.add_argument("--output", required=True)
    parsed = args.parse_args()
    logging.basicConfig(level=logging.INFO)
    serve(parsed.config, parsed.output)


if __name__ == "__main__":
    main()
