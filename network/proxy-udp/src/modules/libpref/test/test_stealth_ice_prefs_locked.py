# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

"""The two prefs that decide WHERE a PeerConnection's ICE runs are locked to the retail value, once, at startup.

The SOCKS5 UDP layer is attached in one place, NrSocket::create, in the process that owns nICEr's sockets: the socket process in a
retail build. media.peerconnection.mtransport_process=false (ICE in the content process) and network.process.enabled=false (no socket
process) put the sockets in classes with no layer. Behind a proxy a client that set either would run an ICE with no relay: no srflx, no
relay candidate, and nothing proves it cannot send direct. They are therefore locked after the user prefs are loaded
(toolkit/xre/nsAppRunner.cpp, next to mDirProvider.InitializeUserPrefs()): a locked pref reads its default whatever user.js, prefs.js or
a caller wrote.

Both prefs are declared by Mozilla and we do not change their value, so there is nothing to declare here: this test only asserts that
the lock is there, after the load of the user prefs, for both names, under the hook marker.

Run: python modules/libpref/test/test_stealth_ice_prefs_locked.py
"""

import os
import re
import sys

TREE = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
APPRUNNER = "toolkit/xre/nsAppRunner.cpp"
PREFS = ["network.process.enabled", "media.peerconnection.mtransport_process"]


def source():
    with open(os.path.join(TREE, APPRUNNER), encoding="utf-8") as f:
        return f.read()


def test_both_prefs_are_locked_after_the_user_prefs_are_loaded():
    text = source()
    load = text.index("mDirProvider.InitializeUserPrefs();")
    for name in PREFS:
        locks = [m.start() for m in re.finditer(r'Preferences::Lock\(\s*"%s"\s*\)' % re.escape(name), text)]
        assert len(locks) == 1, "%s must be locked exactly once in %s, found %d" % (name, APPRUNNER, len(locks))
        assert locks[0] > load, "%s is locked before the user prefs are loaded: a user value set later would not be seen as locked" % name
        assert locks[0] - load < 1500, "%s is locked far from the load of the user prefs" % name


def test_the_lock_sits_under_the_hook_marker():
    text = source()
    marker = text.index("STEALTH-HOOK(network/proxy-udp)", text.index("mDirProvider.InitializeUserPrefs();"))
    block = text[marker:marker + 1500]
    for name in PREFS:
        assert name in block, "%s is not in the block of the hook marker" % name


def test_nobody_declares_them_again():
    # Declared by Mozilla; a second declaration of ours would win by load order and hide the retail value (see
    # test_stealth_prefs_declared_once.py).
    for rel in ["modules/libpref/init/StealthPrefs.js", "modules/libpref/init/StealthPrefList.yaml"]:
        with open(os.path.join(TREE, rel), encoding="utf-8") as f:
            text = f.read()
        for name in PREFS:
            assert name not in text, "%s declares %s" % (rel, name)


if __name__ == "__main__":
    failed = 0
    for fn in [test_both_prefs_are_locked_after_the_user_prefs_are_loaded, test_the_lock_sits_under_the_hook_marker,
               test_nobody_declares_them_again]:
        try:
            fn()
            print("ok   ", fn.__name__)
        except AssertionError as e:
            failed += 1
            print("FAIL ", fn.__name__, "-", e)
    sys.exit(1 if failed else 0)
