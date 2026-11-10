/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

// Which navigation each frame of a page is in, as the parent process tells it.
//
// The navigations of a frame were reported by the content process that held it, from its own state. A navigation
// that changes the process (a frame going to another site, once sites are isolated) starts in one process and
// commits in another, and the process that commits cannot know the id the navigation was started with. The parent
// sees both ends: the load identifier of every document request (NetworkObserver, `navigationId` of the request),
// the document appearing in whichever process holds the frame and the load ending with an error (both relayed
// by the content process that holds the frame, over the 'frames' namespace, in the order they happen). One registry per
// PageHandler, a frame at a time: started, then committed or aborted, each id once.
//
// `emit(name, payload, frameId)`: 'navigationstarted' {frameId, navigationId}, 'navigationcommitted'
// {frameId, navigationId, url, name}, 'navigationaborted' {frameId, navigationId, errorText},
// 'initialloadfailed' {frameId}: a frame that never committed a document of its own was refused its first load.

// An address without its fragment: the one a request goes by, whatever the document's location adds.
function withoutFragment(url) {
  const hash = url.indexOf('#');
  return hash < 0 ? url : url.slice(0, hash);
}

// How many ended navigations of a frame are remembered, so a late request of an ended navigation is not a new one.
const ENDED_REMEMBERED = 8;

export class FrameNavigations {
  constructor(emit) {
    this._emit = emit;
    this._pending = new Map();      // frameId -> {navigationId, url}
    this._committed = new Map();    // frameId -> navigationId
    this._ended = new Map();        // frameId -> [navigationId] (committed, aborted or replaced), the last few
    this._replaced = new Map();     // frameId -> Set(navigationId): replaced by a newer navigation before they were heard from
    this._initialDone = new Set();  // frameIds that have had a navigation end
    this._restoredDocuments = 0;
    this._documents = new Map();    // frameId -> {navigationId, url, name}: the last document told committed
  }

  pendingNavigationId(frameId) {
    const pending = this._pending.get(frameId);
    return pending ? pending.navigationId : undefined;
  }

  lastCommittedNavigationId(frameId) {
    return this._committed.has(frameId) ? this._committed.get(frameId) : null;
  }

  // A document request of the navigation, seen in the parent. Before the request itself is forwarded, so
  // the navigation starts before its first request.
  onDocumentRequest(frameId, navigationId, url) {
    this._start(frameId, navigationId, url);
  }

  // The load was begun in the content process of the frame, whether or not a request follows (about:blank,
  // data:, a navigation the network never sees).
  onContentNavigationStarted(frameId, navigationId) {
    this._start(frameId, navigationId, undefined);
  }

  _start(frameId, navigationId, url) {
    if (!navigationId)
      return;
    const pending = this._pending.get(frameId);
    if (pending && pending.navigationId === navigationId) {
      if (url) {
        pending.url = pending.url || url;
        pending.urls.add(withoutFragment(url));   // a redirect hop of the same navigation
      }
      return;
    }
    if (this._committed.get(frameId) === navigationId || this._hasEnded(frameId, navigationId))
      return;
    // A navigation replaces the one in flight without ending it, as it always did: its document or its abort can
    // still come, and is told under its own id.
    if (pending) {
      this._remember(frameId, pending.navigationId);
      let replaced = this._replaced.get(frameId);
      if (!replaced) {
        replaced = new Set();
        this._replaced.set(frameId, replaced);
      }
      replaced.add(pending.navigationId);
      // Bounded like the list of ended navigations: a frame that never hears from the ones it replaced (the main
      // frame is never forgotten) does not keep them for ever.
      while (replaced.size > ENDED_REMEMBERED)
        replaced.delete(replaced.values().next().value);
    }
    this._pending.set(frameId, {navigationId, url, urls: new Set(url ? [withoutFragment(url)] : [])});
    this._emit('navigationstarted', {frameId, navigationId}, frameId);
  }

  // A document appeared in the frame, in whichever process holds it. It commits the navigation in flight; a
  // document with none (the first about:blank of an iframe, document.open) commits nothing.
  //
  // `navigationId`: the navigation the process that holds the frame had in flight when the document appeared, if it
  // had one. The document is that navigation's even if the parent has since started another one for the frame (a page
  // that navigates from its first script: the parent sees the new request before it hears of the document), so it is
  // committed under its own id and the navigation in flight stays.
  onDocumentInserted(frameId, url, name, navigationId) {
    const pending = this._pending.get(frameId);
    if (navigationId && (!pending || pending.navigationId !== navigationId)) {
      const replaced = this._replaced.get(frameId);
      if (replaced && replaced.delete(navigationId)) {
        this._committed.set(frameId, navigationId);
        this._initialDone.add(frameId);
        this._tellCommit(frameId, navigationId, url, name);
      }
      return;
    }
    if (!pending)
      return;
    // No navigation of the process's own to go by: the document is the pending navigation's if it is at an address
    // that navigation went by. A document.open() writes a root element into the document that stays, which is not
    // the one the navigation in flight is loading.
    if (!navigationId && pending.urls.size && url && !pending.urls.has(withoutFragment(url)))
      return;
    this._pending.delete(frameId);
    this._committed.set(frameId, pending.navigationId);
    this._remember(frameId, pending.navigationId);
    this._initialDone.add(frameId);
    this._tellCommit(frameId, pending.navigationId, url, name);
  }

