// io_memfd.inc — bun:internal-for-testing memfd_create(size) → fd.
//
// An anonymous memory-backed REGULAR file of `size` bytes, adopted into the
// virtual fd table so writeSync / readFileSync / closeSync take it like any
// other fd.
//
// Why a memfd rather than a temp file or /dev/zero: the caller needs a file
// whose fstat reports a real st_size (/dev/zero reports 0) so that
// fs.readFileSync sizes its allocation from it and can be driven into the
// synthetic allocation limit deterministically, without touching the disk.
// node/fs/fs-oom.test.ts builds a 16 MiB one and then lowers the limit to 2 MiB
// to assert the ENOMEM is catchable instead of aborting the process.
//
// Its own slice because io_bindings.inc sits against the 2000-line structural
// budget test_runtime_structure enforces. Included from io_bindings.inc after
// the fd helpers (fd_table / fd_num / fd_throw*) it uses.
//
// Linux only, exactly as in bun (memfd_create(2) is a Linux syscall).

JSC_DEFINE_HOST_FUNCTION(memfd_create_host, (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
#if defined(__linux__) && defined(__NR_memfd_create)
    const double size{frame->argumentCount() > 0 ? fd_num(global, frame->argument(0)) : 0.0};
    const long raw{::syscall(__NR_memfd_create, "mbun-test", 0)};
    if (raw < 0) return fd_throw_errno_nopath(global, errno, "memfd_create");
    const int native{static_cast<int>(raw)};
    if (size > 0.0 && ::ftruncate(native, static_cast<off_t>(size)) != 0) {
        const int err{errno};
        (void)::close(native);
        return fd_throw_errno_nopath(global, err, "ftruncate");
    }
    auto& tbl{fd_table()};
    int fd{1000};
    while (tbl.contains(fd)) ++fd;
    tbl.emplace(fd, mbun::core::io::File::adopt(native));
    return JSC::JSValue::encode(JSC::jsNumber(fd));
#else
    (void)frame;
    return fd_throw(global, "ENOSYS: memfd_create is only supported on Linux");
#endif
}
