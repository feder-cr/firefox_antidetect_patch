#!/usr/bin/env python3
"""Smoke test 1/4 — Firefox launches under Playwright.

Verifies the C5 juggler-pipe handoff (LauncherProcessWin → nsWindowsWMain →
nsRemoteDebuggingPipe) is intact.  A break here means the launcher is not
forwarding inherited stdio FDs 3/4 to the browser child, or PW_PIPE_READ/WRITE
env vars are not being published.

Usage:
  STEALTHFOX_PYTHONPATH=<path-to-invisible_playwright> \\
  STEALTHFOX_BINARY=<path-to-firefox.exe> \\
  python scripts/tests/test_launch.py
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
    if not binary:
        print("[FAIL] STEALTHFOX_BINARY env var not set", file=sys.stderr)
        return 2
    if not Path(binary).exists():
        print(f"[FAIL] Binary not found at {binary}", file=sys.stderr)
        return 2

    async with async_playwright() as p:
        try:
            browser = await p.firefox.launch(
                executable_path=binary,
                headless=False,
            )
            print("[PASS] Browser launched (pipe stayed connected)")
            await browser.close()
            return 0
        except Exception as e:
            print(f"[FAIL] launch: {type(e).__name__}: {e}", file=sys.stderr)
            return 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
