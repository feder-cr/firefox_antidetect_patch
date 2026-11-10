/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";
// No Gecko dependency: the channel is injected (`createChannel`), so a node test loads this file.

// The parent's end of a page's conversation with its content processes.
//
// A page is held by one content process per fragment root: the document of the top, and every frame
// whose parent is in another process (with sites isolated, a cross-site iframe). Each root has its own
// channel to its own actor; the PageHandler still sees one channel, with `connect(ns)` for what it
// sends and `register(ns, handlers)` for what it receives, and the mux decides where a message goes.
//
// What it sends is routed by method name (`ROUTES`): by execution context, by frame, by the parent
// of a frame, to the main root, or to every root that has an actor. What it receives is fanned in from
// every root, with the root it came from as the second argument of the handler: `(payload, {rootId})`.
// The mux also keeps what only it can know across roots: which root holds an execution context, a
// worker, an armed click landing.

// Method -> where it goes. `kind`: 'ctx' (by executionContextId), 'frame' (the root of frameId),
// 'parentOf' (the root of the parent of childFrameId), 'main' (the main root), 'broadcast' (every root
// with an actor), 'landing' (the root that armed the landing token; inside the mux).
const byContext = p => ({kind: 'ctx', id: p.executionContextId});
const byFrame = p => ({kind: 'frame', id: p.frameId});
const toMain = () => ({kind: 'main'});
const toAll = () => ({kind: 'broadcast'});

export const ROUTES = {
  // by execution context
  evaluate: byContext,
  callFunction: byContext,
  getObjectProperties: byContext,
  disposeObject: byContext,
  // by frame
  describeNode: byFrame,
  elementAtom: byFrame,
  getFrameQuads: byFrame,
  frameViewport: byFrame,
  scrollIntoViewIfNeeded: byFrame,
  setFileInputFiles: byFrame,
  setUserInput: byFrame,
  selectOptions: byFrame,
  armLanding: byFrame,
  // the frame's parent: the box of a child frame is measured where its element is
  frameBoxTransform: p => ({kind: 'parentOf', id: p.childFrameId}),
  // the name of a child frame is read where its <iframe> is
  frameName: p => ({kind: 'parentOf', id: p.childFrameId}),
  // without an object: the context the node is adopted into; with one: the frame of the object
  adoptNode: p => p.objectId ? {kind: 'frame', id: p.frameId} : {kind: 'ctx', id: p.executionContextId},
  // the root that armed the token
  awaitInputHandled: p => ({kind: 'landing', token: p.landingToken}),
  // the main root, until the input steps route them by focus and by gesture
  dispatchKeyEvent: toMain,
  insertText: toMain,
  dispatchTapEvent: toMain,
  dispatchDragEvent: toMain,
  effectiveZoom: toMain,
  awaitViewportDimensions: toMain,
  // every root with an actor
  setInitScripts: toAll,
  addBinding: toAll,
  applyContextSetting: toAll,
  setInterceptFileChooserDialog: toAll,
  ensurePermissions: toAll,
};

// Pure: where a command of the page namespaces goes. A method with no route is an error, not the main
// root: a command added without a route failed silently towards the top.
export function routeOf(method, params) {
  const route = ROUTES[method];
  if (!route)
    throw new Error('ContentMux: no route for ' + method);
  return route(params || {});
}

export class ContentMux {
  // `pageTarget`: `linkedBrowser()` and `frameIdToBrowsingContext(frameId)`. `createChannel(name, uid)`
  // makes one end of a channel to one content process (a SimpleChannel).
  constructor({pageTarget, name, uid, createChannel}) {
    this._pageTarget = pageTarget;
    this._name = name;
    this._uid = uid || name;
    this._createChannel = createChannel;
    this._endpoints = new Map();      // rootId -> {rootId, bc, actor, channel, isMain, contexts, workers}
    this._contexts = new Map();       // executionContextId -> {id, auxData, rootId}
    this._workerRoot = new Map();     // workerId -> rootId
    this._armed = new Map();          // muxToken -> {rootId, rootToken}
    this._nextToken = 0;
    this._registrations = new Map();  // namespace -> handlers (the fan-in, on every root)
  }

  // ---- the roots ------------------------------------------------------------------------------

