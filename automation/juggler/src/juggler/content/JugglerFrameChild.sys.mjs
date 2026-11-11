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
      // Each fragment root goes into the cache and comes back on its own: the top, and a frame whose parent is in
      // another process. A frame served by the agents of its root (its parent is in this process) is not a root.
      if (!aEvent.persisted || !helper.isFragmentRoot(this.browsingContext) || aEvent.target !== this.document)
        return;
      if (aEvent.type === 'pagehide') {
        // A worker belongs to the current document: the workers of this root end for the client now, over the
        // channel that is still attached (what is said after the detach waits for the restore). A channel that
        // is not attached will not be reattached, so nothing is said then.
        if (agents.channel.isReady())
          agents.pageAgent.enterCache();
        agents.channel.detachFromPeer();
      } else
        this.sendAsyncMessage('juggler:became-current', {});
      return;
    }
    if (aEvent.target === this.document) {
      agents.pageAgent.onWindowEvent(aEvent);
      agents.frameTree.onWindowEvent(aEvent);
    }
  }

  _agents() {
    return topBrowingContextToAgents.get(helper.fragmentRootOf(this.browsingContext));
  }

  actorCreated() {
    this.actorName = `content::${this.browsingContext.browserId}/${this.browsingContext.id}/${++sameProcessInstanceNumber}`;

    this._eventListeners.push(helper.addEventListener(this.contentWindow, 'load', event => {
      this._agents()?.pageAgent.onWindowEvent(event);
    }));

    if (this.document.documentURI.startsWith('moz-extension://'))
      return;

    // A frame nested in the same process goes through the agents of its fragment root.
    if (!helper.isFragmentRoot(this.browsingContext))
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

    if (!helper.isFragmentRoot(this.browsingContext))
      return;

    const agents = topBrowingContextToAgents.get(this.browsingContext);
    // The agents are already re-bound to a new actor.
    if (agents?.actor !== this)
      return;

    topBrowingContextToAgents.delete(this.browsingContext);

    // Independent, in this order: none of them may stop the next one. A tree that is not disposed keeps every
    // document it served alive (its observers, its debugger, its contexts).
    for (const [name, dispose] of [
      ['channel', () => agents.channel.resetTransport()],
      ['pageAgent', () => agents.pageAgent.dispose()],
      ['frameTree', () => agents.frameTree.dispose()],
    ]) {
      try {
        dispose();
      } catch (e) {
        if (e) dump(`[JugglerFrameChild] disposing the ${name} failed: ${e.message}\n`);
      }
    }
  }

  receiveMessage() { }
}
