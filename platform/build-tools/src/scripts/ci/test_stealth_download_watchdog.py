# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

import http.server
import inspect
import multiprocessing
import os
import pickle
import signal
import sys
import tempfile
import threading
import time
import types
import unittest
import urllib.error

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
VSDIR = os.path.join(ROOT, "third_party", "python", "vsdownload")
sys.path.insert(0, VSDIR)

import stealth_download_watchdog as m  # noqa: E402

HAVE_ALARM = hasattr(signal, "SIGALRM")


def _sentinel(signum, frame):
    pass


def _cut_short(url, dest):
    raise urllib.error.ContentTooShortError(
        "retrieval incomplete: got only 6291456 out of 15860356 bytes", b"")


def _still_cut(request):
    raise urllib.error.ContentTooShortError(
        "retrieval incomplete: got only 6291456 out of 15860356 bytes", b"")


def _download_in_worker():
    m.urllib.request.urlretrieve = _cut_short
    m.urllib.request.urlopen = _still_cut
    m.stealth_urlretrieve({"url": "http://cdn.invalid/file.cab"}, "d", "pkg/file.cab", wait=lambda s: None)


FILE = bytes(range(256)) * 64  # 16 KiB


class _CutEdge(http.server.BaseHTTPRequestHandler):
    """A CDN edge that holds the head of FILE only: a plain GET gets the head
    and the connection closes; a ranged GET reaches the origin."""
    cut = 4096
    ranges = []

    def do_GET(self):
        wanted = self.headers.get("Range")
        _CutEdge.ranges.append(wanted)
        if wanted is None:
            self.send_response(200)
            self.send_header("Content-Length", str(len(FILE)))
            self.end_headers()
            self.wfile.write(FILE[: self.cut])
            self.close_connection = True
            return
        start = int(wanted[len("bytes="):-1])
        self.send_response(206)
        self.send_header("Content-Range",
                         "bytes %d-%d/%d" % (start, len(FILE) - 1, len(FILE)))
        self.send_header("Content-Length", str(len(FILE) - start))
        self.end_headers()
        self.wfile.write(FILE[start:])

    def log_message(self, *args):
        pass


