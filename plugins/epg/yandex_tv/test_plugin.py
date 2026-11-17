import datetime as dt
import os
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

sys.path.insert(0, os.path.dirname(__file__))
import plugin


class TestYandexEpgPlugin(unittest.TestCase):
    def setUp(self):
        self.url = "https://tv.yandex.by/channel/ntv-11"
        self.config = {
            "enabled": True, "permission_confirmed": True,
            "timezone_minutes": 180, "refresh_hours": 6,
            "channel_pages": [{
                "id": "ntv-11", "name": "НТВ", "url": self.url}]
        }

    def fixture(self):
        zone = dt.timezone(dt.timedelta(hours=3))
        entries = []
        for minutes in (-5, 25, 65):
            t = dt.datetime.now(zone) + dt.timedelta(minutes=minutes)
            iso = t.strftime("%Y-%m-%dT%H:%M:%S+03:00")
            entries.append('<li><time datetime="%s">%s</time>'
                           '<h3>Programme %d</h3></li>' %
                           (iso, iso[11:16], len(entries)))
        return "<html><h2>НТВ</h2><ul>" + "".join(entries) + "</ul></html>"

    def test_xmltv_fully_timed(self):
        result = plugin.build_xmltv(
            self.config, downloader=lambda _: self.fixture(),
            sleeper=lambda _: None)
        root = ET.fromstring(result)
        self.assertEqual(len(root.findall("channel")), 1)
        entries = root.findall("programme")
        self.assertEqual(len(entries), 2)
        self.assertEqual(entries[0].find("title").text, "Programme 0")
        self.assertTrue(entries[0].get("start").endswith(" +0000"))

    def test_disabled_and_permission(self):
        for key in ("enabled", "permission_confirmed"):
            config = dict(self.config)
            config[key] = False
            with self.assertRaises(ValueError):
                plugin.validate_config(config)

    def test_captcha_fail_closed(self):
        with self.assertRaisesRegex(ValueError, "CAPTCHA"):
            plugin.parse_html("<div>SmartCaptcha</div>", self.url)

    def test_url_validation(self):
        for url in ("http://tv.yandex.by/channel/ntv-11",
                    "https://tv.yandex.by.evil.test/channel",
                    "https://169.254.169.254/", "file:///etc/passwd"):
            self.assertFalse(plugin.allowed_url(url))
        self.assertTrue(plugin.allowed_url(self.url))

    def test_midnight_rollover(self):
        page = "<h2>НТВ</h2><ul><li>23:55 First</li>" \
               "<li>00:20 Second</li><li>01:00 Third</li></ul>"
        data = plugin.parse_html(
            page, self.url + "?date=2026-11-17")
        rows = next(iter(data.programmes.values()))
        self.assertEqual(rows[0][0], plugin.timestamp("2026-11-17", 23, 55, 180))
        self.assertEqual(rows[1][0] - rows[0][0], 25*60, repr(rows))
        self.assertEqual(rows[2][0] - rows[1][0], 40*60, repr(rows))

    def test_last_good_not_lost(self):
        with tempfile.TemporaryDirectory() as root:
            state = plugin.GuideState(os.path.join(root, "epg.xml"))
            sample = b"<?xml version='1.0'?><tv></tv>"
            state.replace(sample)
            self.assertEqual(state.last_good, sample)
            with open(state.output_path, "rb") as stored:
                self.assertEqual(stored.read(), sample)

    def test_no_schedule_refuses(self):
        with self.assertRaisesRegex(ValueError, "no valid"):
            plugin.build_xmltv(
                self.config, downloader=lambda _: "<p>No schedule</p>",
                sleeper=lambda _: None)

    def test_discovery_limit(self):
        page = "".join("<a href='/channel/ch-%d'>Channel %d</a>" % (i, i)
                       for i in range(10))
        self.assertEqual(len(plugin.discover_pages(page, 3)), 3)


    def test_index_multiple_channels_and_resolved_links(self):
        index = ("<html><a href='/29/channel/news-16'>Новости</a>"
                 "<a href='https://bad.invalid/channel/no'>Bad</a>"
                 "<h2 data-channel-id='one'>Первый</h2>"
                 "<ul><li>10:00 Утро</li><li>10:30 Новости</li></ul>"
                 "<h2 data-channel-id='two'>Второй</h2>"
                 "<ul><li>11:00 Фильм</li><li>12:00 Финал</li></ul></html>")
        parsed = plugin.parse_html(
            index, "https://tv.yandex.by/?date=2026-11-17")
        self.assertEqual(set(parsed.channels), {"one", "two"})
        self.assertEqual(len(parsed.programmes["one"]), 2)
        self.assertEqual(len(parsed.programmes["two"]), 2)
        pages = plugin.discover_pages(index, 16)
        self.assertEqual(pages[0]["url"],
                         "https://tv.yandex.by/29/channel/news-16")
        self.assertEqual(len(pages), 1)

    def test_automatic_index_crawl_and_partial_error(self):
        zone = dt.timezone(dt.timedelta(hours=3))
        times = []
        for minutes in (0, 30, 60):
            t = dt.datetime.now(zone) + dt.timedelta(minutes=minutes)
            iso = t.strftime("%Y-%m-%dT%H:%M:%S+03:00")
            times.append("<li><time datetime='%s'>%s</time><h3>Item</h3></li>" %
                         (iso, iso[11:16]))
        index = "<a href='/29/channel/c1'>One</a><a href='/29/channel/c2'>Two</a>"
        page = "<h2>One</h2><ul>" + "".join(times) + "</ul>"
        calls = []
        def download(url):
            calls.append(url)
            if url == "https://tv.yandex.by/":
                return index
            if "/channel/c1" in url:
                return page
            raise ValueError("unavailable channel")
        config = dict(self.config)
        config["channel_pages"] = []
        config["crawl_delay"] = 0.5
        paused = []
        result = plugin.build_xmltv(config, downloader=download,
                                  sleeper=paused.append)
        xml = ET.fromstring(result)
        self.assertEqual([c.get("id") for c in xml.findall("channel")], ["c1"])
        self.assertEqual(len(xml.findall("programme")), 2)
        self.assertEqual(len(paused), 2)
        self.assertEqual(paused, [0.5, 0.5])
        self.assertEqual(len(calls), 3)

    def test_empty_index_not_accepted_as_valid_epg(self):
        config = dict(self.config)
        config["channel_pages"] = []
        config["crawl_links"] = False
        with self.assertRaisesRegex(ValueError, "no valid"):
            plugin.build_xmltv(config, downloader=lambda _: "<html></html>",
                              sleeper=lambda _: None)

    def test_configuration_validation_and_cache_refresh_gate(self):
        for field, value in [
                ("index_url", "http://tv.yandex.by/"),
                ("index_url", "https://example.com/"),
                ("crawl_delay", 0.0),
                ("cache_ttl_seconds", -1),
                ("crawl_links", "false")]:
            config = dict(self.config)
            config[field] = value
            with self.assertRaises(ValueError):
                plugin.validate_config(config)
        with tempfile.TemporaryDirectory() as folder:
            state = plugin.GuideState(os.path.join(folder, "guide.xml"))
            self.assertTrue(state.request_refresh())
            self.assertTrue(state.wake.is_set())
            self.assertFalse(state.request_refresh())
            self.assertTrue(state.last_refresh_request > 0)

    def test_loopback_api_health_alias_refresh_and_stale(self):
        import json
        import threading
        from urllib.request import urlopen
        from urllib.error import HTTPError
        with tempfile.TemporaryDirectory() as folder:
            state = plugin.GuideState(os.path.join(folder, "epg.xml"))
            sample = (b'<?xml version="1.0"?><tv>'
                      b'<channel id="ch"/><programme channel="ch"/></tv>')
            state.replace(sample)
            server = plugin.LoopbackHttpServer(("127.0.0.1", 0),
                                               plugin.Handler)
            server.state = state
            server.cache_ttl_seconds = 3600
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            base = "http://127.0.0.1:%s" % server.server_port
            try:
                self.assertEqual(urlopen(base + "/xmltv").read(), sample)
                self.assertEqual(urlopen(base + "/epg.xml").read(), sample)
                health = json.loads(urlopen(base + "/health").read().decode())
                self.assertTrue(health["ready"])
                self.assertEqual(health["channel_count"], 1)
                response = urlopen(base + "/refresh")
                self.assertEqual(response.status, 202)
                self.assertTrue(json.loads(response.read().decode())["refresh_queued"])
                self.assertEqual(urlopen(base + "/refresh").status, 200)
                self.assertEqual(urlopen(base + "/epg.xml?refresh=1").read(), sample)
                with self.assertRaises(HTTPError) as result:
                    urlopen(base + "/does-not-exist")
                self.assertEqual(result.exception.code, 404)
            finally:
                server.shutdown()
                server.server_close()
                thread.join(timeout=2)


    def test_isolated_systemd_configuration_path(self):
        base = os.path.dirname(__file__)
        with open(os.path.join(base, "epg-yandex-tv.service"),
                  encoding="utf-8") as stream:
            service = stream.read()
        with open(os.path.join(base, "install_plugin.sh"),
                  encoding="utf-8") as stream:
            installer = stream.read()
        self.assertIn("DynamicUser=yes", service)
        self.assertIn("--config /etc/epg-yandex-tv/config.json", service)
        self.assertNotIn("--config /etc/dvbstreamer5/", service)
        self.assertIn('install -d -m 0755 "$CFG_DIR"', installer)
        self.assertIn('install -m 0644 -- "$LEGACY" "$CFG"', installer)
        self.assertIn('Preserving existing plugin config', installer)
        self.assertNotIn("systemctl restart dvbstreamer5", installer)
        self.assertNotIn("systemctl restart oscam-mini", installer)


if __name__ == "__main__":
    unittest.main()
