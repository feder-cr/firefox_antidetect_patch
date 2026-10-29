# platform/untrusted-modules

Category: other
Prefs: none read directly
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `toolkit/xre/dllservices/UntrustedModulesData.cpp` (ModuleRecord::GetVersionAndVendorInfo; ModuleRecord::IsInDirectoryOf (new); IsOwnUnsignedBinary (new), movable): Performance fix (commit 26e54ca1b1): the Untrusted Modules processor skips the Authenticode lookup for this build's own unsigned binaries (beside firefox.exe and with its version), which always fell through to the catalog search: 1.26 s of CPU per launch. Classification unchanged (FirefoxDirectory + version). IsOwnUnsignedBinary is inline logic that can move to an own file.
- `toolkit/xre/dllservices/UntrustedModulesData.h` (ModuleRecord::IsInDirectoryOf (new), already-hook): Performance fix (commit 26e54ca1b1): the Untrusted Modules processor skips the Authenticode lookup for this build's own unsigned binaries (beside firefox.exe and with its version), which always fell through to the catalog search: 1.26 s of CPU per launch. Classification unchanged (FirefoxDirectory + version). IsOwnUnsignedBinary is inline logic that can move to an own file.
- `toolkit/xre/dllservices/tests/gtest/moz.build` (UNIFIED_SOURCES, already-hook): Performance fix (commit 26e54ca1b1): the Untrusted Modules processor skips the Authenticode lookup for this build's own unsigned binaries (beside firefox.exe and with its version), which always fell through to the catalog search: 1.26 s of CPU per launch. Classification unchanged (FirefoxDirectory + version). IsOwnUnsignedBinary is inline logic that can move to an own file.

## Our own files

- `toolkit/xre/dllservices/tests/gtest/TestStealthOwnBinaries.cpp`