  // A root is announced: the endpoint exists from now on, bound or not. Requests to an endpoint with no
  // actor wait for it, as they always did for the top.
  onRootAdded(rootId, bc) {
    const endpoint = this._ensureEndpoint(rootId, !bc.parent);
    endpoint.bc = bc;
    return endpoint;
  }

  // The actor of a root is there (a process, or a new document in it): its channel is bound to it.
  bindRoot(rootId, actor, {isMain}) {
    const endpoint = this._ensureEndpoint(rootId, isMain);
    endpoint.actor = actor;
    endpoint.isMain = isMain;
    endpoint.channel.bindToActor(actor);
    return endpoint;
  }

  // The actor of a root is gone. Null when it is not the one bound any more (a new one came first: the
  // order of the two is not fixed), else the endpoint, `{rootId, isMain}`.
  unbindRoot(rootId, actor) {
    const endpoint = this._endpoints.get(rootId);
    if (!endpoint || endpoint.actor !== actor)
      return null;
    endpoint.channel.resetTransport();
    endpoint.actor = null;
    return endpoint;
  }

  // The browsing context of a root was discarded. The main root stays: it changes id with the top.
  onRootRemoved(rootId) {
    const endpoint = this._endpoints.get(rootId);
    if (endpoint && !endpoint.isMain)
      this.disposeRoot(rootId);
  }

  // Nothing is left of a root: what waits for it is refused, what it held is forgotten, its workers end.
  disposeRoot(rootId) {
    const endpoint = this._endpoints.get(rootId);
    if (!endpoint)
      return;
    this._endpoints.delete(rootId);
    const handlers = this._registrations.get('page');
    for (const workerId of [...endpoint.workers]) {
      this._workerRoot.delete(workerId);
      // The WorkerHandler of a worker whose root went without saying so is disposed by the handler.
      if (handlers && handlers.pageWorkerDestroyed)
        handlers.pageWorkerDestroyed({workerId}, {rootId});
    }
    endpoint.workers.clear();
    for (const id of [...endpoint.contexts])
      this._contexts.delete(id);
    endpoint.contexts.clear();
    for (const [token, armed] of [...this._armed]) {
      if (armed.rootId === rootId)
        this._armed.delete(token);
    }
    endpoint.channel.dispose();
  }

  rootIds() {
    return [...this._endpoints.keys()];
  }

  endpointFor(rootId) {
    return this._endpoints.get(rootId);
  }

  // The endpoint of the main root. There is one: when the top changes (a new browsing context in another
  // group) it is the same endpoint under the new id, so what waits on it is sent to the new document.
  _mainEndpoint() {
    for (const endpoint of this._endpoints.values()) {
      if (endpoint.isMain)
        return endpoint;
    }
    return undefined;
  }

  _ensureEndpoint(rootId, isMain) {
    let endpoint = isMain ? this._mainEndpoint() : undefined;
    if (endpoint && endpoint.rootId !== rootId) {
      this._endpoints.delete(endpoint.rootId);
      for (const id of endpoint.contexts) {
        const context = this._contexts.get(id);
        if (context)
          context.rootId = rootId;
      }
      for (const armed of this._armed.values()) {
        if (armed.rootId === endpoint.rootId)
          armed.rootId = rootId;
      }
      endpoint.rootId = rootId;
      this._endpoints.set(rootId, endpoint);
    }
    if (!endpoint)
      endpoint = this._endpoints.get(rootId);
    if (endpoint) {
      if (isMain)
        endpoint.isMain = true;
      return endpoint;
    }
    endpoint = {
      rootId,
      bc: null,
      actor: null,
      isMain: !!isMain,
      channel: this._createChannel(`${this._name}/root-${rootId}`, this._uid),
      contexts: new Set(),
      workers: new Set(),
    };
    this._endpoints.set(rootId, endpoint);
    for (const [namespace, handlers] of this._registrations)
      this._installFanIn(endpoint, namespace, handlers);
    return endpoint;
  }

  // The endpoint a message for `rootId` goes to; the main root's is made on the first message.
  _endpointToSend(rootId) {
    const existing = this._endpoints.get(rootId);
    if (existing)
      return existing;
    return this._ensureEndpoint(rootId, rootId === this.mainRootId());
  }

  // ---- what the page asks of a frame, an execution context, a worker --------------------------

  mainRootId() {
    return this._pageTarget.linkedBrowser().browsingContext.id;
  }

