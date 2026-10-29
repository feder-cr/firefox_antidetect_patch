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

import signal
import urllib.request


def stealth_urlretrieve(payload, destname, fileid, timeout=120):
    """urllib.request.urlretrieve(payload["url"], destname) with a total time
    limit of `timeout` seconds where SIGALRM exists.

    Raises TimeoutError on overrun (an OSError, so the caller's `except
    Exception` retry loop handles it). `payload` is the dict, not the url: the
    key is read after the alarm is armed, as before, so a missing "url" fails the
    same way and the alarm is still cancelled in `finally`.
    """

    def _dl_watchdog(signum, frame):
        raise TimeoutError("download exceeded %ds for %s" % (timeout, fileid))

    have_alarm = hasattr(signal, "SIGALRM")
    if have_alarm:
        prev_alarm = signal.signal(signal.SIGALRM, _dl_watchdog)
        signal.alarm(timeout)
    try:
        urllib.request.urlretrieve(payload["url"], destname)
    finally:
        if have_alarm:
            signal.alarm(0)
            signal.signal(signal.SIGALRM, prev_alarm)
