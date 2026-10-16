# Changelog

## 0.2.0 — 2026-10-16

### Added

- `DDHUB_ASYNC_PROCESS_ENABLE`: choose direct synchronous send (`0`) or queued asynchronous dispatch (`1`, default).
- One optional global `ddhub_set_process_notify()` hook for application scheduling after successful enqueue.
- Live registration during dispatch. New subscriptions participate from the next message, preserving priority and registration order.
- Safe removal of inactive subscriptions while another callback is running. Only the node still pinned by dispatch is protected from removal.
- Tests for both modes, C++ interoperability, generation rebasing, controlled concurrency, final-check handoff, and 2000 messages from multiple producers using a retained binary wakeup.

### Changed

- **Breaking API change:** replace `ddhub_process(hub_id)` and `ddhub_process_all()` with `ddhub_process(void)` in asynchronous builds.
- `ddhub_process()` drains all hubs by finite rounds, including messages published during callbacks, until the final locked check finds every queue empty.
- Synchronous builds dispatch in send and compile out FIFO storage, the process API, and its notification configuration API.
- List-writer reservations no longer block enqueue, dequeue, or process admission. Link publication/removal and traversal cursor handoff share platform exclusion.
- Updated Linux, Windows and STM32 examples and English/Chinese usage documentation for both modes.

### Migration

1. Keep the default asynchronous mode to retain queued sends; replace either old processing call with `ddhub_process()` on one consumer.
2. Optionally bind a global notify hook during startup after binding the lock adapter. Keep wakeups that arrive while processing; do not reset the wakeup primitive after process returns.
3. Set `DDHUB_ASYNC_PROCESS_ENABLE=0` consistently for the component and callers to use synchronous dispatch, and remove process/notify calls from that build.
4. Prefer asynchronous mode for ISR sends. Synchronous callbacks inherit the sender's context, and same-hub overlapping or recursive sends return `DDHUB_BUSY`.
5. Continuous producers may keep a drain call running indefinitely. `DDHUB_OK` describes the final locked empty check; later sends schedule new work.

## 0.1.0

- Initial release with static hub queues, intrusive subscriptions, event-mask dispatch, completion callbacks, and injected platform locking.