  // The root a browsing context belongs to: up while the parent is in the same process. Null while it
  // is not known (the window global of a context in navigation is not there): the caller tries again.
  frameRootOf(browsingContext) {
    let current = browsingContext;
    while (current.parent) {
      const windowGlobal = current.currentWindowGlobal;
      if (!windowGlobal)
        return null;
      if (windowGlobal.isProcessRoot)
        break;
      current = current.parent;
    }
    return current.id;
  }

  rootOf(frameId) {
    const browsingContext = this._pageTarget.frameIdToBrowsingContext(frameId);
    if (!browsingContext)
      throw new Error('Failed to find frame with id = ' + frameId);
    return this.frameRootOf(browsingContext);
  }

  contextRoot(executionContextId) {
    const context = this._contexts.get(executionContextId);
    if (!context)
      throw new Error('Failed to find execution context with id = ' + executionContextId);
    return context.rootId;
  }

  routeOf(method, params) {
    return routeOf(method, params);
  }

  // The armed landing a token of ours stands for, without consuming it.
  armedEntry(muxToken) {
    return this._armed.get(muxToken);
  }

  // ---- sending --------------------------------------------------------------------------------

  // What the PageHandler (and the target) talk through: `send(method, ...args)` is routed, `emit` is
  // `send` that nobody waits for, `dispose()` refuses what this connector still waits for on every root.
  connect(namespace) {
    const connector = {namespace, disposed: false, owns: new Map()};   // owns: endpoint -> its connector on that channel
    return {
      send: (method, ...args) => this._send(connector, method, args),
      emit: (method, ...args) => void this._send(connector, method, args).catch(e => {}),
      dispose: () => {
        connector.disposed = true;
        for (const own of connector.owns.values())
          own.dispose();
        connector.owns.clear();
      },
    };
  }

  _connectorOn(endpoint, connector) {
    let own = connector.owns.get(endpoint);
    if (!own) {
      own = endpoint.channel.connect(connector.namespace);
      connector.owns.set(endpoint, own);
    }
    return own;
  }

  async _send(connector, method, args) {
    if (connector.disposed)
      throw new Error(`ERROR: connector for "${connector.namespace}" is already disposed! Cannot send "${method}"`);
    const namespace = connector.namespace;
    // A worker's namespace is its id: it goes to the root that holds the worker.
    if (namespace !== '' && namespace !== 'page') {
      if (!this._workerRoot.has(namespace))
        throw new Error('Failed to find worker with id = ' + namespace);
      return this._sendToRoot(connector, this._workerRoot.get(namespace), method, args);
    }
    const params = args[0];
    const route = routeOf(method, params);
    switch (route.kind) {
      case 'broadcast':
        return this.broadcast(namespace, method, params);
      case 'ctx':
        return this._sendToRoot(connector, this.contextRoot(route.id), method, args);
      case 'frame': {
        const rootId = this.rootOf(route.id);
        if (rootId === null)
          throw new Error('root of frame ' + route.id + ' unknown, retry');
        if (method === 'adoptNode' && params.executionContextId !== undefined && this.contextRoot(params.executionContextId) !== rootId)
          return {remoteObject: null};
        const result = await this._sendToRoot(connector, rootId, method, args);
        if (method === 'armLanding' && result && result.token) {
          const muxToken = ++this._nextToken;
          this._armed.set(muxToken, {rootId, rootToken: result.token});
          return {...result, token: muxToken};
        }
        return result;
      }
      case 'parentOf': {
        const child = this._pageTarget.frameIdToBrowsingContext(route.id);
        if (!child || !child.parent)
          throw new Error('Failed to find frame with id = ' + route.id);
        const rootId = this.frameRootOf(child.parent);
        if (rootId === null)
          throw new Error('root of frame ' + route.id + ' unknown, retry');
        return this._sendToRoot(connector, rootId, method, args);
      }
      case 'main':
        return this._sendToRoot(connector, this.mainRootId(), method, args);
      case 'landing': {
        const armed = route.token ? this._armed.get(route.token) : undefined;
        if (!armed)
          return this._sendToRoot(connector, this.mainRootId(), method, [{...params, landingToken: 0}]);
        try {
          return await this._sendToRoot(connector, armed.rootId, method, [{...params, landingToken: armed.rootToken}]);
        } finally {
          this._armed.delete(route.token);
        }
      }
    }
    throw new Error('ContentMux: unknown kind of route ' + route.kind + ' for ' + method);
  }