class WatchdogTest(unittest.TestCase):
    def setUp(self):
        self._orig = m.urllib.request.urlretrieve
        self._orig_open = m.urllib.request.urlopen
        if HAVE_ALARM:
            self._prev = signal.signal(signal.SIGALRM, _sentinel)

    def tearDown(self):
        m.urllib.request.urlretrieve = self._orig
        m.urllib.request.urlopen = self._orig_open
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

    def test_an_unpicklable_error_comes_back_as_an_oserror(self):
        errors = (
            urllib.error.ContentTooShortError("retrieval incomplete", b""),
            urllib.error.HTTPError("u", 503, "Service Unavailable", {}, None),
        )
        for error in errors:
            def fail(url, dest, error=error):
                raise error

            def fail_again(request, error=error):
                raise error
            m.urllib.request.urlretrieve = fail
            m.urllib.request.urlopen = fail_again
            with self.assertRaises(OSError) as cm:
                m.stealth_urlretrieve({"url": "http://cdn.invalid/f"}, "d", "f", wait=lambda s: None)
            self.assertIs(type(cm.exception), OSError)
            self.assertTrue(str(cm.exception).startswith(type(error).__name__ + ": "))
            self.assertEqual(str(pickle.loads(pickle.dumps(cm.exception))), str(cm.exception))
            if HAVE_ALARM:
                self._assert_clean()

    def test_a_picklable_error_keeps_its_type(self):
        def reset(url, dest):
            raise ConnectionResetError(104, "Connection reset by peer")
        m.urllib.request.urlretrieve = reset
        with self.assertRaises(ConnectionResetError):
            m.stealth_urlretrieve({"url": "u"}, "d", "f")

    def test_a_failed_download_does_not_hang_the_pool(self):
        # vsdownload.downloadPackages: apply_async, then task.get(). Before the
        # fix the result thread died unpickling the error and get() never returned.
        with multiprocessing.Pool(1) as pool:
            task = pool.apply_async(_download_in_worker)
            with self.assertRaises(OSError) as cm:
                task.get(timeout=60)
        self.assertTrue(str(cm.exception).startswith("ContentTooShortError: "))
        self.assertIn("got only 6291456 out of 15860356 bytes", str(cm.exception))

    def _serve(self):
        _CutEdge.ranges = []
        server = http.server.HTTPServer(("127.0.0.1", 0), _CutEdge)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        return "http://127.0.0.1:%d/pkg.vsix" % server.server_address[1]

    def test_a_cut_file_is_resumed_from_where_it_stopped(self):
        # Run 41302586484: five fetches from the start, five cuts at 4 MiB.
        url = self._serve()
        waits = []
        with tempfile.TemporaryDirectory() as d:
            dest = os.path.join(d, "pkg.vsix")
            m.stealth_urlretrieve({"url": url}, dest, "pkg.vsix", wait=waits.append)
            with open(dest, "rb") as f:
                self.assertEqual(f.read(), FILE)
        self.assertEqual(_CutEdge.ranges, [None, "bytes=4096-"])
        self.assertEqual(waits, [5])
        if HAVE_ALARM:
            self._assert_clean()

    def test_a_server_that_ignores_the_range_sends_the_whole_file(self):
        class Full:
            status = 200
            headers = {"Content-Length": str(len(FILE))}

            def __init__(self):
                self.left = FILE

            def read(self, n):
                block, self.left = self.left[:n], self.left[n:]
                return block

            def __enter__(self):
                return self

            def __exit__(self, *a):
                return False

        def cut(url, dest):
            with open(dest, "wb") as f:
                f.write(FILE[:100])
            raise urllib.error.ContentTooShortError("retrieval incomplete", b"")
        m.urllib.request.urlretrieve = cut
        m.urllib.request.urlopen = lambda request: Full()
        with tempfile.TemporaryDirectory() as d:
            dest = os.path.join(d, "f")
            m.stealth_urlretrieve({"url": "http://cdn.invalid/f"}, dest, "f", wait=lambda s: None)
            with open(dest, "rb") as f:
                self.assertEqual(f.read(), FILE)

    def test_a_file_still_cut_after_every_resume_fails_as_an_oserror(self):
        def cut(url, dest):
            with open(dest, "wb") as f:
                f.write(FILE[:100])
            raise urllib.error.ContentTooShortError("retrieval incomplete", b"")
        requests = []

        def still_cut(request):
            requests.append(request.get_header("Range"))
            raise urllib.error.ContentTooShortError("still cut", b"")
        m.urllib.request.urlretrieve = cut
        m.urllib.request.urlopen = still_cut
        waits = []
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaises(OSError) as cm:
                m.stealth_urlretrieve({"url": "http://cdn.invalid/f"}, os.path.join(d, "f"), "f",
                                      wait=waits.append)
        self.assertIs(type(cm.exception), OSError)
        self.assertTrue(str(cm.exception).startswith("ContentTooShortError: "))
        self.assertEqual(waits, list(m.RESUME_WAITS))
        self.assertEqual(requests, ["bytes=100-"] * len(m.RESUME_WAITS))
        if HAVE_ALARM:
            self._assert_clean()

    @unittest.skipUnless(HAVE_ALARM, "SIGALRM is Unix-only")
    def test_no_alarm_runs_during_a_wait(self):
        def cut(url, dest):
            with open(dest, "wb") as f:
                f.write(FILE[:100])
            raise urllib.error.ContentTooShortError("retrieval incomplete", b"")
        armed = []

        def wait(seconds):
            armed.append(signal.getitimer(signal.ITIMER_REAL)[0])

        def still_cut(request):
            raise urllib.error.ContentTooShortError("still cut", b"")
        m.urllib.request.urlretrieve = cut
        m.urllib.request.urlopen = still_cut
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaises(OSError):
                m.stealth_urlretrieve({"url": "http://cdn.invalid/f"}, os.path.join(d, "f"), "f", wait=wait)
        self.assertEqual(armed, [0] * len(m.RESUME_WAITS))
        self._assert_clean()

    def test_hook_presence_guard(self):
        with open(os.path.join(VSDIR, "vsdownload.py"), encoding="utf-8") as f:
            text = f.read()
        self.assertEqual(text.count("stealth_urlretrieve(payload, destname, fileid)"), 1)
        self.assertNotIn("signal.", text)
        self.assertNotIn('urllib.request.urlretrieve(payload["url"]', text)


if __name__ == "__main__":
    unittest.main()
