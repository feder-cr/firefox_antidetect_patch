/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_StealthWindowGeometry_h
#define mozilla_dom_StealthWindowGeometry_h

// The window geometry a page sees when the stealth engine is on, for the outer
// window it asks. Each Stealth* function below answers false (or Nothing) when
// the engine is off: the caller then keeps Firefox's own path. Otherwise it
// fills its out parameter and answers true. Main thread only.
//
// This file holds the definitions too, and is included by
// nsGlobalWindowOuter.cpp ONLY: dom/base/moz.build registers no source file
// for it, so the hooks in nsGlobalWindowOuter.cpp add no line to a shared
// build file. Do not include it from another translation unit.
//
// No compiled value anywhere: a missing declaration refuses in
// StealthDeclarationGate instead of being papered over here.

#include "StealthDeclarationGate.h"
#include "Units.h"
#include "mozilla/DimensionRequest.h"
#include "mozilla/ErrorResult.h"
#include "mozilla/Maybe.h"
#include "mozilla/RefPtr.h"
#include "mozilla/dom/BrowserChild.h"
#include "mozilla/dom/BrowsingContext.h"
#include "mozilla/dom/Document.h"
#include "nsCOMPtr.h"
#include "nsGlobalWindowOuter.h"
#include "nsIBaseWindow.h"
#include "nsIDocShell.h"
#include "nsIWidget.h"
#include "nsPIDOMWindowInlines.h"
#include "nsPresContext.h"
#include "nsRect.h"

