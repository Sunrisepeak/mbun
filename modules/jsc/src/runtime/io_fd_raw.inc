// io_fd_raw.inc — fd-level read/write on a descriptor the virtual fd table does
// not own.
//
// mbun hands JS a VIRTUAL descriptor (fd_table(), numbered from 1000) for every
// fd it opens itself, and fs.readSync/writeSync used to reject anything absent
// from that table with EBADF. But a process also holds descriptors it never
// opened: the fd of an "ipc" stdio slot, or anything else a parent handed down.
// node writes to those happily -- `fs.writeSync(process.channel.fd, buf)` is
// exactly what test-child-process-fork-advanced-header-serialization does -- so
// EBADF there is wrong. fsn_fstat_cb already falls back to the raw descriptor
// for the same reason; this is its read/write counterpart.
//
// Extracted into its own slice only to keep io_bindings.inc inside the 2000-line
// budget test_runtime_structure enforces, exactly as io_fs_errors.inc was.
// Included from io_bindings.inc immediately before fd_io_args, its only caller, so
// the fd_throw_* helpers are already visible and ordering is otherwise unchanged.

// Is `fd` a descriptor that can only be an un-owned OS one?
//
// The window is stderr (2), or 3 <= fd < 1000, and both bounds are load-bearing.
//
// Upper: virtual fds start AT 1000, and under load (Bun.spawn's 250-process
// pipe-leak test) the OS descriptor table climbs past 1000 too. Without the cap,
// a virtual fd that was closed and erased from the table would resolve to a LIVE,
// unrelated OS descriptor of the same number instead of the EBADF its caller is
// waiting for.
//
// Lower: stderr (2) must be usable by node diagnostics and test fixtures. stdout
// and stdin remain deferred: sending fs.writeSync(1, …) through the raw descriptor
// also uncovers an unbounded buffering defect in the Bun.spawn 'pipe' reader: a
// child that pushes 8 MiB into an unread pipe grew the parent past 1.3 GB until
// fork() failed (compat/bun spawn-pipe-leak passed only because its child died on
// EBADF). That reader is a separate job; keep fd 1/0 closed to this fallback until
// its buffering is bounded.
bool fd_raw_eligible(int fd) {
#if defined(_WIN32)
    (void)fd;
    return false;
#else
    return (fd == 2 || (fd >= 3 && fd < 1000)) && ::fcntl(fd, F_GETFD) != -1;
#endif
}

// One read/write syscall on such a descriptor. Same reporting contract as the
// io::File paths in io_bindings.inc: a would-block is backpressure (-1), not an
// error, and a real failure throws the node-shaped, path-less fd error.
JSC::EncodedJSValue fd_raw_io(JSC::JSGlobalObject* global, int fd, void* base, std::size_t len,
                              double pos, bool write, const char* syscall) {
#if defined(_WIN32)
    (void)fd;
    (void)base;
    (void)len;
    (void)pos;
    (void)write;
    return fd_throw_errno_nopath(global, EBADF, syscall);
#else
    const off_t at{static_cast<off_t>(pos)};
    ssize_t n{};
    if (write) {
        n = pos >= 0 ? ::pwrite(fd, base, len, at) : ::write(fd, base, len);
    } else {
        n = pos >= 0 ? ::pread(fd, base, len, at) : ::read(fd, base, len);
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return JSC::JSValue::encode(JSC::jsNumber(-1));
        return fd_throw_errno(global, errno, syscall, "");
    }
    return JSC::JSValue::encode(JSC::jsNumber(static_cast<double>(n)));
#endif
}
