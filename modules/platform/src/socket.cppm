// Socket primitives whose spelling differs between Linux and Darwin.
//
// PORT-SOURCE:
//   bun packages/bun-usockets internal/networking/bsd.c -- bsd_create_socket /
//   bsd_accept_socket / apple_no_sigpipe, which exist for exactly these three.
//
// modules/runtime_socket is otherwise plain POSIX: socket, connect, bind,
// listen, recv, send, sockaddr_in/_un, TCP_NODELAY all behave the same on both.
// Only three things diverge, and they were the reason that whole 640-line file
// sat behind `#if defined(__linux__)`:
//
//   * SOCK_NONBLOCK|SOCK_CLOEXEC as socket() type bits is a Linux extension.
//   * accept4 is Linux-only.
//   * MSG_NOSIGNAL is a per-call send() flag on Linux; Darwin's equivalent is
//     the SO_NOSIGPIPE socket option, set once at creation.
//
// Resolving them here is what lets that file's guards widen to `!_WIN32`
// instead of being duplicated per platform.

module;

#if !defined(_WIN32)
#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

export module mbun.platform.socket;

import std;

export namespace mbun::platform::net {

// A non-blocking, close-on-exec stream socket. -1 with errno set on failure.
int stream_socket(int domain) noexcept;

// accept() a non-blocking, close-on-exec connection. -1 with errno set; EAGAIN
// / EWOULDBLOCK means the queue is drained, exactly as accept4 reports it.
int accept_stream(int listenFd) noexcept;

// send() that never raises SIGPIPE, however the platform spells that.
std::ptrdiff_t send_nosignal(int fd, const void* data, std::size_t size) noexcept;

}  // namespace mbun::platform::net

namespace mbun::platform::net {

#if defined(_WIN32)

int stream_socket(int) noexcept { errno = ENOSYS; return -1; }
int accept_stream(int) noexcept { errno = ENOSYS; return -1; }
std::ptrdiff_t send_nosignal(int, const void*, std::size_t) noexcept {
    errno = ENOSYS;
    return -1;
}

#else

namespace {

// Darwin has no atomic create-with-flags form, so the descriptor exists briefly
// without them. Harmless single-threaded, and it is what libuv and usockets do.
[[nodiscard]] bool make_nonblocking_cloexec(int fd) noexcept {
    const int fl{::fcntl(fd, F_GETFL, 0)};
    if (fl < 0 || ::fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) {
        return false;
    }
    const int fd_flags{::fcntl(fd, F_GETFD, 0)};
    return fd_flags >= 0 && ::fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC) >= 0;
}

// Darwin's answer to MSG_NOSIGNAL: a per-socket option, so it has to be armed
// at creation rather than passed to each send.
void arm_nosigpipe(int fd) noexcept {
#if defined(SO_NOSIGPIPE)
    const int on{1};
    static_cast<void>(::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on)));
#else
    static_cast<void>(fd);
#endif
}

}  // namespace

int stream_socket(int domain) noexcept {
#if defined(SOCK_NONBLOCK) && defined(SOCK_CLOEXEC)
    const int fd{::socket(domain, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};
    if (fd < 0) {
        return -1;
    }
#else
    const int fd{::socket(domain, SOCK_STREAM, 0)};
    if (fd < 0) {
        return -1;
    }
    if (!make_nonblocking_cloexec(fd)) {
        const int saved{errno};
        ::close(fd);
        errno = saved;
        return -1;
    }
#endif
    arm_nosigpipe(fd);
    return fd;
}

int accept_stream(int listenFd) noexcept {
#if defined(__linux__)
    return ::accept4(listenFd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
    const int fd{::accept(listenFd, nullptr, nullptr)};
    if (fd < 0) {
        return -1;  // caller distinguishes EAGAIN/EWOULDBLOCK from a real error
    }
    if (!make_nonblocking_cloexec(fd)) {
        const int saved{errno};
        ::close(fd);
        errno = saved;
        return -1;
    }
    arm_nosigpipe(fd);
    return fd;
#endif
}

std::ptrdiff_t send_nosignal(int fd, const void* data, std::size_t size) noexcept {
#if defined(MSG_NOSIGNAL)
    return ::send(fd, data, size, MSG_NOSIGNAL);
#else
    // SO_NOSIGPIPE was armed at creation, so a plain send is already safe.
    return ::send(fd, data, size, 0);
#endif
}

#endif

}  // namespace mbun::platform::net
