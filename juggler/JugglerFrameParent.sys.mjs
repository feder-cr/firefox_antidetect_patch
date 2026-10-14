"use strict";

const { TargetRegistry } = ChromeUtils.importESModule('resource://gre/modules/juggler/TargetRegistry.js');
const { Helper } = ChromeUtils.importESModule('resource://gre/modules/juggler/Helper.js');

const helper = new Helper();

export class JugglerFrameParent extends JSWindowActorParent {
  constructor() {
    super();
  }

  receiveMessage(message) {
    // ⛔ THIS IS ONLY REACHED WHILE THE CHANNEL IS NOT BOUND TO THIS ACTOR,
    // and that is exactly the case worth handling: `SimpleChannel.bindToActor`
    // binds by replacing an actor's `receiveMessage`, so the bound actor never
    // arrives here. A document restored from the back-forward cache belongs to
    // an actor that was created earlier and then stopped being the bound one,
    // which is why everything it says is dropped today.
    if (message?.name !== 'juggler:became-current')
      return;
    if (this._browserId === undefined || !this.manager?.isCurrentGlobal)
      return;
    const registry = TargetRegistry.instance();
    const target = registry?.targetForBrowserId(this._browserId);
    if (!target)
      return;
    registry.onActorCreated(this, this._browserId);
    // Only now does the restored document have somewhere to speak, so only now
    // is it asked to announce itself. The other order loses the announcement,
    // measured.
    target._channel.connect('').send('announceRestoredDocument')
        .catch(e => void e);
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
