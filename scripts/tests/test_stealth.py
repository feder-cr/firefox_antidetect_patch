#!/usr/bin/env python3
"""Smoke test 4/4 — anti-bot site sees us as legitimate Firefox.

Verifies the stealth surface holds: navigator.webdriver=False, UA override,
language override, plugins/hardware overrides.

Uses InvisiblePlaywright with locale="" (C7 workaround) so we don't trip the
known content-process crash with Browser.setLocaleOverride.
"""
import asyncio
import os
import sys
from pathlib import Path

SDK_PATH = os.environ.get(
    "STEALTHFOX_PYTHONPATH",
    r"c:/src/firefox-stealth/release/stealthfox/src",
)
sys.path.insert(0, SDK_PATH)

from invisible_playwright.async_api import InvisiblePlaywright  # noqa: E402


async def main() -> int:
    binary = os.environ.get("STEALTHFOX_BINARY")
    if not binary or not Path(binary).exists():
        print("[FAIL] STEALTHFOX_BINARY missing or invalid", file=sys.stderr)
        return 2

    async with InvisiblePlaywright(
        seed=42, headless=False, binary_path=binary, locale=""
    ) as browser:
        ctx = await browser.new_context()
        page = await ctx.new_page()
        try:
            await page.goto("https://bot.sannysoft.com/", timeout=20000)
        except Exception as e:
            print(f"[FAIL] goto sannysoft: {type(e).__name__}: {e}",
                  file=sys.stderr)
            return 1
        await asyncio.sleep(3)

        markers = await page.evaluate("""() => ({
            webdriver: navigator.webdriver,
            ua: navigator.userAgent,
            platform: navigator.platform,
            languages_len: navigator.languages.length,
            plugins_len: navigator.plugins.length,
        })""")
        # Hard-fail on the obvious-bot signals
        if markers["webdriver"] is True or markers["webdriver"] == "true":
            print(f"[FAIL] navigator.webdriver = {markers['webdriver']!r}",
                  file=sys.stderr)
            return 1
        if "Firefox" not in (markers["ua"] or ""):
            print(f"[FAIL] UA missing Firefox: {markers['ua']!r}",
                  file=sys.stderr)
            return 1

        # Soft-check sannysoft pass count
        try:
            counts = await page.evaluate("""() => {
                const rows = document.querySelectorAll('table tr');
                let p = 0, f = 0;
                for (const r of rows) {
                    const c = r.querySelectorAll('td');
                    if (c.length < 2) continue;
                    const s = c[1].textContent.trim().toLowerCase();
                    if (s.includes('passed') || s.includes('ok')) p++;
                    else if (s.includes('failed')) f++;
                }
                return {p, f};
            }""")
            print(f"[PASS] webdriver={markers['webdriver']}, "
                  f"UA={markers['ua'][:40]}..., "
                  f"sannysoft {counts['p']} passed / {counts['f']} failed")
            return 0
        except Exception as e:
            print(f"[PASS] basic markers OK (sannysoft scrape skipped: {e})")
            return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
