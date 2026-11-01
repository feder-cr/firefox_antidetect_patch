# platform/declaration-gate

Category: spoof
Prefs: zoom.stealth.gfx.feature_status (and the gate's list of required declarations in StealthDeclarationGate.cpp)
Test: gfx/thebes/gtest/TestStealthGfxFeatureStatus.cpp (StealthGfxFeatureStatus.TheMeasuredRetailDeclarationsParse, EveryBrokenRuleIsRefused, WebglOpenglGatesTheWebglPathOfThisPlatform, EveryLiveFeatureAnswersFromTheDeclaration, TheDownloadedBlocklistLeavesTheProfileAlone, NoSuggestedDriverVersionWhileDeclared); gtest filter StealthGfxFeatureStatus.* gfx/thebes/gtest/TestStealthScaleDeclaration.cpp (StealthScaleDeclaration.TheScaleTheDeclarationWasMadeAt, StealthScaleDeclaration.AnyOtherScaleIsAnIdentityNoMachineHas).

## What it does

The gate refuses to run the engine when a declaration it needs is missing, so
nothing is served from a compiled default or from the host
(StealthAssertDeclarationsComplete, StealthRefuse). It also holds the graphics
feature decisions: nsIGfxInfo::GetFeatureStatus answers every live feature
from zoom.stealth.gfx.feature_status, the statuses retail Firefox 159 on
Windows 11 gives the persona's real GPU, measured by invisible_core. Mozilla
decided them from the host's adapter, so a Windows VM or a GPU-less host lost
WebGL2, accelerated ANGLE and WebGPU. The parent and the RDD/utility processes
parse the declaration once and refuse when it is missing or malformed (every
live feature of widget/GfxInfoFeatureDefs.inc exactly once, a status retail
gives, the failure id retail gives); content and GPU processes answer from the
parent's cache. The downloaded blocklist is not evaluated while declared (it
wrote host-derived gfx.blacklist.* prefs into the profile), gfx.blacklist.*
prefs no longer outrank the declaration, and the suggested driver version is
empty. On GTK, WEBGL_OPENGL answers the declared WEBGL entry when the GL probe
succeeded: there it gates the only WebGL path, on Windows a path retail never
takes. The gate also requires the WebGL renderer and vendor strings.

## One scale for the window geometry (2026-11-02)

The screen is declared in CSS pixels, the work area and the window frame in
device pixels, and the page reads every CSS value through
layout.css.devPixelsPerPx. StealthAssertDeclarationsComplete refuses a scale
that is not the one the declaration was made at (screen width x scale must be
the device work-area width, within one device pixel) and a missing one (-1, the
host's own). Run 41325657085 forced 1.5 over a 100% persona and read innerWidth
1920 inside an outerWidth of 1291. The DPR is pinned in the persona
(screen.dpr), never forced through the pref.

## Hooks in Mozilla files

- `widget/GfxInfoBase.cpp` (after #include "GfxInfoBase.h"; GetFeatureStatus right after the retired-feature check; the top of GetFeatureSuggestedDriverVersion; the top of EvaluateDownloadedBlocklist, already-hook): every graphics feature decision answers from the declared retail statuses (StealthGfxFeatureStatus.cpp).

## Our own files

- `gfx/2d/StealthRefuse.h`
- `gfx/thebes/StealthDeclarationGate.cpp`
- `gfx/thebes/StealthDeclarationGate.h`
- `gfx/thebes/StealthDeclarationGate.mozbuild`
- `gfx/thebes/StealthGfxFeatureStatus.cpp`
- `gfx/thebes/StealthGfxFeatureStatus.h`
- `gfx/thebes/gtest/moz.build`
- `gfx/thebes/gtest/TestStealthGfxFeatureStatus.cpp`
