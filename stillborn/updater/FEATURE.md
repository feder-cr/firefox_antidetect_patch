# stillborn/updater

Category: stillborn
Prefs: none read directly
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `build/moz.configure/update-programs.configure` (top-level configure options: updater_default, --enable-updater, --enable-unverified-updates, mac elevated-update requirements, MAR channel IDs, --enable-nss-mar, maintenance service, update agent, zucchini/bspatch block (all deleted), hard): Deletes the upstream update machinery (updater, MAR signature checks and channel IDs, maintenance service, update agent, bspatch). Hookable=hard: nothing is left to hook, and every upstream edit to these lines will conflict on rebase. The header comment says this was done on owner instruction and deliberately NOT as a --disable-updater switch. --enable-update-channel is kept: CONFIRMED, it lives i
