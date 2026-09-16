# app_state

Application state machine and watchdog policy (TASK-115).

## Purpose

One explicit owner for the application lifecycle.  The component turns the
required boot sequence

    boot -> safe-off -> filesystem -> configuration -> Wi-Fi
         -> provisioning (only when no saved credential) -> verified MQTT/TLS
         -> state sync -> online

and every error path into a single state machine with:

- explicit states, legal transitions, events and **transition owners**
  (a callback from a context that does not own the transition is rejected),
- generation/session identity that rejects stale callbacks (a late Wi-Fi or
  MQTT callback from a previous reconnect episode can never drive the
  machine),
- a fail-off invariant: the lamp output is forced inactive on **every** entry
  into a non-online state, so "all failure paths leave the output off" is a
  property of the transition machinery itself,
- bounded retry/backoff for recoverable failures (exponential, capped,
  attempt-limited — no connection storms, no callback races),
- a watchdog policy with a **single feeding task**, documented feed points,
  timeout behavior and blocking constraints.

## States

`BOOT`, `FILESYSTEM`, `CONFIGURATION`, `NETWORK`, `PROVISIONING`, `TLS`,
`SYNC`, `ONLINE`, `SAFE_OFF` (degraded), `FATAL`, `OTA`.  The only path into
`ONLINE` requires successful filesystem + configuration + Wi-Fi (+ Wi-Fi
provisioning when no saved station credential exists — a device with saved
credentials passes straight through) + verified TLS + synchronization.  OTA
completion returns to `BOOT`; every error path converges on
`SAFE_OFF`/`FATAL` with the output off.

## Events and owners

FS results (`APP_EVENT_FS_OK/FAIL`), config results, network
connected/disconnected, provisioning started/succeeded/failed (when no
saved credential exists), verified-TLS connected/failed, sync
complete/failed/invalid-state, transport disconnect, OTA begin/end/failed,
external reset and fatal reports.  See `include/app_state.h` for the full
transition table and per-transition owners; the module validates the owner
of every delivery and drops (and counts) mismatches.

## Retry / backoff

Recoverable failures park in `SAFE_OFF` and schedule a retry back to the
exact failed stage — network (also after a provisioning failure, since the
provisioning adapter is owned by the network stage), TLS or sync.  Default
schedule: 2 s first delay, exponential 2x growth, 30 s cap, at most 5
retries per recovery episode; the budget resets when `ONLINE` is reached
again.  Exhaustion parks the machine silently — no retry storm is possible.
Non-retryable failures (filesystem, configuration, OTA) park degraded until
an explicit reset/provisioning/OTA action.

## Watchdog

- **Owner:** exactly one task — the integrator's supervisor loop, the only
  caller of `app_state_poll()`.
- **Feed:** every `app_state_poll()` call and every successful transition.
- **Timeout:** when the deadline is not refreshed within the configured
  window, the next poll forces the output inactive, fires the expiry
  callback and transitions to `FATAL`; recovery is an explicit reset.
- **Blocking constraints:** the owning task must not block in worker
  callbacks, every blocking call it makes must be bounded below the
  watchdog timeout, and wait loops that can outlive one poll interval must
  poll inside the loop (same task).

## Integration

`main/app_main.c` runs the boot gates through the machine in a supervisor
loop that owns watchdog feeding; the Wi-Fi wait loop polls `app_state`
inside each wait iteration.  Host tests (tests/app_state) drive the machine
with an injected clock, an observer and a lamp-control double for
deterministic fault injection of every failure transition, the reconnect
loop, timeouts, watchdog expiry/recovery and OTA entry/exit.