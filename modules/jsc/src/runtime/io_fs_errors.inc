// io_fs_errors.inc — the tail of io_bindings.inc's free functions.
//
// Extracted only to keep io_bindings.inc inside the 2000-line budget
// test_runtime_structure enforces (it stood at 2046). Included from
// io_bindings.inc at the point the functions used to occupy, so ordering and
// registration are unchanged.

JSValueRef fsn_utimes_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                         const JSValueRef args[], JSValueRef* exc) {
    return fsn_utimes_impl_(ctx, argc, args, exc, 0, "utime");
}
// node fs.lutimes / fs.lutimesSync: utimensat with AT_SYMLINK_NOFOLLOW, so the
// timestamps land on the SYMLINK rather than on whatever it points at. Without
// its own entry point the JS shim fell back to fsn_utimes_cb and followed the
// link (test-fs-utimes stamped the target and then failed lstat's mtime check).
JSValueRef fsn_lutimes_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                          const JSValueRef args[], JSValueRef* exc) {
    return fsn_utimes_impl_(ctx, argc, args, exc, AT_SYMLINK_NOFOLLOW, "lutime");
}
JSValueRef fsn_unlink_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                         const JSValueRef args[], JSValueRef* exc) {
    std::string path{fs_arg(ctx, argc, args, 0)};
    if (permission_deny_fs(ctx, exc, path, true)) return JSValueMakeUndefined(ctx);
    // std::filesystem::remove reports "nothing to remove" by returning FALSE with
    // an EMPTY error_code, so unlinkSync(<nonexistent>) silently succeeded — node
    // throws ENOENT. Go straight to unlink(2): it also keeps EISDIR/EPERM/EACCES
    // distinct, which `remove` (which happily rmdir's a directory) does not.
#if defined(_WIN32)
    std::error_code ec;
    if (!std::filesystem::remove(std::filesystem::path{path}, ec) || ec) {
        const int e{ec ? ec.value() : ENOENT};
        const std::string code{fd_errno_code(e)};
        return fs_throw(ctx, exc, (code.empty() ? std::string{"EPERM"} : code) + ": "
                                      + fd_errno_message(e) + ", unlink '" + path + "'");
    }
#else
    if (::unlink(path.c_str()) != 0) {
        const int e{errno};
        const std::string code{fd_errno_code(e)};
        return fs_throw(ctx, exc, (code.empty() ? std::string{"EPERM"} : code) + ": "
                                      + fd_errno_message(e) + ", unlink '" + path + "'");
    }
#endif
    return JSValueMakeUndefined(ctx);
}
// node fs.linkSync(existingPath, newPath): a HARD link, i.e. link(2). This was
// implemented as copy_file, which is not the same thing at all — the copy has
// its own inode, so nlink never rises, writes through one path are invisible
// through the other, and every error carried syscall 'copyfile' with the two
// paths mangled into one .path string.
JSValueRef fsn_link_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                       const JSValueRef args[], JSValueRef* exc) {
    const std::string oldPath{fs_arg(ctx, argc, args, 0)};
    const std::string newPath{fs_arg(ctx, argc, args, 1)};
    // node Link (node_file.cc): read(src), write(src), write(dest) — the source
    // is gated for WRITE too because a new hard link to it is a second writable
    // handle on the same inode.
    if (permission_deny_fs(ctx, exc, oldPath, false)) return JSValueMakeUndefined(ctx);
    if (permission_deny_fs(ctx, exc, oldPath, true)) return JSValueMakeUndefined(ctx);
    if (permission_deny_fs(ctx, exc, newPath, true)) return JSValueMakeUndefined(ctx);
#if defined(_WIN32)
    std::error_code ec;
    std::filesystem::create_hard_link(std::filesystem::path{oldPath},
                                      std::filesystem::path{newPath}, ec);
    if (ec) {
        const int e{ec.value()};
        const std::string code{fd_errno_code(e)};
        return fs_throw(ctx, exc, (code.empty() ? std::string{"EPERM"} : code) + ": "
                                      + fd_errno_message(e) + ", link '" + oldPath + "' -> '" + newPath + "'");
    }
#else
    if (::link(oldPath.c_str(), newPath.c_str()) != 0) {
        const int e{errno};
        const std::string code{fd_errno_code(e)};
        return fs_throw(ctx, exc, (code.empty() ? std::string{"EPERM"} : code) + ": "
                                      + fd_errno_message(e) + ", link '" + oldPath + "' -> '" + newPath + "'");
    }
#endif
    return JSValueMakeUndefined(ctx);
}
