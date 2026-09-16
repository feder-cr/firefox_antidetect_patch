"use strict";

const { TargetRegistry } = ChromeUtils.importESModule('chrome://juggler/content/TargetRegistry.js');
const { Helper } = ChromeUtils.importESModule('chrome://juggler/content/Helper.js');

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
    if (!this._target || !this.manager?.isCurrentGlobal)
      return;
    this.wireToTarget(this._target);
    // Only now does the restored document have somewhere to speak, so only now
    // is it asked to announce itself. The other order loses the announcement,
    // measured.
    this._target._channel.connect('').send('announceRestoredDocument')
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

    const registry = TargetRegistry.instance();
    const browserId = this.browsingContext.browserId;
    // FF150 Fission: two actors fire per tab — one for the parent-process BC (browserId=N)
    // and one for the content-process BC (browserId=N+1, where N = outer BC's bcId).
    // Primary lookup: direct browserId match (parent-process actor case).
    // Fission fallback: content-process BC.browserId == outer BC.id, so _bcIdToTarget hits.
    let target = registry?.targetForBrowserId(browserId)
              ?? registry?._bcIdToTarget?.get(browserId)
              ?? null;
    // ⛔ UN NUMERO NON E' UN'IDENTITA'. `browserId` e l'`id` di un
    // BrowsingContext sono due contatori DIVERSI, e la riga sopra li confronta
    // fra loro: basta che si scontrino perche' un browser estraneo si prenda il
    // canale della pagina, e `setActor` piu' sotto lo lega davvero.
    //
    // Misurato il 2026-08-23 su Windows, con `browser.newtab.preload` al suo
    // default (acceso da quando il newtab e' tornato a upstream): il browser
    // PREALLOCATO della nuova scheda nasce con browserId 12 mentre 12 era l'id
    // del BrowsingContext della nostra scheda, e si legava allo stesso target.
    // Da fuori si vedeva un secondo `Page.frameAttached mainframe-12` sulla
    // stessa sessione, `page.url` diventava `about:newtab` e la PRIMA
    // `page.goto()` moriva - 0 su 9 - con "can't access property loadURI,
    // browsingContext is undefined", perche' `frameIdToBrowsingContext` cerca
    // quel frame sotto il browser della scheda e li' non c'e'. Spegnendo la
    // preallocazione: 4 su 4 riusciti, nessun mainframe di troppo.
    //
    // L'identita' vera e' l'ELEMENTO <browser> a cui il contesto appartiene.
    // Si usa per SMENTIRE, non per cercare: dove l'elemento non c'e' non si puo'
    // dire niente e resta il comportamento di prima.
    const embedder = this.browsingContext.top?.embedderElement;
    let smentito = false;
    if (target && embedder && registry?.targetForBrowser(embedder) !== target) {
      target = null;
      smentito = true;
    }
    this._target = target;
    if (!this._target && registry && !smentito) {
      // Actor arrived before its PageTarget exists — register as pending.
      // TargetRegistry checks both browserId and bcId when wiring pending actors.
      //
      // Un attore SMENTITO non entra qui: la lista dei pendenti e' indicizzata
      // sugli stessi due numeri che si sono appena scontrati, quindi metterlo in
      // coda sposterebbe la stessa collisione al prossimo target che nasce.
      registry._pendingActors.set(browserId, this);
    }
    if (!this._target)
      return;

    this.actorName = `browser::page[${this._target.id()}]/${this.browsingContext.browserId}/${this.browsingContext.id}/${this._target.nextActorSequenceNumber()}`;
    this._target.setActor(this);
  }

  wireToTarget(target) {
    this._target = target;
    this.actorName = `browser::page[${target.id()}]/${this.browsingContext.browserId}/${this.browsingContext.id}/${target.nextActorSequenceNumber()}`;
    target.setActor(this);
  }

  didDestroy() {
    if (!this._target)
      return;
    this._target.removeActor(this);
  }
}
