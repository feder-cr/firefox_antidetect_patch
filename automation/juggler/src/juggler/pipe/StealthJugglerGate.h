/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#pragma once

// True when an automation client is attached to the Juggler pipe. Asked by
// nsCertOverrideService::IsDebugger() the same way it asks whether Marionette
// or the Remote Agent is running. Main thread only: off the main thread the
// pipe refuses to answer and this returns false.
bool StealthJugglerPipeConnected();
