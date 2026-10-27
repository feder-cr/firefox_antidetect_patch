"use strict";

const { Helper } = ChromeUtils.importESModule('resource://gre/modules/juggler/Helper.js');
const { initialize } = ChromeUtils.importESModule('resource://gre/modules/juggler/content/main.js');

const Ci = Components.interfaces;
const helper = new Helper();

let sameProcessInstanceNumber = 0;

const topBrowingContextToAgents = new Map();

export class JugglerFrameChild extends JSWindowActorChild {
  constructor() {
    super();

    this._eventListeners = [];
  }

  handleEvent(aEvent) {
    const agents = this._agents();
    if (!agents)
      return;
    if (aEvent.type === 'DOMWillOpenModalDialog') {
      agents.channel.pause();
      return;
    }
    if (aEvent.type === 'DOMModalDialogClosed') {
      agents.channel.resumeSoon();
      return;
    }
    if (aEvent.type === 'pagehide' || aEvent.type === 'pageshow') {
      // ⛔ THE BACK-FORWARD CACHE, SAID BY THE ONE WHO KNOWS. `persisted` on a
      // top-level document's pagehide means it goes into the cache, and the
      // parent end of the channel is about to follow the next document's
      // actor: this end stops sending, and keeps what it is told to send. On
      // its pageshow it is the current document again, and asks the parent,
      // over its own actor (the channel cannot carry it yet), to bind to it;
      // the READY of that binding runs the restored document's announcement
      // and then sends what was kept (SimpleChannel.detachFromPeer). The
      // restore used to be guessed from a location change whose document was
      // already complete, and what the page said before the parent rebound was
      // lost (B303 E2, E3).
      if (!aEvent.persisted || this.browsingContext.parent || aEvent.target !== this.document)
        return;
      if (aEvent.type === 'pagehide')
        agents.channel.detachFromPeer();
      else
        this.sendAsyncMessage('juggler:became-current', {});
      return;
    }
    if (aEvent.target === this.document) {
      agents.pageAgent.onWindowEvent(aEvent);
      agents.frameTree.onWindowEvent(aEvent);
    }
  }

  _agents() {
    return topBrowingContextToAgents.get(this.browsingContext.top);
  }

  actorCreated() {
    this.actorName = `content::${this.browsingContext.browserId}/${this.browsingContext.id}/${++sameProcessInstanceNumber}`;

    this._eventListeners.push(helper.addEventListener(this.contentWindow, 'load', event => {
      this._agents()?.pageAgent.onWindowEvent(event);
    }));

    if (this.document.documentURI.startsWith('moz-extension://'))
      return;

    // Child frame events will be forwarded to related top-level agents.
    if (this.browsingContext.parent)
      return;

    let agents = topBrowingContextToAgents.get(this.browsingContext);
    if (!agents) {
      agents = initialize(this.browsingContext, this.docShell);
      topBrowingContextToAgents.set(this.browsingContext, agents);
    }
    agents.channel.bindToActor(this);
    agents.actor = this;
  }

  didDestroy() {
    try {
      helper.removeListeners(this._eventListeners);
    } catch (e) {
      if (e) dump(`[JugglerFrameChild] removeListeners failed: ${e.message}\n`);
    }

    if (this.browsingContext.parent)
      return;

    const agents = topBrowingContextToAgents.get(this.browsingContext);
    // The agents are already re-bound to a new actor.
    if (agents?.actor !== this)
      return;

    topBrowingContextToAgents.delete(this.browsingContext);

    agents.channel.resetTransport();
    agents.pageAgent.dispose();
    agents.frameTree.dispose();
  }

  receiveMessage() { }
}