  _sendToRoot(connector, rootId, method, args) {
    return this._connectorOn(this._endpointToSend(rootId), connector).send(method, ...args);
  }

  // To one root, past the routing.
  sendTo(rootId, namespace, method, params) {
    return this._connectorOn(this._endpointToSend(rootId), {namespace, disposed: false, owns: new Map()}).send(method, params);
  }

  // To every root that has an actor; the roots without one read the shared data when their process starts.
  // Settled when all answered; it fails only if every root refused.
  async broadcast(namespace, method, params) {
    const endpoints = [...this._endpoints.values()].filter(endpoint => endpoint.actor);
    if (!endpoints.length)
      return undefined;
    const answers = await Promise.allSettled(
        endpoints.map(endpoint => endpoint.channel.connect(namespace).send(method, params)));
    if (answers.every(answer => answer.status === 'rejected'))
      throw answers[0].reason;
    return undefined;
  }

  // ---- receiving ------------------------------------------------------------------------------

  // `handlers[name](payload, {rootId})` for what any root says in `namespace`. Returns what undoes it.
  register(namespace, handlers) {
    if (this._registrations.has(namespace))
      throw new Error('ERROR: double-register for namespace ' + namespace);
    this._registrations.set(namespace, handlers);
    for (const endpoint of this._endpoints.values())
      this._installFanIn(endpoint, namespace, handlers);
    return () => this._unregister(namespace);
  }

  _unregister(namespace) {
    if (!this._registrations.delete(namespace))
      return;
    for (const endpoint of this._endpoints.values())
      endpoint.channel.unregister(namespace);
  }

  _installFanIn(endpoint, namespace, handlers) {
    const wrapped = {};
    for (const name of Object.keys(handlers))
      wrapped[name] = payload => this._onEvent(name, payload, endpoint.rootId, handlers);
    endpoint.channel.register(namespace, wrapped);
  }

  _onEvent(name, payload, rootId, handlers) {
    const endpoint = this._endpoints.get(rootId);
    if (endpoint && name === 'runtimeExecutionContextCreated') {
      this._contexts.set(payload.executionContextId, {id: payload.executionContextId, auxData: payload.auxData, rootId});
      endpoint.contexts.add(payload.executionContextId);
    } else if (endpoint && name === 'runtimeExecutionContextDestroyed') {
      this._forgetContext(payload.executionContextId);
    } else if (endpoint && name === 'runtimeExecutionContextsCleared') {
      return this._onContextsCleared(endpoint, payload, handlers);
    } else if (endpoint && name === 'pageWorkerCreated') {
      this._workerRoot.set(payload.workerId, rootId);
      endpoint.workers.add(payload.workerId);
    } else if (endpoint && name === 'pageWorkerDestroyed') {
      this._workerRoot.delete(payload.workerId);
      endpoint.workers.delete(payload.workerId);
    }
    return handlers[name](payload, {rootId});
  }

  _forgetContext(executionContextId) {
    const context = this._contexts.get(executionContextId);
    if (!context)
      return;
    this._contexts.delete(executionContextId);
    const endpoint = this._endpoints.get(context.rootId);
    if (endpoint)
      endpoint.contexts.delete(executionContextId);
  }

  // A root dropped the contexts it announced. The client drops everything on this event, so the main
  // root's is forwarded and the contexts of the others are announced again at once; a root that is not
  // the main one only loses its own.
  _onContextsCleared(endpoint, payload, handlers) {
    const {rootId} = endpoint;
    if (endpoint.isMain) {
      for (const id of [...endpoint.contexts])
        this._forgetContext(id);
      const result = handlers.runtimeExecutionContextsCleared(payload, {rootId});
      for (const context of this._contexts.values()) {
        const owner = this._endpoints.get(context.rootId);
        if (owner && !owner.isMain)
          handlers.runtimeExecutionContextCreated({executionContextId: context.id, auxData: context.auxData}, {rootId: context.rootId});
      }
      return result;
    }
    for (const id of [...endpoint.contexts]) {
      this._forgetContext(id);
      handlers.runtimeExecutionContextDestroyed({executionContextId: id}, {rootId});
    }
    return undefined;
  }
}
