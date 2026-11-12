# fingerprint/geolocation/no-host-provider

Category: spoof
Prefs: geo.provider.network.url (declared empty once, the hook is on Mozilla's own line in all.js), geo.provider.network.scan (declared false once in StealthPrefs.js)
Test: TestStealthGeolocation.* (toolkit/components/geolocation/test/gtest, in the suite StealthDeclaredNetwork); the node tests of juggler/tests:
geolocation_windows_provider_never_selected, geolocation_no_wifi_scan, geolocation_system_switch_declared_on,
geolocation_network_url_is_empty_by_default, geolocation_override_timestamp_is_delivery_time

## What it does

The position a page reads comes only from the core's declaration (the geolocation override). The engine never asks the host for a
location, a Wi-Fi scan or a privacy switch.

- No host provider. `GeolocationService::Init()` calls `StealthSelectGeolocationProvider` (toolkit/components/geolocation/
  StealthGeolocation.cpp) after the shutdown observer and before Mozilla picks a provider: the provider is the network one
  (`NetworkGeolocationProvider.sys.mjs`) in every process, so Portal, GeoClue, CoreLocation, Android and the MLS provider a pref could
  ask for are never created. The Windows location provider is out of the tree: `dom/system/windows/location/` is deleted, its
  messages leave `PWindowsUtils.ipdl` (the protocol stays, with no message), `WindowsUtilsChild` no longer makes its actors and the
  two `moz.build` stop building it. It started the WINDOWS_UTILS utility process and loaded the WinRT geolocation modules.
- No Wi-Fi scan. `geo.provider.network.scan` is declared false once: its fallback is true and would start the Wi-Fi monitor and
  load wlanapi.dll in the parent.
- No service URL. `geo.provider.network.url` is "" on Mozilla's own line in all.js; on Windows Mozilla declares nothing, so the pref
  has no value. `NetworkGeolocationProvider` finds no URL and answers POSITION_UNAVAILABLE without asking anyone.
- The OS location switch is declared on and never read. `GetGeolocationPermissionBehavior` (GeolocationSystemWin.cpp) and the
  permission monitor of `PermissionObserver.cpp` return before `AppCapability("location")` is created
  (`StealthSystemLocationDeclaredOn`): no capabilityaccessmanagerclient.dll, and a switch off on the host never cancels a request.
- The override's timestamp is the time of each delivery (a getter in juggler/content/main.js, owned by automation/juggler): the page
  reads `position.timestamp - Date.now()` between -2 and 0 ms, never a day ahead.

## Hooks in Mozilla files

- `toolkit/components/geolocation/GeolocationService.cpp`: one include and a call with an early return in `Init()`; the Windows
  block of Mozilla stays intact beside it. `toolkit/components/geolocation/moz.build`: our source and the gtest directory.
- `dom/geolocation/GeolocationSystemWin.cpp`, `dom/permission/PermissionObserver.cpp`: an early return before Mozilla's body.
- `modules/libpref/init/all.js` (owned by platform/prefs): the empty default of `geo.provider.network.url`.
- Deleted Mozilla files: `dom/system/windows/location/*` (11 files). Changed: `dom/system/windows/{PWindowsUtils.ipdl,
  WindowsUtilsChild.cpp,WindowsUtilsChild.h,moz.build}`. They are Mozilla lines removed (the removed budget).

## Our own files

- `toolkit/components/geolocation/StealthGeolocation.{h,cpp}`, `toolkit/components/geolocation/test/gtest/{TestStealthGeolocation.cpp,
  moz.build}`, and the node tests above.
