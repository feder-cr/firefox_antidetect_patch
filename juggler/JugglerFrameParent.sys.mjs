"use strict";

const { TargetRegistry } = ChromeUtils.importESModule('resource://gre/modules/juggler/TargetRegistry.js');
const { Helper } = ChromeUtils.importESModule('resource://gre/modules/juggler/Helper.js');

const helper = new Helper();

export class JugglerFrameParent extends JSWindowActorParent {
  constructor() {
    super();
  }

  receiveMessage(message) {
    // ⛔ THIS IS ONLY REACHED WHILE THE CHANNEL IS NOT BOUND TO THIS ACTOR:
    // `SimpleChannel.bindToActor` binds by replacing an actor's
    // `receiveMessage`, so the bound actor never arrives here. The one message
    // that comes this way is a restored document asking to be bound again
    // (JugglerFrameChild, on its pageshow). Its channel end has been detached
    // since its pagehide and keeps what it says, so nothing else is sent to an
    // unbound actor; and the READY of this binding is what makes it announce
    // itself and then speak (SimpleChannel.detachFromPeer).
    if (message?.name !== 'juggler:became-current')
      return;
    if (this._browserId === undefined || !this.manager?.isCurrentGlobal)
      return;
    const registry = TargetRegistry.instance();
    if (!registry?.targetForBrowserId(this._browserId))
      return;
    registry.onActorCreated(this, this._browserId);
  }

  async actorCreated() {
    // Actors are registered per the WindowGlobalParent / WindowGlobalChild pair. We are only
    // interested in those WindowGlobalParent actors that are matching current browsingContext
    // window global.
    // See https://github.com/mozilla/gecko-dev/blob/cd2121e7d83af1b421c95e8c923db70e692dab5f/testing/mochitest/BrowserTestUtils/BrowserTestUtilsParent.sys.mjs#L15
    if (!this.manager?.isCurrentGlobal)
      return;

    // Only interested in main frames for now.
    if (this.browsingContext.parent)
      return;

    // ⛔ A target is found by browserId and by nothing else (see
    // TargetRegistry._browserIdToActor). Matching it against a
    // BrowsingContext id as well let a foreign browser - the preallocated
    // about:newtab one, 2026-08-23 - take a page's channel when the two
    // counters met, and the guard added against that dropped a new tab's own
    // actor whenever they met with a stale entry. One identity removes both.
    this._browserId = this.browsingContext.browserId;
    TargetRegistry.instance()?.onActorCreated(this, this._browserId);
  }

  didDestroy() {
    if (this._browserId === undefined)
      return;
    TargetRegistry.instance()?.onActorDestroyed(this, this._browserId);
  }
}
