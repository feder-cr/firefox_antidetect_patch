# fingerprint/hardware/storage-quota

Category: spoof
Prefs: zoom.stealth.storage.quota_mb
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `dom/quota/ActorsParent.cpp` (QuotaManager::GetUsageAndLimitForEstimate, already-hook): The same limit expression (stealthOverride ? quota*MiB : fallback) is written twice. A helper in our file would make both sites one line each. A block of about 20 comment lines holds a 2026-08-09 correction and should move out. The pinned value replaces both the locked-origin limit and the group limit.
