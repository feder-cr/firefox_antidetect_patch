# stillborn/updater

Category: stillborn
Prefs: none read directly
Test: TODO

## What it does

Upstream, `build/moz.configure/update-programs.configure` declares the whole
application-update machinery. In this tree it declares none of it: the
updater, the MAR signature checks, the MAR channel identifiers, the
maintenance service, the update agent and the binary patch formats were
removed on 2026-08-31 on the owner's instruction ("tutto quello che riguarda
aggiornamenti"), and explicitly NOT as a `--disable-updater` lever, so that
nothing is left to switch back on.

The consequence is deliberate: `MOZ_UPDATER` is never set, so
`toolkit/mozapps/update` is not built, `AppConstants.MOZ_UPDATER` is false,
every `#ifdef MOZ_UPDATER` compiles out, no updater binary is packaged, and
the "Update available" badge that UpdateListener shows has no code left to
show it.

What still reaches aus5.mozilla.org (measured 2026-10-29, idle session,
host-level proxy log): the GMP plugin check (`/update/3/GMP/...`), exactly
as in official Firefox. What does not: the application update check
(`/update/6/...`), which official Firefox 157 makes at startup. That is a
network-shape difference and an open decision: the owner's rule of
2026-10-29 is that the requests a real Firefox makes are made by us too and
only installing an update is refused, which would mean bringing the check
back without the updater.

What did NOT go, because it is not update machinery: `--enable-update-channel`
(`MOZ_UPDATE_CHANNEL`). It names the CHANNEL, and RemotingName is derived
from it (`firefox` on release, `firefox-default` without it), so removing it
would rename the application on disk and on the wire. Two options upstream
keeps in this file are unrelated to updates and are kept: the default browser
agent and the notification server; since 157 also the macOS dmgInstallHelper
debug option.

## Hooks in Mozilla files

- `build/moz.configure/update-programs.configure` (top-level configure options: updater_default, --enable-updater, --enable-unverified-updates, mac elevated-update requirements, MAR channel IDs, --enable-nss-mar, maintenance service, update agent, zucchini/bspatch block (all deleted), hard): Deletes the upstream update machinery (updater, MAR signature checks and channel IDs, maintenance service, update agent, bspatch). Hookable=hard: nothing is left to hook, and every upstream edit to these lines will conflict on rebase. The header comment says this was done on owner instruction and deliberately NOT as a --disable-updater switch. --enable-update-channel is kept: CONFIRMED, it lives i
