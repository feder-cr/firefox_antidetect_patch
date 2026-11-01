# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

# Per-file download watchdog for vsdownload.py (cross-compile build tooling).
#
# socket.setdefaulttimeout only catches a fully stalled connection, not a slow
# trickle from a flaky CDN edge: the Win11 SDK "X64 Debuggers And Tools" MSI
# dripped for hours without ever erroring, so the 5-attempt retry loop in
# _downloadPayload never retried and the cross-compile build hung. This caps the
# total time of each file and raises on overrun, so the loop reconnects (fresh
# CDN edge) instead of hanging.
#
# signal.SIGALRM is Unix-only (the watchdog is for the Linux cross-build). On
# native Windows it does not exist, so the alarm is skipped and the only limit is
# the socket default timeout. That timeout is set by vsdownload.py only when it
# runs as a script (its `__main__` block, socket.setdefaulttimeout(15)); it is
# NOT set when the module is imported, as taskcluster/scripts/misc/get_vs.py
# does, so on Windows through get_vs.py there is no time limit at all.
#
# It runs in a worker of vsdownload's multiprocessing.Pool, so whatever it
# raises is pickled back to the parent. urllib's ContentTooShortError and
# HTTPError cannot be unpickled (their __init__ takes arguments the unpickler
# does not pass): the pool's result thread died on one and task.get() waited
# for ever. Run 41205185526: a CDN edge served the same SDK cab cut at 6 MiB
# five times, and the Windows leg idled until the 300-minute timeout. Such an
# error comes back as an OSError with the same text, so the build fails in
# seconds instead.
#
# A cut file is resumed, not fetched again. Run 41302586484: a CDN edge served
# a VS vsix cut at exactly 4 MiB, and vsdownload's loop asked again five times
# in under a second, from the start, from the same edge: five identical cuts and
# a dead build. The edge has the head of the file and not the rest, so the rest
# is asked for (HTTP Range from the size on disk), after a wait, which the edge
# has to fetch from the origin. A server that ignores the range sends the whole
# file again (200), which is written from the start.

import os
import pickle
import re
import signal
import time
import urllib.error
import urllib.request

RESUME_WAITS = (5, 30, 90)


def _resume(url, destname, fileid, cut, alarm, wait):
    """Completes destname after urlretrieve stopped short of it: one ranged
    request per entry of RESUME_WAITS, each after its wait. Raises the last
    error when the file is still not whole."""
    for delay in RESUME_WAITS:
        have = os.path.getsize(destname) if os.path.exists(destname) else 0
        print("%s cut at %d bytes, resuming in %ds" % (fileid, have, delay),
              flush=True)
        alarm(False)  # the wait is not a download: no alarm runs during it
        wait(delay)
        alarm(True)
        request = urllib.request.Request(url, headers={"Range": "bytes=%d-" % have})
        try:
            with urllib.request.urlopen(request) as response:
                if response.status == 206:
                    m = re.match(r"bytes (\d+)-\d+/(\d+)",
                                 response.headers.get("Content-Range", ""))
                    if not m or int(m.group(1)) != have:
                        os.remove(destname)
                        raise urllib.error.ContentTooShortError(
                            "resume of %s answered %r, not from byte %d"
                            % (fileid, response.headers.get("Content-Range"), have),
                            b"")
                    total, mode = int(m.group(2)), "ab"
                else:
                    length = response.headers.get("Content-Length")
                    total, mode = (int(length) if length else None), "wb"
                with open(destname, mode) as f:
                    while True:
                        block = response.read(1 << 16)
                        if not block:
                            break
                        f.write(block)
            size = os.path.getsize(destname)
            if total is None or size == total:
                return
            cut = urllib.error.ContentTooShortError(
                "retrieval incomplete: got only %d out of %d bytes" % (size, total),
                b"")
        except OSError as e:
            cut = e
    raise cut


def stealth_urlretrieve(payload, destname, fileid, timeout=120, wait=time.sleep):
    """urllib.request.urlretrieve(payload["url"], destname) with a time limit
    of `timeout` seconds per network attempt where SIGALRM exists, and a cut
    file resumed (see above) before giving up.

    Raises TimeoutError on overrun (an OSError, so the caller's `except
    Exception` retry loop handles it), and an error that would not survive the
    trip back from the pool's worker as an OSError with its type and text. `payload` is the dict, not the url: the
    key is read after the alarm is armed, as before, so a missing "url" fails the
    same way and the alarm is still cancelled in `finally`.
    """

    def _dl_watchdog(signum, frame):
        raise TimeoutError("download exceeded %ds for %s" % (timeout, fileid))

    have_alarm = hasattr(signal, "SIGALRM")

    def alarm(armed):
        if have_alarm:
            signal.alarm(timeout if armed else 0)

    if have_alarm:
        prev_alarm = signal.signal(signal.SIGALRM, _dl_watchdog)
        signal.alarm(timeout)
    try:
        url = payload["url"]
        try:
            urllib.request.urlretrieve(url, destname)
        except urllib.error.ContentTooShortError as cut:
            _resume(url, destname, fileid, cut, alarm, wait)
    except Exception as e:
        try:
            pickle.loads(pickle.dumps(e))
        except Exception:
            raise OSError("%s: %s" % (type(e).__name__, e)) from None
        raise
    finally:
        if have_alarm:
            signal.alarm(0)
            signal.signal(signal.SIGALRM, prev_alarm)
