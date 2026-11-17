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
        self.assertEqual(rows[1][0] - rows[0][0], 25*60)
        self.assertEqual(rows[2][0] - rows[1][0], 40*60)

    def test_last_good_not_lost(self):
        with tempfile.TemporaryDirectory() as root:
            state = plugin.GuideState(os.path.join(root, "epg.xml"))
            sample = b"<?xml version='1.0'?><tv></tv>"
            state.replace(sample)
            self.assertEqual(state.last_good, sample)
            self.assertEqual(open(state.output_path, "rb").read(), sample)

    def test_no_schedule_refuses(self):
        with self.assertRaisesRegex(ValueError, "no valid"):
            plugin.build_xmltv(
                self.config, downloader=lambda _: "<p>No schedule</p>",
                sleeper=lambda _: None)

    def test_discovery_limit(self):
        page = "".join("<a href='/channel/ch-%d'>Channel %d</a>" % (i, i)
                       for i in range(10))
        self.assertEqual(len(plugin.discover_pages(page, 3)), 3)


if __name__ == "__main__":
    unittest.main()
