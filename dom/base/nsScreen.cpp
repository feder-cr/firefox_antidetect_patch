/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsScreen.h"
#include "StealthDeclarationGate.h"

#include "mozilla/GeckoBindings.h"
#include "mozilla/StaticPrefs_zoom.h"
#include "mozilla/dom/BrowsingContextBinding.h"
#include "mozilla/dom/Document.h"
#include "mozilla/dom/DocumentInlines.h"
#include "mozilla/widget/ScreenManager.h"
#include "nsCOMPtr.h"
#include "nsDeviceContext.h"
#include "nsGlobalWindowInner.h"
#include "nsGlobalWindowOuter.h"
#include "nsIDocShell.h"
#include "nsIDocShellTreeItem.h"
#include "nsLayoutUtils.h"
#include "nsPresContext.h"
#include "mozilla/StaticPrefs_zoom.h"

using namespace mozilla;
using namespace mozilla::dom;

nsScreen::nsScreen(nsPIDOMWindowInner* aWindow)
    : DOMEventTargetHelper(aWindow) {}

/* static */ already_AddRefed<nsScreen> nsScreen::Create(
    nsPIDOMWindowInner* aWindow) {
  RefPtr screen = new nsScreen(aWindow);
  screen->mScreenOrientation = ScreenOrientation::Create(aWindow, screen);
  return screen.forget();
}

nsScreen::~nsScreen() = default;

// QueryInterface implementation for nsScreen
NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(nsScreen)
NS_INTERFACE_MAP_END_INHERITING(DOMEventTargetHelper)

NS_IMPL_ADDREF_INHERITED(nsScreen, DOMEventTargetHelper)
NS_IMPL_RELEASE_INHERITED(nsScreen, DOMEventTargetHelper)

NS_IMPL_CYCLE_COLLECTION_INHERITED(nsScreen, DOMEventTargetHelper,
                                   mScreenOrientation)

int32_t nsScreen::PixelDepth() {
  // Stealth: the declared depth, before anything reads the real panel. This
  // used to fall through to nsDeviceContext::GetDepth() and report whatever
  // display the user actually has, which is a host value inside a persona that
  // claims to be a particular Windows machine.
  //
  // It survived every cross-OS comparison we ran, and the reason is worth
  // keeping: both development machines are 24-bit, so the field AGREED across
  // platforms and looked declared. Agreement is not declaration - an absence
  // of difference is not evidence of a shared source, and this is the field
  // that proved it. Found by the field-by-field audit
  // (19-field-declaration-audit.md), which asks for the source in the code
  // rather than for equality in the measurement.
  {
    const int32_t declared = StaticPrefs::zoom_stealth_screen_color_depth();
    if (declared > 0) {
      return declared;
    }
  }
  // Return 24 to prevent fingerprinting.
  if (ShouldResistFingerprinting(RFPTarget::ScreenPixelDepth)) {
    return 24;
  }
  nsDeviceContext* context = GetDeviceContext();
  if (NS_WARN_IF(!context)) {
    return 24;
  }
  return context->GetDepth();
}

nsPIDOMWindowOuter* nsScreen::GetOuter() const {
  if (nsPIDOMWindowInner* inner = GetOwnerWindow()) {
    return inner->GetOuterWindow();
  }
  return nullptr;
}

nsDeviceContext* nsScreen::GetDeviceContext() const {
  return nsLayoutUtils::GetDeviceContextForScreenInfo(GetOuter());
}

CSSIntRect nsScreen::GetRect() {
  // Stealth: spoof screen resolution from prefs set by Python at launch
  {
    int32_t w = mozilla::StaticPrefs::zoom_stealth_screen_width();
    int32_t h = mozilla::StaticPrefs::zoom_stealth_screen_height();
    if (w > 0 && h > 0) return {0, 0, w, h};
  }
  // Return window inner rect to prevent fingerprinting.
  if (ShouldResistFingerprinting(RFPTarget::ScreenRect)) {
    return GetTopWindowInnerRectForRFP();
  }

  // Here we manipulate the value of aRect to represent the screen size,
  // if there is an override set with WebDriver BiDi or in RDM.
  if (nsPIDOMWindowInner* owner = GetOwnerWindow()) {
    if (Document* doc = owner->GetExtantDoc()) {
      Maybe<CSSIntSize> deviceSize =
          nsGlobalWindowOuter::GetRDMDeviceSize(*doc);
      if (deviceSize.isSome()) {
        const CSSIntSize& size = deviceSize.value();
        return {0, 0, size.width, size.height};
      }
    }

    if (BrowsingContext* bc = owner->GetBrowsingContext()) {
      if (auto size = bc->GetScreenAreaOverride()) {
        return {{}, *size};
      }
    }
  }

  nsDeviceContext* context = GetDeviceContext();
  if (NS_WARN_IF(!context)) {
    return {};
  }
  return CSSIntRect::FromAppUnitsRounded(context->GetRect());
}

