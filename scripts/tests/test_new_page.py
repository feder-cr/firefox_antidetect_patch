#!/usr/bin/env python3
"""Smoke test 2/4 — ctx.new_page() completes without hanging.

Verifies the C6 juggler-navigation observer notifications are intact.  A break
here (typically a 30s timeout then TargetClosedError) means
juggler-navigation-started-renderer or -browser is not firing from
nsDocShell.cpp / CanonicalBrowsingContext.cpp, so Page.ready never reaches
Playwright's ffPage.js.
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

from invisible_playwright.async_api import async_playwright  # noqa: E402


async def main() -> int:
    binary = os.environ.get("STEALTHFOX_BINARY")
    if not binary or not Path(binary).exists():
        print("[FAIL] STEALTHFOX_BINARY missing or invalid", file=sys.stderr)
        return 2

    async with async_playwright() as p:
        browser = await p.firefox.launch(executable_path=binary, headless=False)
        try:
            ctx = await browser.new_context()
            page = await asyncio.wait_for(ctx.new_page(), timeout=10)
            await page.goto("about:blank", timeout=5000)
            title = await page.title()
            print(f"[PASS] new_page + goto (title={title!r})")
            return 0
        except asyncio.TimeoutError:
            print("[FAIL] new_page() hung 10s — Page.ready never fired",
                  file=sys.stderr)
            return 1
        except Exception as e:
            print(f"[FAIL] new_page: {type(e).__name__}: {e}", file=sys.stderr)
            return 1
        finally:
            await browser.close()


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
