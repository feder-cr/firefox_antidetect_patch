# platform/repo

Category: build, other
Prefs: none read directly
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `.gitignore` ((top-level ignore rules), already-hook): Ignores the local release output directory. Repo hygiene only; no Firefox code affected.
- `CLAUDE.md` ((top-level document), already-hook): Developer/agent instructions, not code; Mozilla's AGENTS.md is imported above it (line 1, unchanged). Points at workbench paths outside this repo (c:/src/firefox-stealth) and at a build mirror ~/ff-build/firefox-150 (a WSL path, name kept from 150); whether that mirror is stale is unverified (not checkable from this tree). Says branch stealth/157, which matches the current tree. Text is in Italian
- `README.md` ((top-level document), already-hook): Public disclosure of the anonymous launch counter (one GET per browser start, see BrowserGlue.sys.mjs and firefox.js). Inconsistency claim CONFIRMED: README.md:63 names the asset under feder-cr/invisible_firefox, browser/app/profile/firefox.js:3869 defaults the pref to feder-cr/firefox_antidetect_patch, browser/components/BrowserGlue.sys.mjs:426 falls back to feder-cr/invisible_playwright (that fa

## Our own files

- `STEALTH_BRANCH_README.md`
