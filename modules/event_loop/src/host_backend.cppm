// Which readiness backend this host uses. One alias, so no caller has to carry
// an #ifdef between EpollBackend and KqueueBackend -- they have deliberately
// identical public shapes for exactly this reason.
//
// The alias is the only thing new code should name. EpollBackend remains
// spelled out at the existing call sites because it is also the concrete type
// the linux-only tests construct; where a site is genuinely platform-neutral it
// should move to HostReadinessBackend.
export module mbun.event_loop.host_backend;

import mbun.event_loop.epoll_backend;
import mbun.event_loop.kqueue_backend;

export namespace mbun::event_loop {

#if defined(__linux__)
using HostReadinessBackend = EpollBackend;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
using HostReadinessBackend = KqueueBackend;
#else
// Windows: neither exists. EpollBackend's non-linux path is the honest stub
// (status() == deferred, empty BackendSeam), so it stands in until an IOCP
// backend lands rather than failing to name a type at all.
using HostReadinessBackend = EpollBackend;
#endif

}  // namespace mbun::event_loop
