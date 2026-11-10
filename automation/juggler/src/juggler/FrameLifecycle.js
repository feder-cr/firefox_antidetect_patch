/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const {Helper} = ChromeUtils.importESModule('resource://gre/modules/juggler/Helper.js');

const helper = new Helper();

// Which frames a page has and when they go: the parent process's answer, from
// the browsing contexts it is told about whatever process they live in.
//
// The content process of the top used to be the only voice for `Page.frameAttached`
// and `Page.frameDetached`; with the frames of other sites in other processes it
// cannot know them all. The parent is notified of every browsing context that
// attaches or is discarded (`browsing-context-attached`, `browsing-context-discarded`),
// so the announcement no longer depends on which process holds the frame.
//
// One FrameLifecycle per protocol session, owned by its PageHandler, which registers
// its listeners and only then calls `start()`: the constructor emits nothing, so the
// main frame's event cannot be lost between the two.
//
// Events: 'frameattached' {frameId, parentFrameId}, 'framedetached' {frameId},
// 'framedropped' {frameId} (a browsing context of this page that went before it was announced).
export class FrameLifecycle {
  // `browser`: the page's <browser>. Its browserId is kept, not its browsing
  // context: the top can be replaced (`why === 'replace'`) and the frames of the
  // new top have another `top` than the old one's.
  constructor(browser) {
    this._browser = browser;
    this._browserId = browser.browsingContext.browserId;
    this._mainFrameId = helper.browsingContextToFrameId(browser.browsingContext);
    this._frames = new Map();       // frameId -> {parentFrameId}, only the frames announced
    this._listeners = [];
    helper.decorateAsEventEmitter(this);
  }

  start() {
    const top = this._browser.browsingContext;
    this._announce(top, undefined);
    for (const browsingContext of helper.collectAllBrowsingContexts(top).slice(1))
      this._ensure(browsingContext);
    this._listeners = [
      helper.addObserver((browsingContext, topic, why) => this._onAttached(browsingContext, why), 'browsing-context-attached'),
      helper.addObserver((browsingContext, topic, why) => this._onDiscarded(browsingContext), 'browsing-context-discarded'),
    ];
  }

  _onAttached(browsingContext, why) {
    // The top changing group: the main frame's id is the browser's, it stays.
    if (why === 'replace')
      return;
    if (!browsingContext.parent)
      return;
    this._ensure(browsingContext);
  }

  // Announces `browsingContext` after its ancestors, so a child told of before its
  // parent is not lost. False for a context this page does not have.
  _ensure(browsingContext) {
    if (browsingContext.browserId !== this._browserId)
      return false;
    if (!browsingContext.parent)
      return this._frames.has(this._mainFrameId);
    // Chrome documents have no web progress; extension pages are not the page's frames.
    if (!browsingContext.isContent || !browsingContext.webProgress)
      return false;
    if (browsingContext.currentRemoteType === 'extension')
      return false;
    const frameId = helper.browsingContextToFrameId(browsingContext);
    if (this._frames.has(frameId))
      return true;
    if (!this._ensure(browsingContext.parent))
      return false;
    this._announce(browsingContext, browsingContext.parent);
    return true;
  }

  _announce(browsingContext, parentBrowsingContext) {
    const frameId = helper.browsingContextToFrameId(browsingContext);
    if (this._frames.has(frameId))
      return;
    const parentFrameId = parentBrowsingContext ? helper.browsingContextToFrameId(parentBrowsingContext) : undefined;
    this._frames.set(frameId, {parentFrameId});
    this.emit('frameattached', {frameId, parentFrameId});
  }

  // The frames announced below `frameId` (its children, theirs, ...), parents first: the order they were announced.
  framesBelow(frameId) {
    const below = [];
    for (const [id, {parentFrameId}] of this._frames) {
      for (let parent = parentFrameId; parent !== undefined; parent = this._frames.get(parent)?.parentFrameId) {
        if (parent === frameId) {
          below.push({frameId: id, parentFrameId});
          break;
        }
      }
    }
    return below;
  }

  // Says again the frames below `frameId` that are still there. The client forgets the frames below a frame when
  // a document commits in it; when that document is one restored from the back-forward cache its frames were not
  // discarded (the cache keeps them), and nothing else tells the client they are back.
  reannounceBelow(frameId) {
    for (const frame of this.framesBelow(frameId))
      this.emit('frameattached', frame);
  }

  _onDiscarded(browsingContext) {
    const frameId = helper.browsingContextToFrameId(browsingContext);
    if (this._frames.has(frameId)) {
      this._detach(frameId);
      return;
    }
    // Never announced: whoever buffers events for the frame can let them go.
    let browserId;
    try {
      browserId = browsingContext.browserId;
    } catch (e) {
      return;
    }
    if (browserId === this._browserId)
      this.emit('framedropped', {frameId});
  }

  _detach(frameId) {
    // The main frame is never detached, whatever the context that carried it.
    if (!this._frames.has(frameId) || frameId === this._mainFrameId)
      return;
    for (const [childId, child] of [...this._frames]) {
      if (child.parentFrameId === frameId)
        this._detach(childId);
    }
    this._frames.delete(frameId);
    this.emit('framedetached', {frameId});
  }

  dispose() {
    helper.removeListeners(this._listeners);
    this._listeners = [];
  }
}
