// POSIX stat -> FsStatFields, split out of io_bindings.inc when that file hit
// the 2000-line budget test_runtime_structure enforces. It is one cohesive
// unit: the Darwin/glibc field-name divergence and the birthtime rules are
// only meaningful together.
#if !defined(_WIN32)
// Darwin names the nanosecond stat timestamps st_*timespec; POSIX.1-2008 (what
// glibc exposes) names them st_*tim. Both are `struct timespec`, so one accessor
// per field keeps the arithmetic below identical on either.
[[nodiscard]] inline const struct timespec& stat_atim(const struct stat& st) noexcept {
#if defined(__APPLE__)
    return st.st_atimespec;
#else
    return st.st_atim;
#endif
}
[[nodiscard]] inline const struct timespec& stat_mtim(const struct stat& st) noexcept {
#if defined(__APPLE__)
    return st.st_mtimespec;
#else
    return st.st_mtim;
#endif
}
[[nodiscard]] inline const struct timespec& stat_ctim(const struct stat& st) noexcept {
#if defined(__APPLE__)
    return st.st_ctimespec;
#else
    return st.st_ctim;
#endif
}
[[nodiscard]] inline long long ts_ns(const struct timespec& t) noexcept {
    return static_cast<long long>(t.tv_sec) * 1000000000LL + t.tv_nsec;
}
[[nodiscard]] inline double ts_ms(const struct timespec& t) noexcept {
    return static_cast<double>(t.tv_sec) * 1000.0 + static_cast<double>(t.tv_nsec) / 1e6;
}
// Fill FsStatFields from a POSIX struct stat. birthtime: macOS carries the real
// creation time in struct stat itself; Linux has no such field, so it falls back
// to ctime here and the callers overwrite it via statx.
void fs_fields_from_stat(const struct stat& st, FsStatFields& f) {
    f.dev = static_cast<long long>(st.st_dev);
    f.mode = static_cast<long long>(st.st_mode);
    f.nlink = static_cast<long long>(st.st_nlink);
    f.uid = static_cast<long long>(st.st_uid);
    f.gid = static_cast<long long>(st.st_gid);
    f.rdev = static_cast<long long>(st.st_rdev);
    f.ino = static_cast<long long>(st.st_ino);
    f.size = static_cast<long long>(st.st_size);
    f.blksize = static_cast<long long>(st.st_blksize);
    f.blocks = static_cast<long long>(st.st_blocks);
    f.atimeNs = ts_ns(stat_atim(st));
    f.mtimeNs = ts_ns(stat_mtim(st));
    f.ctimeNs = ts_ns(stat_ctim(st));
    f.atimeMs = ts_ms(stat_atim(st));
    f.mtimeMs = ts_ms(stat_mtim(st));
    f.ctimeMs = ts_ms(stat_ctim(st));
#if defined(__APPLE__)
    // Real btime, straight out of the same stat() -- no second syscall, and
    // statx does not exist here.
    f.birthtimeNs = ts_ns(st.st_birthtimespec);
    f.birthtimeMs = ts_ms(st.st_birthtimespec);
#else
    f.birthtimeMs = f.ctimeMs;  // overwritten by statx btime when available
    f.birthtimeNs = f.ctimeNs;
#endif
}
#endif
