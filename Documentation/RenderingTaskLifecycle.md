# Rendering task lifecycle regression

## Root cause

`EventLoop::update_the_rendering()` owns the rendering-task lifecycle. It verifies
that no rendering task is active, sets `m_running_rendering_task`, starts the
page-client rendering-update notifications, advances the scheduler counters and
serial, takes clock lanes and input events, then finishes the update through
`finish_rendering_update()`. A rendering update may temporarily end its task
while style or layout flies; that transition is intentional and is paired with
the in-flight update bookkeeping.

The extraction in `bd2773cfb1` (`LibWeb+WebContent: Handle button and key
presses before the next frame`) moved lane and input dispatch into
`take_clock_lanes_and_input_events_in()`, but copied the rendering lifecycle
startup into that helper. The regular rendering callback already starts that
lifecycle before calling the helper, so every scheduled rendering opportunity
entered it with `m_running_rendering_task == true` and failed its
`VERIFY(!m_running_rendering_task)`. The new input-only task also called the
helper and could leave the flag set after that task returned, making a later
rendering opportunity fail as well.

The preceding revision, `3a5e309c7f5759b732ea61a8d24fbca14e55a9f6`, kept lane
and input dispatch inline under the single lifecycle owned by
`update_the_rendering()`. It did not contain the duplicate lifecycle startup.

## Scheduled execution path

`request_rendering_update()` records/coalesces a request. On a rendering
opportunity, `rendering_opportunity()` updates the timestamp, declines to queue
while a rendering task is active, and otherwise checks for an eligible local
navigable. It sets `m_rendering_task_queued` and enqueues the shared rendering
callback once. The callback verifies the queued state, clears it, and calls
`update_the_rendering()`.

That function verifies the running state is clear, sets it, sends each local
page's `will_begin_rendering_update()`, advances counters and the serial, and
finishes any eligible prior in-flight update before taking clock lanes and
input events. The helper seals/takes clock plans using the supplied serial and
dispatches pending input; it does not alter lifecycle state. The rendering
steps then run resize, scroll, animation and layout work. A synchronous update
ends through `finish_rendering_update()`, which clears the running flag and
sends `did_finish_rendering_update()`. When style/layout is allowed to fly, the
task flag is deliberately cleared at that handoff and completion is tracked by
`RenderingUpdateInFlight`; resumption uses a scoped running guard. A new
opportunity may be queued during that handoff, but its callback still passes
through the same `VERIFY` before entering a new update.

Input-only dispatch is queued on the user-interaction task source. It clears its
queued marker, skips if a rendering task or in-flight update owns the documents,
and otherwise holds the running flag only for the duration of input dispatch.
That prevents input handlers from reentering rendering while ensuring the next
render opportunity starts with the invariant restored.

## Corrected ownership and invariants

`take_clock_lanes_and_input_events_in()` now only takes clock lanes and
dispatches input events. A full rendering opportunity starts and completes the
rendering lifecycle in `update_the_rendering()` / `finish_rendering_update()`.
The input-only task skips work while a render task or in-flight update owns the
documents, then uses a scoped `TemporaryChange` guard while dispatching input.
That guard blocks reentrant rendering while input handlers run script, and
restores the prior state on every return path. It does not start page-client
render notifications, increment rendering counters or advance the serial.

The in-flight style/layout handoff remains unchanged: the task can end before
the asynchronous work completes, and completion remains recorded by the
existing `RenderingUpdateInFlight` path. No state is cleared to conceal an
active update, and no compositor or presentation code changes are involved.

## Regression coverage

`TestEventLoop` exercises the real queued-rendering callback twice in succession
and verifies that each opportunity consumes the queued flag and leaves the
running flag clear. It also invokes the input-only task twice and verifies that
the scoped guard does not leak into the next opportunity. These cases cover the
invalid double-entry and stale-running-state transitions without leaving test
tasks in the shared event queue.

## Validation

- `ninja -C build/engine-release TestEventLoop`
- `ctest --test-dir build/engine-release -R '^TestEventLoop$' --output-on-failure`
- `./photon build`
- `./photon test`
- `./photon check`
- Isolated Photon automation navigation to `https://www.iana.org/help/example-domains`
  reached `engine_load_finished: true` with no error; closing its tab shut down
  Photon cleanly.
- Isolated Photon automation session against a local page that repeatedly
  scrolled and changed layout; opened, activated and closed a second tab; then
  closed the workload tab through normal lifecycle. The Photon log had no
  `VERIFICATION FAILED` or `WebContent process crashed` markers.

The automation protocol has no native window-resize or wheel-input command, so
the automated run does not claim to cover a user-driven native resize or wheel
gesture. It exercises repeated rendering opportunities, script-driven scroll
and layout changes, real navigation, tab switching, and orderly shutdown.

The fix is in the Engine submodule only. It is separate from Photon's
presentation-strategy refactor; the assertion is reached in LibWeb's scheduled
task path before Photon frame presentation is relevant, and the regression is
covered without changing or mocking a presentation backend.
