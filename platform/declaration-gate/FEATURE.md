# platform/declaration-gate

Category: spoof
Prefs: zoom.stealth.gfx.feature_status (and the gate's list of required declarations in StealthDeclarationGate.cpp)
Test: gfx/thebes/gtest/TestStealthGfxFeatureStatus.cpp (StealthGfxFeatureStatus.TheMeasuredRetailDeclarationsParse, EveryBrokenRuleIsRefused, WebglOpenglGatesTheWebglPathOfThisPlatform, EveryLiveFeatureAnswersFromTheDeclaration, TheDownloadedBlocklistLeavesTheProfileAlone, NoSuggestedDriverVersionWhileDeclared); gtest filter StealthGfxFeatureStatus.*

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
