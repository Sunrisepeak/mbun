# mbun.event_loop

Platform-neutral event-loop foundation for tasks, FIFO queues, timers,
cancellation, worker-thread dispatch, and backend injection.

The structure follows Bun's Rust `event_loop`/`threading` crates with Zig
`event_loop`/`threading` as the algorithm cross-check. It is an MC++
re-implementation, not a source copy. `BackendSeam` is intentionally a seam:
libuv, epoll/kqueue, IOCP, native handles, and JSC wiring are DEFERRED.

Modules are split by responsibility and kept below the repository's 2000-line
source-file limit. The existing `modules/jsc` event loop remains independent.
