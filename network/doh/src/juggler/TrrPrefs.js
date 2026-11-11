/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * The DoH state of the session, derived from the proxies the engine received.
 *
 * Behind a proxy the browser resolves names itself: network.trr.mode=3 sends
 * every lookup over DoH, through the same proxy as the page, and the host's
 * resolver is never asked. The proxies are the source of whether DoH is on, so
 * the engine derives that from them (a state fixed at launch would miss a
 * session whose proxy comes per context, and its names would go to the host's
 * resolver). The PROVIDER is not the engine's: the core declares
 * network.trr.uri and network.trr.bootstrapAddr as user prefs, and the engine
 * neither writes nor clears them.
 *
 * ONE function, syncTrrPrefs, recomputes the whole state from the set of
 * proxies present (the browser-wide proxy plus every live context's) and
 * TargetRegistry calls it at each event that changes the set. At least one
 * proxy: user values for the three prefs below (mode, exclude-etc-hosts,
 * excluded-domains). None: those three user values are cleared and the browser
 * is retail without proxy (mode 0).
 *
 * They are USER values on purpose: the DoH heuristics (DoHController) stand
 * down when mode and uri have a user value (the core declares the uri).
 *
 * Mode 3 with no DoH server resolves nothing, so turning DoH on while
 * network.trr.uri is empty (a build run by hand, no declaration from the core)
 * throws before any write, and the command that changed the set is refused.
 *
 * Order of the writes: turning on, mode goes LAST; turning off, mode goes
 * FIRST. Only a pref whose value changes is written, so a call with the same
 * state is silent.
 *
 * Ordering towards the socket process (where the lookups run): a pref set in
 * the parent reaches it as a PreferenceUpdate message on the PSocketProcess
 * channel (nsIOService::NotifySocketProcessPrefsChanged, prefix "network.trr.",
 * called synchronously by the pref set). Name resolution of a socket-process
 * transaction (PHttpTransaction, PDNSRequest) goes over the same channel, which
 * is in order, so a lookup started after the Juggler wrote the prefs cannot
 * overtake them on the wire. The only slack is inside the socket process, where
 * the update is applied on its main thread and a transaction starts on the
 * socket thread: microseconds, against the milliseconds the client needs to open
 * the first page after the command. No sync point is added.
 *
 * Known limit: a name the system resolver already answered before a context
 * proxy arrived stays in the DNS cache (nsHostResolver, not cleared by this
 * module) and is served from there until its TTL ends.
 */

const MODE_TRR_ONLY = 3;

// A host that is not a name: an IPv4 literal (dotted, or one number) or an IPv6
// literal, with or without brackets. A proxy given by IP needs no exclusion:
// there is no name to resolve.
function nameOf(host) {
  const bare = String(host || '').trim().replace(/^\[|\]$/g, '').toLowerCase();
  if (!bare || bare.includes(':') || /^[0-9.]+$/.test(bare) || /^0x[0-9a-f.x]+$/.test(bare))
    return '';
  return bare;
}

/**
 * The state for a set of proxy records: null when there is none, otherwise the
 * values of the three prefs, in the order they are written.
 */
export function trrStateFor(proxies) {
  const present = proxies.filter(Boolean);
  if (!present.length)
    return null;
  const names = [...new Set(present.map(proxy => nameOf(proxy.host)).filter(Boolean))].sort();
  return [
    ['network.trr.exclude-etc-hosts', false],
    ['network.trr.excluded-domains', names.join(',')],
    ['network.trr.mode', MODE_TRR_ONLY],
  ];
}

const ALL_PREFS = [
  'network.trr.mode',
  'network.trr.exclude-etc-hosts',
  'network.trr.excluded-domains',
];

function readUserValue(prefs, name, value) {
  if (!prefs.prefHasUserValue(name))
    return undefined;
  if (typeof value === 'number')
    return prefs.getIntPref(name);
  if (typeof value === 'boolean')
    return prefs.getBoolPref(name);
  return prefs.getStringPref(name);
}

function writeUserValue(prefs, name, value) {
  if (typeof value === 'number')
    prefs.setIntPref(name, value);
  else if (typeof value === 'boolean')
    prefs.setBoolPref(name, value);
  else
    prefs.setStringPref(name, value);
}

/**
 * Brings the prefs to the state of `proxies` (the proxy records present).
 * Idempotent. Throws when a pref cannot be written; the caller then refuses the
 * command that changed the set.
 */
export function syncTrrPrefs(proxies, prefs = Services.prefs) {
  const state = trrStateFor(proxies);
  if (!state) {
    // Mode first: DoH is off before the rest is taken away. The provider stays.
    for (const name of ALL_PREFS) {
      if (prefs.prefHasUserValue(name))
        prefs.clearUserPref(name);
    }
    return;
  }
  // The provider is the core's declaration. Without it mode 3 resolves nothing.
  if (!prefs.getStringPref('network.trr.uri', '')) {
    throw new Error('DoH needs a resolver: network.trr.uri is empty, and the core declares it in the profile');
  }
  for (const [name, value] of state) {
    if (readUserValue(prefs, name, value) !== value)
      writeUserValue(prefs, name, value);
  }
}
