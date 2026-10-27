"""Where every change of ours lives in the engine layout.

The layout is one folder per feature, grouped by what the feature does:

    platform/     the build, the branding, the repo's own tooling
    stillborn/    Firefox services we kill at birth (updater, telemetry, ...)
    network/      proxies, SOCKS, DNS
    automation/   what the Juggler protocol needs from Gecko
    fingerprint/  what a page can observe, one folder per surface
    port/         fixes tied to one Firefox version, each with its reason to expire

GROUP_ORDER is also the order the series is applied in.

FEATURES maps the feature names of the 157 inventory (tools/inventory-157.json, one
record per Mozilla file we modify) to their folder. OWN_FILES maps the files we add
to Firefox's tree, by path, to the folder that owns them.
"""
from __future__ import annotations

import re

GROUP_ORDER = ["platform", "stillborn", "network", "automation", "fingerprint", "port"]

FEATURES = {
    # platform
    "release-output-ignore": "platform/repo",
    "agent-instructions-fork": "platform/repo",
    "readme-fork-disclosure": "platform/repo",
    "anonymous-launch-counter": "platform/launch-counter",
    "hidden-desktop-window-capture": "platform/hidden-desktop",
    "script-cache-adopt-written": "platform/script-cache",
    "stealth-prefs-content-sync": "platform/prefs",
    "firefox-js-stealth-defaults": "platform/prefs",
    "stealth-pref-declarations": "platform/prefs",
    "stealth-pref-defaults": "platform/prefs",
    "vsdownload-download-watchdog": "platform/build-tools",
    "win-long-build-paths": "platform/build-tools",
    # stillborn
    "updater-removed": "stillborn/updater",
    "pingsender-session-route": "stillborn/telemetry",
    "api-keys-declared": "stillborn/api-keys",
    # network
    "proxy-udp-egress": "network/proxy-udp",
    "socks-udp-h3": "network/socks-udp-h3",
    "socks-credentials": "network/socks",
    "dns-no-local-resolution": "network/dns",
    # automation
    "juggler-build": "automation/juggler",
    "juggler-prefs": "automation/juggler",
    "bypass-csp": "automation/bypass-csp",
    "closed-shadowroot-automation-access": "automation/shadow-root",
    "shadow-root-sandbox-access": "automation/shadow-root",
    "file-chooser-intercept": "automation/file-chooser",
    "hybrid-xrays": "automation/hybrid-xrays",
    "juggler-docshell-attrs": "automation/docshell-attrs",
    "juggler-download-interceptor": "automation/downloads",
    "juggler-drag-gesture": "automation/input",
    "juggler-element-atoms": "automation/element-atoms",
    "juggler-enterprise-policies": "automation/enterprise-policies",
    "juggler-event-id": "automation/input",
    "juggler-mouse-event-id": "automation/input",
    "juggler-input-dispatch": "automation/input",
    "juggler-param-buffer": "automation/input",
    "juggler-forced-offline": "automation/forced-offline",
    "juggler-ignore-https-errors": "automation/ignore-https-errors",
    "juggler-interception": "automation/network-interception",
    "juggler-service-worker-intercept": "automation/network-interception",
    "juggler-link-click-events": "automation/navigation",
    "juggler-navigation-events": "automation/navigation",
    "juggler-load-identifier": "automation/navigation",
    "juggler-pipe-handles": "automation/pipe",
    "juggler-pipe-stdio": "automation/pipe",
    "juggler-popup-explicit-size": "automation/window-size",
    # fingerprint: fonts
    "bundled-fonts-macos": "fingerprint/fonts/bundle",
    "font-bundle-files": "fingerprint/fonts/bundle",
    "font-bundle-borrow": "fingerprint/fonts/bundle",
    "font-bundle-borrowed-bytes": "fingerprint/fonts/bundle",
    "font-bundle-file-path": "fingerprint/fonts/bundle",
    "font-ttc-face-index": "fingerprint/fonts/bundle",
    "font-cmap-publish": "fingerprint/fonts/bundle",
    "hb-face-lifetime": "fingerprint/fonts/bundle",
    "font-fallback-bundle": "fingerprint/fonts/bundle",
    "font-list-bundle-only": "fingerprint/fonts/font-list",
    "font-design-metrics": "fingerprint/fonts/metrics",
    "font-metrics-declared": "fingerprint/fonts/metrics",
    "glyph-coverage-quantise": "fingerprint/fonts/metrics",
    "cleartype-declared": "fingerprint/fonts/metrics",
    "font-loading-api-inert": "fingerprint/fonts/font-loading",
    # fingerprint: canvas, webgl
    "canvas-noise-projection": "fingerprint/canvas/noise-projection",
    "canvas-as-drawn-readers": "fingerprint/canvas/as-drawn",
    "canvas-snapshot-readback": "fingerprint/canvas/as-drawn",
    "canvas-2d-text-metrics": "fingerprint/canvas/text-metrics",
    "webgl-parameters": "fingerprint/webgl/parameters",
    "webgl-shader-output-language": "fingerprint/webgl/shader-language",
    # fingerprint: audio, media
    "audio-context-values": "fingerprint/audio/context-values",
    "audio-fingerprint-noise": "fingerprint/audio/noise",
    "audio-sample-rate": "fingerprint/audio/sample-rate",
    "media-decode-declaration": "fingerprint/media/decode-declaration",
    "media-codecs-declared": "fingerprint/media/decode-declaration",
    "media-enumeration": "fingerprint/media/enumeration",
    "speech-voices-declared": "fingerprint/media/speech-voices",
    # fingerprint: webrtc
    "webrtc-ice-candidates": "fingerprint/webrtc/ice-candidates",
    "webrtc-stealth-bridge": "fingerprint/webrtc/ice-candidates",
    # fingerprint: screen and window
    "screen-metrics": "fingerprint/screen-window/screen",
    "window-geometry": "fingerprint/screen-window/geometry",
    "popup-default-position": "fingerprint/screen-window/popup-position",
    "css-media-features": "fingerprint/screen-window/css-media",
    "occlusion-tracking": "fingerprint/screen-window/occlusion",
    "focus-ignore-os-deactivation": "fingerprint/screen-window/focus",
    # fingerprint: locale and time
    "session-declaration": "fingerprint/locale-time/session-declaration",
    "session-language-declared": "fingerprint/locale-time/language",
    "language-declared": "fingerprint/locale-time/language",
    "accept-language-override-list": "fingerprint/locale-time/accept-language",
    "timezone-declared": "fingerprint/locale-time/timezone",
    "timezone-locale-override": "fingerprint/locale-time/timezone",
    "xslt-date-timezone": "fingerprint/locale-time/timezone",
    "worker-locale-timezone": "fingerprint/locale-time/workers",
    # fingerprint: hardware
    "hardware-profile": "fingerprint/hardware/profile",
    "navigator-hardware-concurrency": "fingerprint/hardware/concurrency",
    "storage-quota-estimate": "fingerprint/hardware/storage-quota",
    # fingerprint: input
    "input-event-birth": "fingerprint/input/event-birth",
    "synthetic-input-latency": "fingerprint/input/latency",
    # fingerprint: what scripts can read about the engine
    "stealth-branding-redirect": "fingerprint/js-surface/branding",
    "debugger-invisible-attach": "fingerprint/js-surface/debugger",
    "devtools-attach-suppressed": "fingerprint/js-surface/debugger",
    "stack-hide-juggler-frames": "fingerprint/js-surface/stack-frames",
    "eval-introduction-name": "fingerprint/js-surface/eval-name",
    "blob-native-endings": "fingerprint/js-surface/line-endings",
    "plaintext-linebreak": "fingerprint/js-surface/line-endings",
}

