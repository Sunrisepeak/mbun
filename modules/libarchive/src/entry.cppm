// Archive entry metadata, ported from bun's libarchive::Entry surface.
// ref: .mbun/bun-ref/src/libarchive/lib.rs (Entry) and
//      .mbun/bun-ref/src/runtime/api/Archive.rs (regular-file metadata).
export module mbun.libarchive.entry;

import std;

export namespace mbun::libarchive {

enum class FileType : std::uint32_t {
    Regular = 0100000,
    Directory = 0040000,
    Symlink = 0120000,
    Other = 0,
};

struct Entry {
    std::string pathname;
    std::string symlink;
    std::int64_t size { 0 };
    FileType fileType { FileType::Regular };
    std::uint32_t permissions { 0644 };
    std::int64_t mtimeSeconds { 0 };
    std::int32_t mtimeNanoseconds { 0 };

    void clear() {
        pathname.clear();
        symlink.clear();
        size = 0;
        fileType = FileType::Regular;
        permissions = 0644;
        mtimeSeconds = 0;
        mtimeNanoseconds = 0;
    }

    void set_pathname(std::string_view value) { pathname = value; }
    void set_size(std::int64_t value) { size = value; }
    void set_filetype(FileType value) { fileType = value; }
    void set_perm(std::uint32_t value) { permissions = value; }
    void set_mtime(std::int64_t seconds, std::int32_t nanoseconds = 0) {
        mtimeSeconds = seconds;
        mtimeNanoseconds = nanoseconds;
    }
};

}  // namespace mbun::libarchive