  _tellCommit(frameId, navigationId, url, name) {
    this._documents.set(frameId, {navigationId, url, name});
    this._emit('navigationcommitted', {frameId, navigationId, url, name}, frameId);
  }

  // The last document told committed in the frame, or undefined: what a frame says again when it comes back with the
  // page from the back-forward cache, the client having dropped it.
  lastDocument(frameId) {
    return this._documents.get(frameId);
  }

  // The address of the document changed without a new document (pushState, a hash): the next time it is said, it is this.
  onSameDocumentNavigation(frameId, url) {
    const document = this._documents.get(frameId);
    if (document)
      document.url = url;
  }

  // A frame as its process has it, said again: a page the client attaches to as it is, the frames of a document
  // restored from the cache (the client dropped them), the first document of a frame that no navigation made. A
  // document already committed is committed again, with the id the process knows; a navigation
  // in flight is started.
  onFrameKnown(frameId, {url, name, navigationId, pendingNavigationId}) {
    if (url) {
      // The id the process gives, and none otherwise: a document that no navigation of ours made (the about:blank of
      // a tree that is born, the one a script navigates to) must not borrow the id of the one it replaces.
      const known = navigationId || undefined;
      if (known)
        this._committed.set(frameId, known);
      this._initialDone.add(frameId);
      this._tellCommit(frameId, known, url, name);
    }
    if (pendingNavigationId && !this._pending.has(frameId))
      this._start(frameId, pendingNavigationId, undefined);
  }

  // The main frame's document came back from the back-forward cache. A restore has no load identifier to borrow:
  // the id only has to be new, and it is new for the whole page because it is counted here.
  onDocumentRestored(frameId, url, name) {
    const navigationId = 'nav-bfcache-' + (++this._restoredDocuments);
    const pending = this._pending.get(frameId);
    if (pending)
      this._remember(frameId, pending.navigationId);
    this._pending.set(frameId, {navigationId, url, urls: new Set()});
    this._emit('navigationstarted', {frameId, navigationId}, frameId);
    this.onDocumentInserted(frameId, url, name);
  }


  // A navigation of the frame ended in an error, as its content process saw it end: window.stop(), document.open(),
  // a refused port, a failed load. The process says it, not this side: it sees the end of the load in order with the
  // documents it inserts, so a navigation whose document came before the load was cut is already committed and is
  // not aborted (a page that navigates from its first script), and it filters the abort it makes itself when a
  // navigation changes process (FrameTree.onStateChange). `errorText` is already the protocol's text.
  onNavigationAborted(frameId, navigationId, errorText) {
    const pending = this._pending.get(frameId);
    if (!pending || pending.navigationId !== navigationId) {
      // A navigation a newer one replaced before its abort was heard: the client still waits for the end of it.
      const replaced = this._replaced.get(frameId);
      if (replaced && replaced.delete(navigationId))
        this._emit('navigationaborted', {frameId, navigationId, errorText}, frameId);
      return;
    }
    this._end(frameId, pending, errorText);
  }

  _end(frameId, pending, errorText) {
    this._pending.delete(frameId);
    this._remember(frameId, pending.navigationId);
    this._emit('navigationaborted', {frameId, navigationId: pending.navigationId, errorText}, frameId);
    if (!this._initialDone.has(frameId)) {
      this._initialDone.add(frameId);
      // A frame that was refused its first load still ends the way a loaded frame does, or whoever waits for its
      // load waits for good.
      if (!frameId.startsWith('mainframe-'))
        this._emit('initialloadfailed', {frameId}, frameId);
    }
  }

  _remember(frameId, navigationId) {
    let ended = this._ended.get(frameId);
    if (!ended) {
      ended = [];
      this._ended.set(frameId, ended);
    }
    ended.push(navigationId);
    if (ended.length > ENDED_REMEMBERED)
      ended.shift();
  }

  _hasEnded(frameId, navigationId) {
    const ended = this._ended.get(frameId);
    return !!ended && ended.includes(navigationId);
  }

  // A frame that is gone keeps nothing.
  forget(frameId) {
    this._pending.delete(frameId);
    this._committed.delete(frameId);
    this._ended.delete(frameId);
    this._replaced.delete(frameId);
    this._documents.delete(frameId);
    this._initialDone.delete(frameId);
  }
}
