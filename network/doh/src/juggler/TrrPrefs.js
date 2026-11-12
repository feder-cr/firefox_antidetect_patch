/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * The DoH state of the session, derived from the proxies the engine received.
 *
 * Behind a proxy the browser resolves names itself, over DoH through the same
 * proxy as the page, and the host's resolver is never asked. WHICH lookups take
 * that road is decided by the engine's resolver hook, per lookup, by the user
 * context of the lookup: a context with a proxy resolves over DoH (fail-closed),
 * a context without resolves natively. The DoH service itself stays in mode 0:
 * no global network.trr.mode is written for a proxy (see below for a value left
 * by an older version). The Juggler only publishes WHICH contexts have a proxy,
 * in the transport pref network.trr.stealth.contexts: comma-separated tokens,
 * `*` = the browser-wide (launch) proxy exists (every context without a proxy
 * of its own is proxied), decimal userContextIds = contexts with a proxy of
 * their own (0 is the default context). Absent = no proxy anywhere. The list is
 * sorted (`*` first, then ids ascending) and without duplicates, so equal
 * states give equal strings. The PROVIDER is not the engine's: the core
 * declares network.trr.uri and network.trr.bootstrapAddr as user prefs, and the
 * engine neither writes nor clears them.
 *
 * ONE function, syncTrrPrefs, recomputes the whole state from the registry
 * (the browser-wide proxy plus every live context's, with its userContextId) and
 * TargetRegistry calls it at each event that changes it. At least one proxy:
 * user values for the three prefs below (exclude-etc-hosts, excluded-domains,
 * contexts). None: those three user values are cleared and the browser is
 * retail without proxy.
 *
 * Turning DoH on while network.trr.uri is empty (a build run by hand, no
 * declaration from the core) throws before any write, and the command that
 * changed the set is refused. A write that fails half way is rolled back to the
 * values the three prefs had, then the error is rethrown: the caller restores
 * the previous proxy, and the list still says what the registry says.
 *
 * Order of the writes: turning on or changing, the list goes LAST (the names
 * that lookups of a proxied context must not send over DoH are in place before
 * the list says the context is proxied); turning off, the list goes FIRST. Only
 * a pref whose value changes is written, so a call with the same state is
 * silent.
 *
 * A leftover network.trr.mode: earlier versions of the engine wrote mode 3 as a
 * user value while a proxy existed. A profile that carries it from such a
 * session would send EVERY lookup over DoH, contexts without a proxy included,
 * and the resolver hook would never be asked. On each sync, a user value of
 * exactly 3 is cleared when the provider is the core's (network.trr.uri is not
 * empty); any other value of the pref is somebody else's and is left
 * alone. Nothing else in the Juggler writes network.trr.mode.
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

// The value earlier versions wrote to network.trr.mode while a proxy existed.
const LEGACY_MODE = 3;
const MODE_PREF = 'network.trr.mode';
const CONTEXTS_PREF = 'network.trr.stealth.contexts';