namespace mozilla::dom {

namespace {

// La finestra di primo livello di questo documento e' un popup (aperto con
// delle feature)? La dichiarazione della finestra dipende da questo.
inline bool StealthIsInPopup(const nsGlobalWindowOuter& aWindow) {
  BrowsingContext* bc = aWindow.GetBrowsingContext();
  return bc && bc->Top()->GetIsPopupRequested();
}

// The content origin shift for the window's document, or Nothing().
inline Maybe<nsPoint> StealthShiftOf(nsGlobalWindowOuter& aWindow) {
  nsIDocShell* docShell = aWindow.GetDocShell();
  RefPtr<nsPresContext> pc = docShell ? docShell->GetPresContext() : nullptr;
  return nsGlobalWindowOuter::StealthContentOriginShift(pc);
}

}  // namespace

// window.outerWidth / outerHeight, in CSS pixels. Throws NS_ERROR_FAILURE
// through aError (and answers 0x0) when the window has no tree owner.
//
// Stealth: the outer size from the declared window, in device pixels,
// converted below with the window's own scale exactly like the real size.
//
// The MAIN window is maximized: the work area plus Windows' invisible resize
// border on each side (retail 151, 100%: outerWidth - availWidth = 16). Until
// firefox-38 this answered the work area itself, a window that does not
// exist maximized on Windows (B261). A POPUP is its real inner size plus its
// frame - a border on the left, right and bottom, the popup UI above - where
// it used to answer the maximized window's size, 1920x1032 for a
// width=500,height=400 popup (B258). The inner size is the top-level one,
// synced to every process in the BrowsingContext (see
// nsPresContext::UpdateInnerSizeSpoofedForRFP), so an out-of-process iframe
// in the popup answers the same.
//
// No compiled value anywhere: a missing declaration refuses in
// StealthDeclarationGate instead of being papered over here.
inline bool StealthOuterSize(nsGlobalWindowOuter& aWindow, CSSIntSize& aSize,
                             ErrorResult& aError) {
  if (!gfx::StealthEngineActive()) {
    return false;
  }
  gfx::StealthAssertDeclarationsComplete();
  nsCOMPtr<nsIBaseWindow> treeOwnerAsWin = aWindow.GetTreeOwnerWindow();
  if (!treeOwnerAsWin) {
    aError.Throw(NS_ERROR_FAILURE);
    aSize = CSSIntSize();
    return true;
  }
  const CSSToLayoutDeviceScale scale =
      aWindow.CSSToDevScaleForBaseWindow(treeOwnerAsWin);
  if (StealthIsInPopup(aWindow)) {
    const auto frame = gfx::StealthDeclaredWindowFrame();
    BrowsingContext* bc = aWindow.GetBrowsingContext();
    if (frame && bc) {
      const LayoutDeviceIntSize inner = gfx::RoundedToInt(
          CSSSize(bc->TopInnerSizeSpoofedForRFP().width,
                  bc->TopInnerSizeSpoofedForRFP().height) *
          scale);
      const LayoutDeviceIntSize outer =
          inner + LayoutDeviceIntSize(2 * frame->mBorder,
                                      frame->mPopupUi + frame->mBorder);
      aSize = gfx::RoundedToInt(outer / scale);
      return true;
    }
  } else if (const auto maximized = gfx::StealthDeclaredMaximizedWindow()) {
    aSize = gfx::RoundedToInt(maximized->Size() / scale);
    return true;
  }
  // Unreachable: the gate refuses at launch without the declaration.
  aSize = CSSIntSize();
  return true;
}

// window.screenX / screenY of the MAIN window, from the declared maximized
// window. False for a popup, which keeps its real position, and when the
// declaration has no maximized window.
//
// Stealth: the MAIN window's position is the declared maximized window's,
// a resize border off the screen (-8 at 100%, -7 at 125%, -6 at 200% on
// retail 151), which outerWidth above depends on: until 2026-08-09 the
// position stayed whatever the OS handed the headless widget, and screenX +
// outerWidth came to 1924 on a 1920 screen. A POPUP keeps its REAL
// position: the page chose it, or nsWindowWatcher put it at the declared
// default when the page did not, so it is the same on both builds.
inline bool StealthScreenXY(nsGlobalWindowOuter& aWindow,
                            nsIBaseWindow* aTreeOwner, CSSIntPoint& aPoint) {
  if (!gfx::StealthEngineActive() || StealthIsInPopup(aWindow)) {
    return false;
  }
  if (const auto maximized = gfx::StealthDeclaredMaximizedWindow()) {
    aPoint = gfx::RoundedToInt(
        maximized->TopLeft() / aWindow.CSSToDevScaleForBaseWindow(aTreeOwner));
    return true;
  }
  return false;
}

// mozInnerScreenX / mozInnerScreenY: aRect (the real inner screen rect of the
// window) moved by the content origin shift. Nothing when there is no shift.
//
// Stealth: the content origin, from the ONE function that answers it.
//
// This used to read the two prefs here and do the arithmetic inline, and so
// did mozInnerScreenY, and so did screenX with its own pair - three copies of
// the same idea, and the EVENT path never got its copy because nothing in the
// source connected them. A page found that with one subtraction: on every
// mouse event, screenX - clientX must equal this value, and it did not, on 45
// events out of 45. See StealthDeclarationGate.h for the measurement.
//
// GetInnerScreenRect keeps serving its other callers - popup placement,
// coordinate conversion - the real rect. Those are not content-visible.
// Lo SCARTO, non la sostituzione. Sostituire risponde l'origine del PRIMO
// LIVELLO a ogni finestra: misurato il 2026-08-10, un iframe a (220, 150)
// nella pagina rispondeva (0, 85) come il top, e dentro quell'iframe
// screenX - clientX dava 220 contro un mozInnerScreenX di 0 - la stessa
// contraddizione che questa patch esiste per togliere, spostata di un
// livello. Sommando lo scarto al rettangolo REALE della finestra, l'offset
// fra i frame resta vero e la relazione vale a ogni profondita'.
inline Maybe<float> StealthMozInnerScreenX(nsGlobalWindowOuter& aWindow,
                                           const nsRect& aRect) {
  if (const auto shift = StealthShiftOf(aWindow)) {
    return Some(nsPresContext::AppUnitsToFloatCSSPixels(aRect.x + shift->x));
  }
  return Nothing();
}

// Stealth: see StealthMozInnerScreenX. A maximized window's content starts
// the UI height below the screen top - 85 device pixels at 100%, 107 at 125%,
// which reads 85.6 like retail 151 - and a popup's the popup UI below its
// own top.
inline Maybe<float> StealthMozInnerScreenY(nsGlobalWindowOuter& aWindow,
                                           const nsRect& aRect) {
  if (const auto shift = StealthShiftOf(aWindow)) {
    return Some(nsPresContext::AppUnitsToFloatCSSPixels(aRect.y + shift->y));
  }
  return Nothing();
}

}  // namespace mozilla::dom

