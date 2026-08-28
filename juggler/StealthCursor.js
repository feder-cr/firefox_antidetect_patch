/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * A visible pointer in the CHROME window, for watching an automated session.
 *
 * ⛔ IT IS NOT A STEALTH FEATURE AND IT IS NOT A HUMANIZE FEATURE. It draws
 * nothing the page can reach and it changes no event the page receives: it
 * exists so a person watching the monitor, or a screen recording made from
 * outside the browser, can see where the pointer is. It is off by default for
 * exactly that reason - a dot moving on its own is "this is a bot" to anybody
 * looking at the screen, so it is a demo tool, not a default.
 *
 * ⛔ IN THE CHROME DOCUMENT, NEVER IN THE PAGE. The overlay is a node in the
 * browser UI's own document, which runs in a different process from the
 * content: page JavaScript cannot see it, cannot find it with a selector,
 * cannot hit it with `elementFromPoint`, and cannot photograph it - it is
 * absent from `page.screenshot()`, which is a consequence and not a
 * limitation. Drawing it in the page's document would be a DOM node a site can
 * enumerate, which is a tell and breaks rule 12.
 *
 * ⛔ WHY IT CAN WORK AT ALL. Juggler dispatches the mouse INTO THE CHROME
 * WINDOW: `Page.dispatchMouseEvent` takes the `<browser>` bounding box and
 * calls `win.windowUtils.jugglerSendMouseEvent(...)` with page coordinates
 * offset into chrome space (`protocol/PageHandler.js`). So a `mousemove`
 * listener on the chrome document sees automated movement in exactly the
 * coordinates this overlay needs. ⛔ THAT IS THE LOAD-BEARING ASSUMPTION of
 * this file and it is VERIFIED, not assumed: `tests/gates/chrome_cursor.py`
 * drives `page.mouse.move` and reads the counter this object keeps.
 *
 * ⛔ AND IT IS OURS, NOT MOZILLA'S. Camoufox does this by patching
 * `browser/base/content/browser-init.js`, a Mozilla file, which is a conflict
 * to resolve at every major. Everything here lives under `juggler/`, which we
 * already own, and the only foreign line is two in `TargetRegistry.js`.
 */

const PREF_ENABLED = 'stealthfox.showcursor';
const NODE_ID = '__stealth_cursor__';

export class StealthCursor {
  constructor(win) {
    this._win = win;
    this._dot = null;
    this._style = null;
    this._listening = false;
    // ⛔ Counted for the gate, and it is the only reason this field exists.
    // A test that asserts "the dot moved" by looking at a style attribute is
    // asserting on our own write; counting the events we RECEIVED is asserting
    // that the chrome window is reached at all, which is the thing that was
    // never certain.
    this.moves = 0;

    this._onMove = this._onMove.bind(this);
    this._onDown = this._onDown.bind(this);
    this._onUp = this._onUp.bind(this);
    this._onLeave = this._onLeave.bind(this);
    this._onPrefChanged = this._onPrefChanged.bind(this);

    Services.prefs.addObserver(PREF_ENABLED, this._onPrefChanged);
    if (this._wanted())
      this._enable();
  }

  _wanted() {
    // ⛔ Default FALSE. Camoufox defaults this on; we do not, and the
    // difference is the whole point of the feature being optional.
    try { return Services.prefs.getBoolPref(PREF_ENABLED, false); }
    catch (e) { return false; }
  }

  _stepMs() {
    // ⛔ READ, never written here. The same number in two places is exactly
    // the duplication that caused the defect in section 32 of
    // `20-our-patches.md`: the motion planner already owns this value, so the
    // overlay's transition asks IT rather than carrying a copy that can drift.
    try { return Services.prefs.getIntPref('stealthfox.humanize.stepMs', 10); }
    catch (e) { return 10; }
  }

  _onPrefChanged() {
    if (this._wanted())
      this._enable();
    else
      this._disable();
  }

  _enable() {
    if (this._listening)
      return;
    this._build();
    // ⛔ `{passive: true}` and capture FALSE, both deliberate. Passive
    // guarantees this listener can never call `preventDefault`, so it cannot
    // change what the content receives; bubble phase means it runs after the
    // event has already been dispatched onward rather than in front of it.
    // Together they remove the only credible way this overlay could become
    // visible to a page: by delaying or altering input.
    const opts = {passive: true, capture: false};
    this._win.addEventListener('mousemove', this._onMove, opts);
    this._win.addEventListener('mousedown', this._onDown, opts);
    this._win.addEventListener('mouseup', this._onUp, opts);
    this._win.addEventListener('mouseleave', this._onLeave, opts);
    this._listening = true;
  }