# Files we add to Firefox's tree, first match wins.
OWN_FILES = [
    (r"^juggler/", "automation/juggler"),
    (r"^browser/fonts/", "fingerprint/fonts/bundle"),
    (r"^gfx/thebes/StealthBundleFontList\.", "fingerprint/fonts/bundle"),
    (r"^gfx/thebes/StealthDeclarationGate\.", "platform/declaration-gate"),
    (r"^gfx/2d/StealthRefuse\.h$", "platform/declaration-gate"),
    (r"^gfx/2d/StealthCanvasProjection\.", "fingerprint/canvas/noise-projection"),
    (r"^gfx/tests/gtest/TestStealthCanvasProjection\.cpp$", "fingerprint/canvas/noise-projection"),
    (r"^gfx/2d/StealthGlyphCoverage\.", "fingerprint/fonts/metrics"),
    (r"^gfx/tests/gtest/TestStealthTextQuality\.cpp$", "fingerprint/fonts/metrics"),
    (r"^dom/base/StealthSessionDeclaration\.", "fingerprint/locale-time/session-declaration"),
    (r"^intl/locale/StealthSessionPrefs\.", "fingerprint/locale-time/session-declaration"),
    (r"^dom/media/StealthMediaDeclaration\.", "fingerprint/media/decode-declaration"),
    (r"^dom/media/webrtc/transport/nr_stealth_bridge\.", "fingerprint/webrtc/ice-candidates"),
    (r"^third_party/libwebrtc/api/location\.h$", "fingerprint/webrtc/ice-candidates"),
    (r"^netwerk/protocol/res/StealthBranding\.", "fingerprint/js-surface/branding"),
    (r"^netwerk/socket/nsSOCKSUDPIOLayer\.", "network/socks-udp-h3"),
    (r"^browser/config/", "platform/release"),
    (r"^\.mozconfig$", "platform/build-config"),
    (r"^STEALTH_BRANCH_README\.md$", "platform/repo"),
    (r"^scripts/", "platform/ci"),
    (r"^\.github/", "platform/ci"),
]


def own_file_folder(path: str) -> str:
    for pattern, folder in OWN_FILES:
        if re.search(pattern, path):
            return folder
    raise KeyError("no folder owns our file %s: add it to OWN_FILES" % path)


def feature_folder(feature: str) -> str:
    try:
        return FEATURES[feature]
    except KeyError:
        raise KeyError("feature %r has no folder: add it to FEATURES" % feature) from None


def folder_sort_key(folder: str):
    group = folder.split("/")[0]
    return (GROUP_ORDER.index(group), folder)