// The ASCII (punycode, lowercase) form of a host name, the form the DNS suffix
// match of the excluded-domains list compares against. The platform's URL
// parser does the IDNA conversion; when it refuses the name the lowercased name
// is kept.
function asciiName(name) {
  if (/[\s/\\?#@%]/.test(name))
    return name;
  try {
    const host = new URL('http://' + name).hostname.toLowerCase();
    return host || name;
  } catch (e) {
    return name;
  }
}

// A host that is not a name: an IPv4 literal (dotted, or one number) or an IPv6
// literal, with or without brackets. A proxy given by IP needs no exclusion:
// there is no name to resolve. A name is written in its ASCII form, lowercase,
// without the trailing dot of a fully qualified name.
function nameOf(host) {
  const bare = String(host || '').trim().replace(/^\[|\]$/g, '').toLowerCase();
  if (!bare || bare.includes(':') || /^[0-9.]+$/.test(bare) || /^0x[0-9a-f.x]+$/.test(bare))
    return '';
  const name = asciiName(bare).replace(/\.+$/, '');
  if (!name || /^[0-9.]+$/.test(name))
    return '';
  return name;
}

/**
 * The list of the contexts that have a proxy, as the string of the transport
 * pref: `*` when the browser-wide proxy exists, then the userContextId of every
 * context with a proxy of its own, ascending, once each. `contexts` is any
 * iterable of {userContextId, proxy}. Empty string: no proxy anywhere.
 */
export function contextsWithProxy(browserProxy, contexts) {
  const ids = [...new Set([...contexts]
      .filter(context => context && context.proxy)
      .map(context => context.userContextId))]
      .sort((a, b) => a - b);
  return (browserProxy ? ['*'] : []).concat(ids.map(String)).join(',');
}

/**
 * The state for the proxies of the registry: null when there is none,
 * otherwise [name, value] of the three prefs, in the order they are written
 * (the list last).
 */
export function trrStateFor(browserProxy, contexts) {
  const list = contextsWithProxy(browserProxy, contexts);
  if (!list)
    return null;
  const present = [browserProxy, ...[...contexts].map(context => context && context.proxy)].filter(Boolean);
  const names = [...new Set(present.map(proxy => nameOf(proxy.host)).filter(Boolean))].sort();
  return [
    ['network.trr.exclude-etc-hosts', false],
    ['network.trr.excluded-domains', names.join(',')],
    [CONTEXTS_PREF, list],
  ];
}

// The three prefs of the engine, the list first (the order a turn-off clears
// them in), each with a value of the kind it holds.
const ALL_PREFS = [
  [CONTEXTS_PREF, ''],
  ['network.trr.exclude-etc-hosts', false],
  ['network.trr.excluded-domains', ''],
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

// Back to the values the three prefs had (undefined = no user value), the list
// first. Best effort: the error that matters is the one that started the roll
// back.
function rollBack(prefs, before) {
  for (const [name, kind, value] of before) {
    try {
      if (value === undefined) {
        if (prefs.prefHasUserValue(name))
          prefs.clearUserPref(name);
      } else if (readUserValue(prefs, name, kind) !== value) {
        writeUserValue(prefs, name, value);
      }
    } catch (e) {}
  }
}

// The core declared a DoH resolver (network.trr.uri is not empty).
function providerDeclared(prefs) {
  return !!prefs.getStringPref('network.trr.uri', '');
}

/**
 * Brings the prefs to the state of the registry: `browserProxy` (the
 * browser-wide record or null) and `contexts` (iterable of {userContextId,
 * proxy}, one per live context). Idempotent. Throws when a pref cannot be
 * written; the three prefs are then back at their values before the call, and
 * the caller refuses the command that changed the set.
 */
export function syncTrrPrefs(browserProxy, contexts, prefs = Services.prefs) {
  // A mode 3 left by an earlier version of the engine: see the note on top.
  if (providerDeclared(prefs) && prefs.prefHasUserValue(MODE_PREF) &&
      prefs.getIntPref(MODE_PREF) === LEGACY_MODE)
    prefs.clearUserPref(MODE_PREF);
  const state = trrStateFor(browserProxy, contexts);
  // The provider is the core's declaration. Without it DoH resolves nothing.
  if (state && !providerDeclared(prefs))
    throw new Error('DoH needs a resolver: network.trr.uri is empty, and the core declares it in the profile');
  const before = ALL_PREFS.map(([name, kind]) => [name, kind, readUserValue(prefs, name, kind)]);
  try {
    if (!state) {
      // The list first: lookups go native before the rest is taken away. The provider stays.
      for (const [name] of ALL_PREFS) {
        if (prefs.prefHasUserValue(name))
          prefs.clearUserPref(name);
      }
      return;
    }
    for (const [name, value] of state) {
      if (readUserValue(prefs, name, value) !== value)
        writeUserValue(prefs, name, value);
    }
  } catch (e) {
    rollBack(prefs, before);
    throw e;
  }
}
