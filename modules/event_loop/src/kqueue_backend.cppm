// Darwin/BSD kqueue backend for the BackendSeam injection point (backend.cppm).
// Sibling of epoll_backend.cppm, same public shape, so host_backend.cppm can
// select between them without any caller learning which one it got.
//
// PORT-SOURCE:
//   bun packages/bun-usockets internal/eventing/epoll_kqueue.h -- the same file
//   implements both, which is why the interest/token vocabulary lines up.
//
// Three places kqueue does NOT line up with epoll, and each one is handled
// rather than papered over:
//
//   * read and write are SEPARATE filters (EVFILT_READ / EVFILT_WRITE), not
//     bits in one mask. read_write is two kevents, and dropping one direction
//     on modify() means an explicit EV_DELETE of that filter -- which is why
//     this class tracks the interest it registered per fd. epoll needs no such
//     bookkeeping because EPOLL_CTL_MOD replaces the mask wholesale.
//   * one fd can therefore report twice in a single wait (readable AND
//     writable). epoll delivers that as one event with two bits, so the tokens
//     are de-duplicated here to keep the seam's semantics identical.
//   * the wakeup needs no extra descriptor: EVFILT_USER + NOTE_TRIGGER is
//     kqueue's own mechanism, where epoll has to borrow an eventfd.
module;

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#define MBUN_HAS_KQUEUE 1
#include <cerrno>
#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#endif

export module mbun.event_loop.kqueue_backend;

import std;
import mbun.event_loop.backend;
import mbun.event_loop.epoll_backend;  // PollInterest

