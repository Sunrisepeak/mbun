// Linux epoll backend for the BackendSeam injection point (backend.cppm).
//
// PORT-SOURCE:
//   bun Rust src/uws_sys/Loop.rs (PosixLoop: fd, ready_polls[1024],
//   pending_wakeups swapped to 0 before epoll_wait — non-zero means return
//   immediately) and packages/bun-usockets internal/eventing/epoll_kqueue.h
//   (us_wakeup_loop via eventfd, us_poll_start/change/stop via epoll_ctl).
//
// Non-Linux builds keep the honest DEFERRED stub style of
// modules/jsc/src/runtime/net.inc: registration returns false and seam()
// yields a deferred (empty) BackendSeam.
module;

#if defined(__linux__)
#include <cerrno>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#endif

export module mbun.event_loop.epoll_backend;

import std;
import mbun.event_loop.backend;

export namespace mbun::event_loop {

enum class PollInterest : std::uint8_t { read, write, read_write };

class EpollBackend {
public:
    // Sentinel token owned by the wakeup eventfd; user tokens must differ.
    static constexpr std::uint64_t WAKE_TOKEN{std::numeric_limits<std::uint64_t>::max()};
    // Mirrors PosixLoop::ready_polls[1024] in Loop.rs.
    static constexpr std::size_t MAX_READY_POLLS{1024};

private:
#if defined(__linux__)
    int epollFd_{-1};
    int wakeFd_{-1};
#endif
    // Mirrors PosixLoop::pending_wakeups: bumped by wake(), swapped to 0
    // before epoll_wait; non-zero forces an immediate (0ms) poll.
    std::atomic<std::uint32_t> pendingWakeups_{0};

public:  // Big Five: fd owner, non-copyable/non-movable (seam captures this).
    EpollBackend() {
#if defined(__linux__)
        epollFd_ = ::epoll_create1(EPOLL_CLOEXEC);
        wakeFd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (epollFd_ >= 0 && wakeFd_ >= 0) {
            ::epoll_event ev{};
            ev.events = EPOLLIN;
            ev.data.u64 = WAKE_TOKEN;
            if (::epoll_ctl(epollFd_, EPOLL_CTL_ADD, wakeFd_, &ev) != 0) {
                close_fds_();
            }
        } else {
            close_fds_();
        }
#endif
    }
    EpollBackend(const EpollBackend&) = delete;
    EpollBackend& operator=(const EpollBackend&) = delete;
    EpollBackend(EpollBackend&&) = delete;
    EpollBackend& operator=(EpollBackend&&) = delete;
    ~EpollBackend() {
#if defined(__linux__)
        close_fds_();
#endif
    }

public:
    [[nodiscard]] BackendStatus status() const noexcept {
#if defined(__linux__)
        return epollFd_ >= 0 ? BackendStatus::ready : BackendStatus::stopped;
#else
        return BackendStatus::deferred;
#endif
    }

    // The backend's own epoll fd (-1 when unavailable). An epoll fd is itself
    // pollable — readable while events are queued — so an outer poll() set can
    // include it to wake instantly on reactor traffic (the runtime's idle pump
    // parks in poll() over net fds + this fd instead of a blind sleep).
    [[nodiscard]] int backend_fd() const noexcept { return epollFd_; }

    // us_poll_start: register fd with an interest set and a routing token.
    [[nodiscard]] bool add(int fd, PollInterest interest, std::uint64_t token) {
#if defined(__linux__)
        if (epollFd_ < 0 || token == WAKE_TOKEN) {
            return false;
        }
        ::epoll_event ev{};
        ev.events = events_for_(interest);
        ev.data.u64 = token;
        return ::epoll_ctl(epollFd_, EPOLL_CTL_ADD, fd, &ev) == 0;
#else
        static_cast<void>(fd);
        static_cast<void>(interest);
        static_cast<void>(token);
        return false;  // DEFERRED: no non-Linux backend yet.
#endif
    }