CSSIntRect nsScreen::GetAvailRect() {
  // Stealth: spoof screen avail rect from prefs set by Python at launch.
  //
  // The strip availHeight is short of height is the Windows taskbar, and its
  // height is DECLARED - by invisible_core, once, and by nothing else. It used
  // to be the literal 48 here and a second literal 48 in the generator's
  // Python, in two languages with nothing tying them together: a persona
  // needing a different one (auto-hide, a second monitor, a scaled display) had
  // to be changed in both, and nothing would have said so if only one moved.
  //
  // There is NO compiled floor under it any more (engine rule 7, 2026-08-09).
  // The "-1 keeps the 48 that was compiled in" that used to sit here was the
  // second source of truth the rule forbids: it made a browser launched with a
  // half-written profile look like it was working. A missing declaration now
  // refuses, in StealthDeclarationGate, on the FIRST USE of a declared value -
  // not at startup, where the first attempt put it and where it hung every
  // legitimate launch, because on the production path the prefs arrive over
  // the connection after the process is already up.
  {
    int32_t w = mozilla::StaticPrefs::zoom_stealth_screen_width();
    int32_t h = mozilla::StaticPrefs::zoom_stealth_screen_height();
    if (w > 0 && h > 0) {
      // First use of a declared value in this process refuses if the set is
      // incomplete. Cheap after the first call, and it runs BEFORE the value
      // is handed back, so nothing can be served from a compiled default.
      mozilla::gfx::StealthAssertDeclarationsComplete();
      return {0, 0, w,
              h - mozilla::StaticPrefs::zoom_stealth_screen_taskbar_px()};
    }
  }
  // Return window inner rect to prevent fingerprinting.
  if (ShouldResistFingerprinting(RFPTarget::ScreenAvailRect)) {
    return GetTopWindowInnerRectForRFP();
  }

  // Check for overrides set by WebDriver BiDi or RDM before applying
  // fingerprinting protection. This allows developer tools to simulate
  // specific device dimensions for testing purposes.
  if (nsPIDOMWindowInner* owner = GetOwnerWindow()) {
    if (Document* doc = owner->GetExtantDoc()) {
      Maybe<CSSIntSize> deviceSize =
          nsGlobalWindowOuter::GetRDMDeviceSize(*doc);
      if (deviceSize.isSome()) {
        const CSSIntSize& size = deviceSize.value();
        return {0, 0, size.width, size.height};
      }
    }

    if (BrowsingContext* bc = owner->GetBrowsingContext()) {
      if (auto size = bc->GetScreenAreaOverride()) {
        return {{}, *size};
      }
    }
  }

  if (ShouldResistFingerprinting(RFPTarget::ScreenAvailToResolution)) {
    nsDeviceContext* context = GetDeviceContext();
    if (NS_WARN_IF(!context)) {
      return {};
    }

    if (nsPIDOMWindowInner* owner = GetOwnerWindow()) {
      if (Document* doc = owner->GetExtantDoc()) {
        AutoTArray<nsString, 1> params;
        params.AppendElement(
            u"https://support.mozilla.org/kb/firefox-protection-against-fingerprinting"_ns);
        doc->WarnOnceAbout(Document::eScreenFingerprintingProtection, false,
                           params);
      }
    }

    return nsRFPService::GetSpoofedScreenAvailSize(
        context->GetRect(), context->GetFullZoom(), IsFullscreen());
  }

  nsDeviceContext* context = GetDeviceContext();
  if (NS_WARN_IF(!context)) {
    return {};
  }
  return CSSIntRect::FromAppUnitsRounded(context->GetClientRect());
}

bool nsScreen::IsFullscreen() const {
  if (nsPIDOMWindowInner* owner = GetOwnerWindow()) {
    if (Document* doc = owner->GetExtantDoc()) {
      return StyleDisplayMode::Fullscreen ==
             Gecko_MediaFeatures_GetDisplayMode(doc);
    }
  }
  return false;
}

uint16_t nsScreen::GetOrientationAngle() const {
  nsDeviceContext* context = GetDeviceContext();
  if (context) {
    return context->GetScreenOrientationAngle();
  }
  RefPtr<widget::Screen> s =
      widget::ScreenManager::GetSingleton().GetPrimaryScreen();
  return s->GetOrientationAngle();
}

hal::ScreenOrientation nsScreen::GetOrientationType() const {
  nsDeviceContext* context = GetDeviceContext();
  if (context) {
    return context->GetScreenOrientationType();
  }
  RefPtr<widget::Screen> s =
      widget::ScreenManager::GetSingleton().GetPrimaryScreen();
  return s->GetOrientationType();
}

ScreenOrientation* nsScreen::Orientation() const { return mScreenOrientation; }

void nsScreen::GetMozOrientation(nsString& aOrientation,
                                 CallerType aCallerType) const {
  switch (mScreenOrientation->DeviceType(aCallerType)) {
    case OrientationType::Portrait_primary:
      aOrientation.AssignLiteral("portrait-primary");
      break;
    case OrientationType::Portrait_secondary:
      aOrientation.AssignLiteral("portrait-secondary");
      break;
    case OrientationType::Landscape_primary:
      aOrientation.AssignLiteral("landscape-primary");
      break;
    case OrientationType::Landscape_secondary:
      aOrientation.AssignLiteral("landscape-secondary");
      break;
    default:
      MOZ_CRASH("Unacceptable screen orientation type.");
  }
}

/* virtual */
JSObject* nsScreen::WrapObject(JSContext* aCx,
                               JS::Handle<JSObject*> aGivenProto) {
  return Screen_Binding::Wrap(aCx, this, aGivenProto);
}

CSSIntRect nsScreen::GetTopWindowInnerRectForRFP() {
  if (nsPIDOMWindowInner* inner = GetOwnerWindow()) {
    if (BrowsingContext* bc = inner->GetBrowsingContext()) {
      CSSIntSize size = bc->TopInnerSizeSpoofedForRFP();
      return {0, 0, size.width, size.height};
    }
  }
  return {};
}

bool nsScreen::ShouldResistFingerprinting(RFPTarget aTarget) const {
  nsGlobalWindowInner* owner = GetOwnerWindow();
  return owner && owner->ShouldResistFingerprinting(aTarget);
}