// Lo scarto fra l'origine reale del contenuto di primo livello (l'offset su
// schermo del root widget, cio' che mozInnerScreenX riportava prima di
// diventare una dichiarazione) e quella dichiarata, in app units; Nothing()
// col motore spento. UNA funzione per mozInnerScreenX/Y e per gli eventi
// (Event.cpp): e' cosi' che screenX - clientX == mozInnerScreenX resta vero
// anche nei popup e negli iframe. Un sottoframe somma lo scarto al PROPRIO
// rettangolo, non lo sostituisce.
//
// Declared in nsGlobalWindowOuter.h, defined here at global scope: it is not
// inline, so this header must stay included by one translation unit only.
// Member bodies at global scope do not see mozilla::dom, so every name below
// is qualified.
/* static */
mozilla::Maybe<nsPoint> nsGlobalWindowOuter::StealthContentOriginShift(
    nsPresContext* aPresContext) {
  if (!aPresContext || !mozilla::gfx::StealthEngineActive()) {
    return mozilla::Nothing();
  }
  // L'origine reale e' l'offset su schermo del root widget: nel processo di
  // contenuto e' il PuppetWidget, il cui offset e' l'origine dell'area di
  // contenuto di primo livello - per il documento top coincide con
  // GetInnerScreenRect(), per un sottoframe no, ed e' proprio quella
  // differenza che va conservata. Solo chiamate al widget, niente layout:
  // Event.cpp chiama questa funzione per ogni evento del mouse.
  nsCOMPtr<nsIWidget> rootWidget = aPresContext->GetRootWidget();
  if (!rootWidget) {
    return mozilla::Nothing();
  }
  const int32_t auPerDevPx = aPresContext->AppUnitsPerDevPixel();
  const nsPoint real = mozilla::LayoutDevicePixel::ToAppUnits(
      rootWidget->WidgetToScreenOffset(), auPerDevPx);
  // Un popup si dichiara dalla sua posizione REALE: il rettangolo esterno della
  // finestra di primo livello, che BrowserChild riceve dal genitore - anche in
  // un iframe fuori processo, dove arriva dall'embedder. E' la stessa fonte di
  // GetScreenXY (nsIBaseWindow::GetPosition del tree owner).
  mozilla::Maybe<mozilla::LayoutDeviceIntPoint> popupOrigin;
  mozilla::dom::Document* doc = aPresContext->Document();
  mozilla::dom::BrowsingContext* bc =
      doc ? doc->GetBrowsingContext() : nullptr;
  if (bc && bc->Top()->GetIsPopupRequested()) {
    mozilla::dom::BrowserChild* child = rootWidget->GetOwningBrowserChild();
    if (!child) {
      // Il processo genitore non serve pagine: niente da dichiarare.
      return mozilla::Nothing();
    }
    mozilla::LayoutDeviceIntPoint outer;
    if (NS_FAILED(child->GetDimensions(mozilla::DimensionKind::Outer,
                                       &outer.x.value, &outer.y.value, nullptr,
                                       nullptr))) {
      return mozilla::Nothing();
    }
    popupOrigin = mozilla::Some(outer);
  }
  return mozilla::gfx::StealthDeclaredOriginShift(real, popupOrigin,
                                                  auPerDevPx);
}

#endif  // mozilla_dom_StealthWindowGeometry_h
