# stillborn/telemetry

Category: stillborn
Prefs: none read directly
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `toolkit/components/telemetry/app/TelemetrySend.sys.mjs` (sendStandalonePing (module-level, hunk 2); TelemetrySendImpl.runPingSender (hunks 3 and 4); new module-level function sessionRoute() at end of file, movable): Category is stillborn because it reroutes Firefox's own telemetry pings (rule: reroute or silence of phone-home traffic). The standalone pingsender executable path and the Nimbus gate are removed, so the firefox --backgroundtask path is always used. Pings now carry the session proxy from Juggler's TargetRegistry and the profile prefs.js path, passed through an environment variable set around runAs
- `toolkit/components/telemetry/pings/BackgroundTask_pingsender.sys.mjs` (runBackgroundTask (export async function, hunk 1); sendPing (module-level async function, hunks 2 and 3); new takeSessionRoute() at file level, movable): takeSessionRoute reads the env variable, reads the session prefs.js into this empty profile, and registers a proxy channel filter at priority 0. It refuses to send when the env variable is missing (no fallback), which is the intended fail-closed behaviour. It depends on resource://gre/modules/juggler/SessionRoute.js, which is not in this list. The largest logic block in this group: move takeSessio
