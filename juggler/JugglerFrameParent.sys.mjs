"use strict";

const { TargetRegistry } = ChromeUtils.importESModule('chrome://juggler/content/TargetRegistry.js');
const { Helper } = ChromeUtils.importESModule('chrome://juggler/content/Helper.js');

const helper = new Helper();

export class JugglerFrameParent extends JSWindowActorParent {
  constructor() {
    super();
  }

  receiveMessage() { }

  async actorCreated() {
    // Actors are registered per the WindowGlobalParent / WindowGlobalChild pair. We are only
    // interested in those WindowGlobalParent actors that are matching current browsingContext
    // window global.
    // See https://github.com/mozilla/gecko-dev/blob/cd2121e7d83af1b421c95e8c923db70e692dab5f/testing/mochitest/BrowserTestUtils/BrowserTestUtilsParent.sys.mjs#L15
    dump(`[PARENT-ACTOR] actorCreated browserId=${this.browsingContext.browserId} isCG=${this.manager?.isCurrentGlobal} parent=${!!this.browsingContext.parent}\n`);
    if (!this.manager?.isCurrentGlobal)
      return;

    // Only interested in main frames for now.
    if (this.browsingContext.parent)
      return;

    const registry = TargetRegistry.instance();
    const browserId = this.browsingContext.browserId;
    // FF150 Fission: two actors fire per tab — one for the parent-process BC (browserId=N)
    // and one for the content-process BC (browserId=N+1, where N = outer BC's bcId).
    // Primary lookup: direct browserId match (parent-process actor case).
    // Fission fallback: content-process BC.browserId == outer BC.id, so _bcIdToTarget hits.
    this._target = registry?.targetForBrowserId(browserId)
                ?? registry?._bcIdToTarget?.get(browserId)
                ?? null;
    if (!this._target && registry) {
      // Actor arrived before its PageTarget exists — register as pending.
      // TargetRegistry checks both browserId and bcId when wiring pending actors.
      registry._pendingActors.set(browserId, this);
      dump(`[PARENT-ACTOR] registered pending actor for browserId=${browserId}\n`);
    }
    dump(`[PARENT-ACTOR] target=${!!this._target} for browserId=${browserId}\n`);
    if (!this._target)
      return;

    this.actorName = `browser::page[${this._target.id()}]/${this.browsingContext.browserId}/${this.browsingContext.id}/${this._target.nextActorSequenceNumber()}`;
    dump(`[PARENT-ACTOR] calling setActor for target ${this._target.id()}\n`);
    this._target.setActor(this);
    dump(`[PARENT-ACTOR] setActor done\n`);
  }

  wireToTarget(target) {
    this._target = target;
    this.actorName = `browser::page[${target.id()}]/${this.browsingContext.browserId}/${this.browsingContext.id}/${target.nextActorSequenceNumber()}`;
    target.setActor(this);
    dump(`[PARENT-ACTOR] wireToTarget: bound to target ${target.id()} for browserId=${this.browsingContext.browserId}\n`);
  }

  didDestroy() {
    if (!this._target)
      return;
    this._target.removeActor(this);
  }
}
