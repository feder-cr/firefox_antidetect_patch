#!/usr/bin/env python3
"""Smoke test 3/4 — page.mouse.{move,down,up,click,wheel} all work.

Verifies the C++ jugglerSendMouseEvent re-land is intact.  A break here
means the mouse event dispatch in WindowUtils / juggler-mouse-event-hit-renderer
observer chain is broken; commonly from rebases that touch dom/base/.
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
            page = await ctx.new_page()
            await page.goto("https://example.com", timeout=15000)
            ops = [
                ("move",  lambda: page.mouse.move(100, 100)),
                ("down",  lambda: page.mouse.down()),
                ("up",    lambda: page.mouse.up()),
                ("click", lambda: page.mouse.click(200, 200)),
                ("wheel", lambda: page.mouse.wheel(0, 200)),
            ]
            for name, op in ops:
                try:
                    await op()
                except Exception as e:
                    print(f"[FAIL] mouse.{name}: {type(e).__name__}: {e}",
                          file=sys.stderr)
                    return 1
            print(f"[PASS] mouse.{{{','.join(n for n,_ in ops)}}} all OK")
            return 0
        finally:
            await browser.close()


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
