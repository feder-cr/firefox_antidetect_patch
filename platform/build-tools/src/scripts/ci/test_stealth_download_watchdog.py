# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

import inspect
import os
import signal
import sys
import time
import types
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
VSDIR = os.path.join(ROOT, "third_party", "python", "vsdownload")
sys.path.insert(0, VSDIR)

import stealth_download_watchdog as m  # noqa: E402

HAVE_ALARM = hasattr(signal, "SIGALRM")


def _sentinel(signum, frame):
    pass


class WatchdogTest(unittest.TestCase):
    def setUp(self):
        self._orig = m.urllib.request.urlretrieve
        if HAVE_ALARM:
            self._prev = signal.signal(signal.SIGALRM, _sentinel)

    def tearDown(self):
        m.urllib.request.urlretrieve = self._orig
        if HAVE_ALARM:
            signal.alarm(0)
            signal.signal(signal.SIGALRM, self._prev)

    def _assert_clean(self):
        self.assertIs(signal.getsignal(signal.SIGALRM), _sentinel)
        self.assertEqual(signal.getitimer(signal.ITIMER_REAL)[0], 0)

    @unittest.skipUnless(HAVE_ALARM, "SIGALRM is Unix-only")
    def test_timeout_fires(self):
        m.urllib.request.urlretrieve = lambda url, dest: time.sleep(5)
        with self.assertRaises(TimeoutError) as cm:
            m.stealth_urlretrieve({"url": "u"}, "d", "pkg/file.msi", timeout=1)
        self.assertEqual(str(cm.exception), "download exceeded 1s for pkg/file.msi")
        self.assertIsInstance(cm.exception, OSError)
        self.assertIsInstance(cm.exception, Exception)
        self._assert_clean()

    @unittest.skipUnless(HAVE_ALARM, "SIGALRM is Unix-only")
    def test_success_path(self):
        calls = []
        m.urllib.request.urlretrieve = lambda url, dest: calls.append((url, dest))
        m.stealth_urlretrieve({"url": "u"}, "d", "f")
        self.assertEqual(calls, [("u", "d")])
        self._assert_clean()

    @unittest.skipUnless(HAVE_ALARM, "SIGALRM is Unix-only")
    def test_missing_url_cleans_up(self):
        with self.assertRaises(KeyError):
            m.stealth_urlretrieve({}, "d", "f")
        self._assert_clean()

    def test_no_sigalrm_platform(self):
        calls = []
        m.urllib.request.urlretrieve = lambda url, dest: calls.append((url, dest))
        real = m.signal
        m.signal = types.SimpleNamespace()
        try:
            m.stealth_urlretrieve({"url": "u"}, "d", "f")
        finally:
            m.signal = real
        self.assertEqual(calls, [("u", "d")])
        default = inspect.signature(m.stealth_urlretrieve).parameters["timeout"].default
        self.assertEqual(default, 120)

    def test_hook_presence_guard(self):
        with open(os.path.join(VSDIR, "vsdownload.py"), encoding="utf-8") as f:
            text = f.read()
        self.assertEqual(text.count("stealth_urlretrieve(payload, destname, fileid)"), 1)
        self.assertNotIn("signal.", text)
        self.assertNotIn('urllib.request.urlretrieve(payload["url"]', text)


if __name__ == "__main__":
    unittest.main()
