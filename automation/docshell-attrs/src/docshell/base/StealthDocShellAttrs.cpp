/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Stealthfox: the two attributes Juggler sets; see nsIDocShell.idl for
// where each is read (fileInputInterceptionEnabled by HTMLInputElement, the
// file picker; bypassCSPEnabled by CSP_IsBypassedFor in nsCSPUtils). They are
// declared by NS_DECL_NSIDOCSHELL and backed by two bit-fields of nsDocShell
// that its constructor initialises to false.

#include "nsDocShell.h"

NS_IMETHODIMP
nsDocShell::GetFileInputInterceptionEnabled(bool* aEnabled) {
  *aEnabled = mFileInputInterceptionEnabled;
  return NS_OK;
}

NS_IMETHODIMP
nsDocShell::SetFileInputInterceptionEnabled(bool aEnabled) {
  mFileInputInterceptionEnabled = aEnabled;
  return NS_OK;
}

NS_IMETHODIMP
nsDocShell::GetBypassCSPEnabled(bool* aEnabled) {
  *aEnabled = mBypassCSPEnabled;
  return NS_OK;
}

NS_IMETHODIMP
nsDocShell::SetBypassCSPEnabled(bool aEnabled) {
  mBypassCSPEnabled = aEnabled;
  return NS_OK;
}