export namespace mbun::event_loop {

class KqueueBackend {
public:
    static constexpr std::uint64_t WAKE_TOKEN{std::numeric_limits<std::uint64_t>::max()};
    static constexpr std::size_t MAX_READY_POLLS{1024};

private:
#if defined(MBUN_HAS_KQUEUE)
    // Identifier for the EVFILT_USER wakeup. EVFILT_USER idents share no
    // namespace with fds, so any constant is safe; this one is distinctive in
    // a trace.
    static constexpr std::uintptr_t WAKE_IDENT{0xBEEF'0001ULL};
    int kqueueFd_{-1};
    // What we last registered per fd, so modify()/remove() can delete exactly
    // the filters that are actually armed. See the header note.
    std::unordered_map<int, PollInterest> armed_{};
#endif
    std::atomic<std::uint32_t> pendingWakeups_{0};

public:  // Big Five: fd owner, non-copyable/non-movable (seam captures this).
    KqueueBackend() {
#if defined(MBUN_HAS_KQUEUE)
        kqueueFd_ = ::kqueue();
        if (kqueueFd_ >= 0) {
            // No CLOEXEC handling to mirror epoll_create1(EPOLL_CLOEXEC): kqueue()
            // takes no flags, and a kqueue descriptor is not inherited across
            // fork at all, so there is nothing for a child to leak.
            struct ::kevent ev {};
            EV_SET(&ev, WAKE_IDENT, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0,
                   reinterpret_cast<void*>(static_cast<std::uintptr_t>(WAKE_TOKEN)));
            if (::kevent(kqueueFd_, &ev, 1, nullptr, 0, nullptr) != 0) {
                close_fd_();
            }
        }
#endif
    }
    KqueueBackend(const KqueueBackend&) = delete;
    KqueueBackend& operator=(const KqueueBackend&) = delete;
    KqueueBackend(KqueueBackend&&) = delete;
    KqueueBackend& operator=(KqueueBackend&&) = delete;
    ~KqueueBackend() {
#if defined(MBUN_HAS_KQUEUE)
        close_fd_();
#endif
    }

public:
    [[nodiscard]] BackendStatus status() const noexcept {
#if defined(MBUN_HAS_KQUEUE)
        return kqueueFd_ >= 0 ? BackendStatus::ready : BackendStatus::stopped;
#else
        return BackendStatus::deferred;
#endif
    }

    // A kqueue fd is itself pollable, exactly like an epoll fd, so the runtime's
    // idle pump can park over it instead of sleeping blind.
    [[nodiscard]] int backend_fd() const noexcept {
#if defined(MBUN_HAS_KQUEUE)
        return kqueueFd_;
#else
        return -1;
#endif
    }

    [[nodiscard]] bool add(int fd, PollInterest interest, std::uint64_t token) {
#if defined(MBUN_HAS_KQUEUE)
        if (kqueueFd_ < 0 || token == WAKE_TOKEN) {
            return false;
        }
        if (!apply_(fd, interest, token)) {
            return false;
        }
        armed_[fd] = interest;
        return true;
#else
        static_cast<void>(fd);
        static_cast<void>(interest);
        static_cast<void>(token);
        return false;
#endif
    }

    [[nodiscard]] bool modify(int fd, PollInterest interest, std::uint64_t token) {
#if defined(MBUN_HAS_KQUEUE)
        if (kqueueFd_ < 0 || token == WAKE_TOKEN) {
            return false;
        }
        // Drop whichever direction is being given up. EV_ADD on the directions
        // that remain is idempotent and also re-binds udata, so only removals
        // need explicit work -- this is the bookkeeping epoll's CTL_MOD hides.
        if (const auto it{armed_.find(fd)}; it != armed_.end()) {
            const bool hadRead{it->second != PollInterest::write};
            const bool hadWrite{it->second != PollInterest::read};
            const bool wantRead{interest != PollInterest::write};
            const bool wantWrite{interest != PollInterest::read};
            if (hadRead && !wantRead) {
                drop_(fd, EVFILT_READ);
            }
            if (hadWrite && !wantWrite) {
                drop_(fd, EVFILT_WRITE);
            }
        }
        if (!apply_(fd, interest, token)) {
            return false;
        }
        armed_[fd] = interest;
        return true;
#else
        static_cast<void>(fd);
        static_cast<void>(interest);
        static_cast<void>(token);
        return false;
#endif
    }

    [[nodiscard]] bool remove(int fd) {
#if defined(MBUN_HAS_KQUEUE)
        if (kqueueFd_ < 0) {
            return false;
        }
        const auto it{armed_.find(fd)};
        if (it == armed_.end()) {
            return false;
        }
        if (it->second != PollInterest::write) {
            drop_(fd, EVFILT_READ);
        }
        if (it->second != PollInterest::read) {
            drop_(fd, EVFILT_WRITE);
        }
        armed_.erase(it);
        return true;
#else
        static_cast<void>(fd);
        return false;
#endif
    }

    void wake() {
        pendingWakeups_.fetch_add(1, std::memory_order_release);
#if defined(MBUN_HAS_KQUEUE)
        if (kqueueFd_ >= 0) {
            struct ::kevent ev {};
            EV_SET(&ev, WAKE_IDENT, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
            int rc{-1};
            do {
                rc = ::kevent(kqueueFd_, &ev, 1, nullptr, 0, nullptr);
            } while (rc < 0 && errno == EINTR);
        }
#endif
    }

    // One kevent pass. timeout < 0 blocks until an event or wake().
    [[nodiscard]] std::vector<BackendEvent> poll(std::chrono::milliseconds timeout) {
        std::vector<BackendEvent> ready{};
#if defined(MBUN_HAS_KQUEUE)
        if (kqueueFd_ < 0) {
            return ready;
        }
        // Same rule as Loop.rs and the epoll sibling: a wake() that landed
        // before the wait must stop it blocking.
        bool immediate{pendingWakeups_.exchange(0, std::memory_order_acquire) != 0};
        struct ::timespec ts {};
        const struct ::timespec* tsPtr{nullptr};
        if (immediate) {
            ts = {.tv_sec = 0, .tv_nsec = 0};
            tsPtr = &ts;
        } else if (timeout >= std::chrono::milliseconds{0}) {
            const auto ms{timeout.count()};
            ts.tv_sec = static_cast<std::time_t>(ms / 1000);
            ts.tv_nsec = static_cast<long>((ms % 1000) * 1000000);
            tsPtr = &ts;
        }

        std::array<struct ::kevent, MAX_READY_POLLS> events{};
        int count{-1};
        do {
            count = ::kevent(kqueueFd_, nullptr, 0, events.data(),
                             static_cast<int>(events.size()), tsPtr);
        } while (count < 0 && errno == EINTR);
        if (count <= 0) {
            return ready;
        }

        // De-duplicate: one fd readable AND writable is two kevents here but a
        // single two-bit event under epoll, and the seam must not be able to
        // tell which backend produced it.
        ready.reserve(static_cast<std::size_t>(count));
        std::array<std::uint64_t, MAX_READY_POLLS> seen{};
        std::size_t seenCount{0};
        for (int i{0}; i < count; ++i) {
            const auto& ev{events[static_cast<std::size_t>(i)]};
            const auto token{static_cast<std::uint64_t>(
                reinterpret_cast<std::uintptr_t>(ev.udata))};
            if (ev.filter == EVFILT_USER || token == WAKE_TOKEN) {
                continue;  // EV_CLEAR armed it; nothing to drain, unlike eventfd
            }
            bool duplicate{false};
            for (std::size_t j{0}; j < seenCount; ++j) {
                if (seen[j] == token) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) {
                continue;
            }
            seen[seenCount++] = token;
            ready.push_back(BackendEvent{.token = token});
        }
#else
        static_cast<void>(timeout);
#endif
        return ready;
    }

    [[nodiscard]] BackendSeam seam() {
#if defined(MBUN_HAS_KQUEUE)
        if (kqueueFd_ < 0) {
            return BackendSeam{};
        }
        return BackendSeam{
            [this](std::chrono::milliseconds timeout) { return poll(timeout); },
            [this] { wake(); }};
#else
        return BackendSeam{};
#endif
    }

#if defined(MBUN_HAS_KQUEUE)
private:
    // EV_ADD both directions requested. Idempotent, and re-binds udata, so it
    // serves add() and the additive half of modify() alike.
    [[nodiscard]] bool apply_(int fd, PollInterest interest, std::uint64_t token) {
        std::array<struct ::kevent, 2> changes{};
        int n{0};
        void* const udata{reinterpret_cast<void*>(static_cast<std::uintptr_t>(token))};
        if (interest != PollInterest::write) {
            EV_SET(&changes[static_cast<std::size_t>(n++)], static_cast<std::uintptr_t>(fd),
                   EVFILT_READ, EV_ADD, 0, 0, udata);
        }
        if (interest != PollInterest::read) {
            EV_SET(&changes[static_cast<std::size_t>(n++)], static_cast<std::uintptr_t>(fd),
                   EVFILT_WRITE, EV_ADD, 0, 0, udata);
        }
        int rc{-1};
        do {
            rc = ::kevent(kqueueFd_, changes.data(), n, nullptr, 0, nullptr);
        } while (rc < 0 && errno == EINTR);
        return rc == 0;
    }

    // Best-effort: ENOENT just means it was not armed, which is not an error
    // for a caller that is taking the fd away.
    void drop_(int fd, std::int16_t filter) noexcept {
        struct ::kevent ev {};
        EV_SET(&ev, static_cast<std::uintptr_t>(fd), filter, EV_DELETE, 0, 0, nullptr);
        int rc{-1};
        do {
            rc = ::kevent(kqueueFd_, &ev, 1, nullptr, 0, nullptr);
        } while (rc < 0 && errno == EINTR);
    }

    void close_fd_() noexcept {
        if (kqueueFd_ >= 0) {
            ::close(kqueueFd_);
            kqueueFd_ = -1;
        }
        armed_.clear();
    }
#endif
};

}  // namespace mbun::event_loop