    // us_poll_change: swap the interest set (token may be re-bound too).
    [[nodiscard]] bool modify(int fd, PollInterest interest, std::uint64_t token) {
#if defined(__linux__)
        if (epollFd_ < 0 || token == WAKE_TOKEN) {
            return false;
        }
        ::epoll_event ev{};
        ev.events = events_for_(interest);
        ev.data.u64 = token;
        return ::epoll_ctl(epollFd_, EPOLL_CTL_MOD, fd, &ev) == 0;
#else
        static_cast<void>(fd);
        static_cast<void>(interest);
        static_cast<void>(token);
        return false;  // DEFERRED
#endif
    }

    // us_poll_stop: deregister fd.
    [[nodiscard]] bool remove(int fd) {
#if defined(__linux__)
        if (epollFd_ < 0) {
            return false;
        }
        return ::epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, nullptr) == 0;
#else
        static_cast<void>(fd);
        return false;  // DEFERRED
#endif
    }

    // us_wakeup_loop: bump pending_wakeups, then kick the eventfd so a
    // blocked epoll_wait returns immediately.
    void wake() {
        pendingWakeups_.fetch_add(1, std::memory_order_release);
#if defined(__linux__)
        if (wakeFd_ >= 0) {
            const std::uint64_t one{1};
            ssize_t written{-1};
            do {
                written = ::write(wakeFd_, &one, sizeof(one));
            } while (written < 0 && errno == EINTR);
        }
#endif
    }

    // One epoll_wait pass. timeout < 0 blocks until an event or wake().
    [[nodiscard]] std::vector<BackendEvent> poll(std::chrono::milliseconds timeout) {
        std::vector<BackendEvent> ready{};
#if defined(__linux__)
        if (epollFd_ < 0) {
            return ready;
        }
        // Loop.rs: pending_wakeups is swapped to 0 before epoll/kqueue; if it
        // was non-zero the wait must not block.
        int waitMs{timeout < std::chrono::milliseconds{0}
                       ? -1
                       : static_cast<int>(std::min<std::int64_t>(
                             timeout.count(), std::numeric_limits<int>::max()))};
        if (pendingWakeups_.exchange(0, std::memory_order_acquire) != 0) {
            waitMs = 0;
        }
        std::array<::epoll_event, MAX_READY_POLLS> events{};
        int count{-1};
        do {
            count = ::epoll_wait(epollFd_, events.data(),
                                 static_cast<int>(events.size()), waitMs);
        } while (count < 0 && errno == EINTR);
        if (count <= 0) {
            return ready;
        }
        ready.reserve(static_cast<std::size_t>(count));
        for (int i{0}; i < count; ++i) {
            const std::uint64_t token{events[static_cast<std::size_t>(i)].data.u64};
            if (token == WAKE_TOKEN) {
                drain_wake_fd_();
                continue;
            }
            ready.push_back(BackendEvent{.token = token});
        }
#else
        static_cast<void>(timeout);  // DEFERRED: deferred stub never blocks.
#endif
        return ready;
    }

    // Bind this backend into the platform-neutral seam. The backend must
    // outlive the seam (and any EventLoop holding it).
    [[nodiscard]] BackendSeam seam() {
#if defined(__linux__)
        if (epollFd_ < 0) {
            return BackendSeam{};
        }
        return BackendSeam{
            [this](std::chrono::milliseconds timeout) { return poll(timeout); },
            [this] { wake(); }};
#else
        return BackendSeam{};  // DEFERRED: poll_once() reports deferred.
#endif
    }

#if defined(__linux__)
private:
    static std::uint32_t events_for_(PollInterest interest) noexcept {
        switch (interest) {
            case PollInterest::read: return EPOLLIN;
            case PollInterest::write: return EPOLLOUT;
            case PollInterest::read_write: return EPOLLIN | EPOLLOUT;
        }
        return EPOLLIN;
    }

    void drain_wake_fd_() const noexcept {
        std::uint64_t counter{0};
        ssize_t got{-1};
        do {
            got = ::read(wakeFd_, &counter, sizeof(counter));
        } while (got < 0 && errno == EINTR);
    }

    void close_fds_() noexcept {
        if (wakeFd_ >= 0) {
            ::close(wakeFd_);
            wakeFd_ = -1;
        }
        if (epollFd_ >= 0) {
            ::close(epollFd_);
            epollFd_ = -1;
        }
    }
#endif
};

}  // namespace mbun::event_loop
