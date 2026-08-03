// Aggregator for the platform-neutral event-loop foundation.
export module mbun.event_loop;

export import mbun.event_loop.backend;
export import mbun.event_loop.cancellation;
export import mbun.event_loop.epoll_backend;
export import mbun.event_loop.kqueue_backend;
export import mbun.event_loop.host_backend;
export import mbun.event_loop.queue;
export import mbun.event_loop.task;
export import mbun.event_loop.thread;
export import mbun.event_loop.timer;
export import mbun.event_loop.loop;
