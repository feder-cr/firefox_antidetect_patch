/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";
// Note: this file should be loadabale with eval() into worker environment.
// Avoid Components.*, ChromeUtils and global const variables.

var SIMPLE_CHANNEL_MESSAGE_NAME = 'juggler:simplechannel';
// How many channel ends this load of the file has made: one part of each end's
// identity (see `_uid`).
var SIMPLE_CHANNEL_INSTANCES = 0;

var SimpleChannel = class SimpleChannel {
  constructor(name, uid) {
    this._name = name;
    this._messageId = 0;
    this._connectorId = 0;
    this._pendingMessages = new Map();
    this._handlers = new Map();
    this._bufferedIncomingMessages = [];
    this.transport = {
      sendMessage: null,
      dispose: () => {},
    };
    this._ready = false;
    // Set by detachFromPeer, cleared by the READY that attaches this end again.
    this._detached = false;
    this._onReattached = null;
    this._paused = false;
    this._disposed = false;

    // The answers given to the other end, kept until it acknowledges them, keyed
    // by the requesting end's identity AND its request number (see `_answerKey`).
    this._bufferedResponses = new Map();
    // The identity of THIS end, unique per instance, and it has to be the channel
    // that makes it so: every end numbers its requests from 1, so an end that
    // shared its identity with an earlier one had its requests answered from the
    // earlier one's answers and never dispatched. `uid` is only the caller's
    // label; content/main.js gave 'process-<pid>' to every page channel of a
    // process, and a page's "main world created" announcement was lost that way
    // (B286). The counter separates the ends of one process, the random part
    // separates processes that reuse a pid.
    this._uid = `${uid}#${++SIMPLE_CHANNEL_INSTANCES}.${Math.random().toString(36).slice(2, 10)}`;
    this._connectedToUID = undefined;
  }

  bindToActor(actor) {
    this.resetTransport();
    this._name = actor.actorName;
    const oldReceiveMessage = actor.receiveMessage;
    actor.receiveMessage = message => this._onMessage(message.data);
    this.setTransport({
      sendMessage: obj => actor.sendAsyncMessage(SIMPLE_CHANNEL_MESSAGE_NAME, obj),
      dispose: () => actor.receiveMessage = oldReceiveMessage,
    });
  }

  resetTransport() {
    this.transport.dispose();
    this.transport = {
      sendMessage: null,
      dispose: () => {},
    };
    this._ready = false;
  }

  setTransport(transport) {
    this.transport = transport;
    // connection handshake:
    // 1. There are two channel ends in different processes.
    // 2. Both ends start in the `ready = false` state, meaning that they will
    //    not send any messages over transport.
    // 3. Once channel end is created, it sends { ack: `READY` } message to the other end.
    // 4. Eventually, at least one of the ends receives { ack: `READY` } message and responds with
    //    { ack: `READY_ACK` }. We assume at least one of the ends will receive { ack: "READY" } event from the other, since
    //    channel ends have a "parent-child" relation, i.e. one end is always created before the other one.
    // 5. Once channel end receives either { ack: `READY` } or { ack: `READY_ACK` }, it transitions to `ready` state.
    this.transport.sendMessage({ ack: 'READY', uid: this._uid });
  }

  pause() {
    this._paused = true;
  }

  resumeSoon() {
    if (!this._paused)
      return;
    this._paused = false;
    this._setTimeout(() => this._deliverBufferedIncomingMessages(), 0);
  }

  _setTimeout(cb, timeout) {
    // Lazy load on first call.
    this._setTimeout = ChromeUtils.importESModule('resource://gre/modules/Timer.sys.mjs').setTimeout;
    this._setTimeout(cb, timeout);
  }

  /**
   * The other end has stopped listening to this one, though the transport is
   * still there: the parent end follows the CURRENT document's actor, so a
   * document that goes into the back-forward cache keeps its actor while the
   * parent rebinds to the next one. Without this the end stayed ready, and a
   * restored document spoke at once into an actor nothing listened to, and the
   * READY of the rebinding replayed nothing, since the end was already ready
   * (B303 E3). Now what is said meanwhile waits, and goes out at the READY,
   * after the reattachment hook.
   */
  detachFromPeer() {
    if (!this._ready)
      return;
    this._ready = false;
    this._detached = true;
  }

  /**
   * `callback` runs when a detached end is attached again, before what it
   * queued while detached goes out, so that what it says there is heard
   * first: the restored document announces itself before its console speaks
   * in a context the client does not know yet.
   */
  onReattached(callback) {
    this._onReattached = callback;
  }

  _markAsReady() {
    if (this._ready)
      return;
    this._ready = true;
    // Snapshot first: what the hook sends goes out as it is sent, and must not
    // be sent a second time by the replay below.
    const queued = [...this._pendingMessages.values()];
    if (this._detached) {
      this._detached = false;
      if (this._onReattached)
        this._onReattached();
    }
    for (const { message } of queued)
      this.transport.sendMessage(message);
  }

  dispose() {
    if (this._disposed)
      return;
    this._disposed = true;
    for (const {resolve, reject, methodName} of this._pendingMessages.values())
      reject(new Error(`Failed "${methodName}": ${this._name} is disposed.`));
    this._pendingMessages.clear();
    this._handlers.clear();
    this.transport.dispose();
  }

  _rejectCallbacksFromConnector(connectorId) {
    for (const [messageId, callback] of this._pendingMessages) {
      if (callback.connectorId === connectorId) {
        callback.reject(new Error(`Failed "${callback.methodName}": connector for namespace "${callback.namespace}" in channel "${this._name}" is disposed.`));
        this._pendingMessages.delete(messageId);
      }
    }
  }

  connect(namespace) {
    const connectorId = ++this._connectorId;
    return {
      send: (...args) => this._send(namespace, connectorId, ...args),
      emit: (...args) => void this._send(namespace, connectorId, ...args).catch(e => {}),
      dispose: () => this._rejectCallbacksFromConnector(connectorId),
    };
  }

  register(namespace, handler) {
    if (this._handlers.has(namespace))
      throw new Error('ERROR: double-register for namespace ' + namespace);
    this._handlers.set(namespace, handler);
    this._deliverBufferedIncomingMessages();
    return () => this.unregister(namespace);
  }

  _deliverBufferedIncomingMessages() {
    const bufferedRequests = this._bufferedIncomingMessages;
    this._bufferedIncomingMessages = [];
    for (const data of bufferedRequests) {
      this._onMessage(data);
    }
  }

  unregister(namespace) {
    this._handlers.delete(namespace);
  }

  /**
   * @param {string} namespace
   * @param {number} connectorId
   * @param {string} methodName
   * @param {...*} params
   * @return {!Promise<*>}
   */
  async _send(namespace, connectorId, methodName, ...params) {
    if (this._disposed)
      throw new Error(`ERROR: channel ${this._name} is already disposed! Cannot send "${methodName}" to "${namespace}"`);
    const id = ++this._messageId;
    const message = {requestId: id, uid: this._uid, methodName, params, namespace};
    const promise = new Promise((resolve, reject) => {
      this._pendingMessages.set(id, {connectorId, resolve, reject, methodName, namespace, message});
    });
    if (this._ready)
      this.transport.sendMessage(message);
    return promise;
  }

  _onMessage(data) {
    if (data?.ack === 'READY') {
      // The "READY" and "READY_ACK" messages are a part of initialization sequence.
      // This sequence happens when a new SimpleChannel instance is getting
      // initialized on the other end, or when a new transport is assigned to
      // communicate between 2 SimpleChannel instances.
      //
      // The answers kept for the double-dispatch issue are NOT cleared here: they
      // are keyed by the requesting end, so a new end cannot meet an old end's
      // answer, and an old end that comes back (a document restored from the
      // back-forward cache) still finds its own. Clearing them on a change of
      // peer was what left an old end to be dispatched twice.
      this._connectedToUID = data.uid;
      this.transport.sendMessage({ ack: 'READY_ACK', uid: this._uid });
      this._markAsReady();
      return;
    }
    if (data?.ack === 'READY_ACK') {
      this._connectedToUID = data.uid;
      this._markAsReady();
      return;
    }
    if (data?.ack === 'RESPONSE_ACK') {
      this._bufferedResponses.delete(this._answerKey(data.uid, data.responseId));
      return;
    }
    if (this._paused)
      this._bufferedIncomingMessages.push(data);
    else
      this._onMessageInternal(data);
  }

  // An answer belongs to one request of one end: request numbers restart with
  // every end.
  _answerKey(uid, requestId) {
    return `${uid}/${requestId}`;
  }

  async _onMessageInternal(data) {
    if (data.responseId) {
      this.transport.sendMessage({ ack: 'RESPONSE_ACK', responseId: data.responseId, uid: this._uid });
      const message = this._pendingMessages.get(data.responseId);
      if (!message) {
        // During cross-process navigation, we might receive a response for
        // the message sent by another process.
        return;
      }
      this._pendingMessages.delete(data.responseId);
      if (data.error)
        message.reject(new Error(data.error));
      else
        message.resolve(data.result);
    } else if (data.requestId) {
      // When the underlying transport gets replaced, some responses might
      // not get delivered. As a result, sender will repeat the same request once
      // a new transport gets set.
      //
      // If this request was already processed, we can fulfill it with the cached response
      // and fast-return.
      const answerKey = this._answerKey(data.uid, data.requestId);
      if (this._bufferedResponses.has(answerKey)) {
        this.transport.sendMessage(this._bufferedResponses.get(answerKey));
        return;
      }

      const namespace = data.namespace;
      const handler = this._handlers.get(namespace);
      if (!handler) {
        this._bufferedIncomingMessages.push(data);
        return;
      }
      const method = handler[data.methodName];
      if (!method) {
        this.transport.sendMessage({responseId: data.requestId, error: `error in channel "${this._name}": No method "${data.methodName}" in namespace "${namespace}"`});
        return;
      }
      let response;
      const connectedToUID = this._connectedToUID;
      try {
        const result = await method.call(handler, ...data.params);
        response = {responseId: data.requestId, result};
      } catch (error) {
        response = {responseId: data.requestId, error: `error in channel "${this._name}": exception while running method "${data.methodName}" in namespace "${namespace}": ${error.message} ${error.stack}`};
      }
      // The connection might have changed during the ASYNCHRONOUS handler execution.
      // We only need to buffer & send response if we are connected to the same
      // end.
      if (connectedToUID === this._connectedToUID) {
        this._bufferedResponses.set(answerKey, response);
        this.transport.sendMessage(response);
      }
    } else {
      dump(`WARNING: unknown message in channel "${this._name}": ${JSON.stringify(data)}\n`);
    }
  }
};
