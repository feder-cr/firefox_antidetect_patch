/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

// A completion (gethash) caused by a page of browser context N is requested in
// context N: its channel carries that userContextId, so the proxy filter of
// the session (Juggler's) and the resolver of the session answer for that
// context. Completions of different contexts are never put in one request; the
// default context is the request upstream makes.

const SERVER_PATH = "/stealth-gethash";

let gServer;
let gGethashUrl;
let gRequests = [];

function onModifyRequest(aSubject) {
  const channel = aSubject.QueryInterface(Ci.nsIHttpChannel);
  if (channel.URI.spec != gGethashUrl) {
    return;
  }
  const attrs = channel.loadInfo.originAttributes;
  gRequests.push({
    userContextId: attrs.userContextId,
    firstPartyDomain: attrs.firstPartyDomain,
  });
}

// A completion callback of a lookup made in context aUserContextId (undefined:
// a callback that declares none, like the ones of upstream's tests).
function makeCallback(aUserContextId, aDone) {
  const callback = {
    completionV2() {},
    completionV4() {},
    completionFinished() {
      aDone();
    },
  };
  if (aUserContextId !== undefined) {
    callback.userContextId = aUserContextId;
  }
  return callback;
}

add_setup(async function () {
  gServer = new HttpServer();
  gServer.registerPathHandler(SERVER_PATH, (aRequest, aResponse) => {
    aResponse.setStatusLine(null, 204, null);
  });
  gServer.start(-1);
  gGethashUrl =
    "http://localhost:" + gServer.identity.primaryPort + SERVER_PATH;
  Services.obs.addObserver(onModifyRequest, "http-on-modify-request");
  registerCleanupFunction(async () => {
    Services.obs.removeObserver(onModifyRequest, "http-on-modify-request");
    await new Promise(resolve => gServer.stop(resolve));
  });
});

add_task(async function test_a_completion_is_requested_in_its_context() {
  const completer = Cc["@mozilla.org/url-classifier/hashcompleter;1"].getService(
    Ci.nsIUrlClassifierHashCompleter
  );
  gRequests = [];

  // Four completions in one tick: contexts 3 and 7 (two prefixes), the
  // default context declared as 0, and a callback that declares no context.
  const calls = [
    ["1111", 3],
    ["2222", 7],
    ["3333", 7],
    ["4444", 0],
    ["5555", undefined],
  ];
  await Promise.all(
    calls.map(
      ([prefix, userContextId]) =>
        new Promise(resolve => {
          completer.complete(
            prefix,
            gGethashUrl,
            "test-phish-shavar",
            makeCallback(userContextId, resolve)
          );
        })
    )
  );

  const ids = gRequests.map(r => r.userContextId).sort((a, b) => a - b);
  Assert.deepEqual(
    ids,
    [0, 3, 7],
    "one request per context, each in its own context, none mixed"
  );
  for (const request of gRequests) {
    Assert.ok(
      request.firstPartyDomain.startsWith("safebrowsing."),
      "the Safe Browsing first party domain is kept"
    );
  }
});
