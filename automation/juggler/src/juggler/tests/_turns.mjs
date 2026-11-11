// How a test waits for the channels of the Juggler: by turns of the event loop and by conditions, never by a time.
//
// The ends of a SimpleChannel meet through a chain of hops (a message to the other actor, a timer of 0 ms for what was
// buffered, the READY and its answers). A fixed wait of 20 ms is one timer: on a machine that is busy the timer comes
// due before the hops that were to come first have run, and the test goes on with a channel that is not ready, or whose
// last READY_ACK is still on its way (a detach of a channel that is not ready does nothing; one that was done before the
// READY_ACK arrives is undone by it, and what the test said "while detached" went out at once).
// A turn of the loop runs the immediates that are queued, whatever the load, and a turn of the timers is added now and then:
// as many turns as there are hops are enough, and the number of turns does not depend on how fast the machine is.

const nextImmediate = () => new Promise(resolve => setImmediate(resolve));
const nextTimer = () => new Promise(resolve => setTimeout(resolve, 0));

// Until `condition()` holds. It gives up after a number of turns, not a time, with the name of the condition.
export async function until(what, condition, turns = 20000) {
  for (let i = 0; i < turns; i++) {
    if (condition())
      return;
    await (i % 25 === 24 ? nextTimer() : nextImmediate());
  }
  throw new Error('never became true: ' + what);
}

// For what must NOT happen, and for what is still on its way once a condition holds: enough turns for every message in
// flight to have arrived.
export async function settle(turns = 200) {
  for (let i = 0; i < turns; i++)
    await (i % 25 === 24 ? nextTimer() : nextImmediate());
}

// A pair of channel ends that have met: both ready, and nothing of the handshake still on its way. `world.inFlight()` is the
// number of messages the test's actors have sent and the other actor has not yet received.
export async function connected(world) {
  await until('both ends of the channel are ready', () => world.parentEnd.isReady() && world.contentEnd.isReady());
  await quiet(world);
}

// Nothing in flight: every message that was sent has been received (and answered, if it asked for an answer).
export async function quiet(world) {
  await until('no message in flight', () => world.inFlight() === 0);
  await settle(5);
  await until('no message in flight', () => world.inFlight() === 0);
}