  _disable() {
    if (this._listening) {
      const opts = {capture: false};
      this._win.removeEventListener('mousemove', this._onMove, opts);
      this._win.removeEventListener('mousedown', this._onDown, opts);
      this._win.removeEventListener('mouseup', this._onUp, opts);
      this._win.removeEventListener('mouseleave', this._onLeave, opts);
      this._listening = false;
    }
    // ⛔ REMOVED, not hidden. "Off" has to mean the node is absent: a hidden
    // node is still a node, and a gate that asserts on visibility passes on a
    // switch that does not switch anything.
    if (this._dot) {
      this._dot.remove();
      this._dot = null;
    }
    if (this._style) {
      this._style.remove();
      this._style = null;
    }
  }

  _build() {
    if (this._dot)
      return;
    const doc = this._win.document;
    const root = doc.documentElement;
    if (!root)
      return;

    this._style = doc.createElement('style');
    // ⛔ `transform`, never `left`/`top`. Position through the transform with
    // `will-change` keeps the dot on its own compositor layer; writing
    // left/top relayouts the chrome document on every mouse event, which is
    // work done in the parent process while an input event is in flight - the
    // one thing that could make this measurable from the page.
    //
    // ⛔ `pointer-events: none` and no `cursor:` declaration, so the overlay
    // never enters the chrome hit-test and never changes what is under the
    // pointer.
    this._style.textContent = `
      #${NODE_ID} {
        position: fixed; top: 0; left: 0;
        width: 14px; height: 14px; margin: -7px 0 0 -7px;
        border-radius: 50%;
        background: rgba(0, 122, 255, .85);
        box-shadow: 0 0 0 6px rgba(0, 122, 255, .22);
        pointer-events: none;
        z-index: 2147483647;
        opacity: 1;
        will-change: transform;
        transition: transform ${this._stepMs()}ms linear, opacity 120ms linear;
      }
      #${NODE_ID}.pressed {
        background: rgba(255, 59, 48, .95);
        box-shadow: 0 0 0 3px rgba(255, 59, 48, .30);
      }
      #${NODE_ID} > .ring {
        position: absolute; top: 50%; left: 50%;
        width: 14px; height: 14px; margin: -7px 0 0 -7px;
        border-radius: 50%;
        border: 2px solid rgba(255, 59, 48, .9);
        pointer-events: none;
        animation: __stealth_cursor_ring__ 380ms ease-out forwards;
      }
      @keyframes __stealth_cursor_ring__ {
        from { transform: scale(1);   opacity: .9; }
        to   { transform: scale(3.4); opacity: 0;  }
      }
    `;
    root.appendChild(this._style);

    this._dot = doc.createElement('div');
    this._dot.id = NODE_ID;
    root.appendChild(this._dot);
  }

  _onMove(event) {
    this.moves++;
    if (!this._dot)
      return;
    this._dot.style.opacity = '1';
    this._dot.style.transform =
        `translate3d(${event.clientX}px, ${event.clientY}px, 0)`;
  }

  _onDown(event) {
    if (!this._dot)
      return;
    this._dot.classList.add('pressed');
    // ⛔ The ring is removed on `animationend` rather than on a timer: a timer
    // that outlives the window leaks a node, and one that fires early removes
    // a ring mid-animation. This is also the part Camoufox does not have.
    const ring = this._win.document.createElement('div');
    ring.className = 'ring';
    ring.addEventListener('animationend', () => ring.remove(), {once: true});
    this._dot.appendChild(ring);
  }

  _onUp(event) {
    if (this._dot)
      this._dot.classList.remove('pressed');
  }

  _onLeave(event) {
    // ⛔ Without this the dot stays frozen wherever the pointer left the
    // window, which looks exactly like a stuck automation and is the opposite
    // of what a person watching wants to see.
    if (this._dot)
      this._dot.style.opacity = '0';
  }

  dispose() {
    try { Services.prefs.removeObserver(PREF_ENABLED, this._onPrefChanged); }
    catch (e) {}
    this._disable();
    this._win = null;
  }
}

