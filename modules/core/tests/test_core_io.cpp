// test_core_io.cpp — T1.4 mbun.core.io low-level filesystem contract.
//
// This is a C++ contract suite for bun's pure sys/io layer, not the later
// JavaScript node:fs or Bun.file/Bun.write bindings. Sources:
//   - bun Rust: src/sys/{file.rs,Error.rs,PosixStat.rs,tmp.rs,lib.rs}
//   - bun Zig:  src/sys/{File.zig,Error.zig,PosixStat.zig,tmp.zig,sys.zig}
//   - test/js/bun/io/bun-write.test.js > large file / not found / empty file
//   - test/js/node/fs/fs.test.ts > append / truncate / partial write blocks
//
// Adaptation: JS Buffer/string values are represented as byte spans. The
// assertions below stay at the shared descriptor/filesystem layer.
//
// DEFERRED(S1/T3.5): node:fs argument coercion and error object shape, async
// callbacks/promises/AbortSignal, fd 0/1/2 behavior, streams/watchers, JS Blob
// caching/lastModified, and platform permission-policy tests.
#include <cerrno>
#if !defined(_WIN32)
#include <sys/stat.h>
#endif

import std;
import mbun.core.io;

namespace io = mbun::core::io;

namespace {

int gChecks{0};
int gFailures{0};
constexpr int MAX_FAILURE_PRINTS{40};

void report_failure(std::string_view what) {
    ++gFailures;
    if (gFailures <= MAX_FAILURE_PRINTS) {
        std::println("  FAIL {}", what);
    }
}

void check(bool condition, std::string what) {
    ++gChecks;
    if (!condition) {
        report_failure(what);
    }
}

template <typename T>
bool check_ok(const io::Result<T>& result, std::string_view what) {
    ++gChecks;
    if (!result) {
        report_failure(std::format("{}: error code={} operation={} native={}", what,
                                   std::to_underlying(result.error().code),
                                   std::to_underlying(result.error().operation),
                                   result.error().nativeCode));
        return false;
    }
    return true;
}

std::span<const std::byte> bytes(std::string_view value) {
    return std::as_bytes(std::span{value.data(), value.size()});
}

std::string text(std::span<const std::byte> value) {
    return std::string{reinterpret_cast<const char*>(value.data()), value.size()};
}

std::string text(const std::vector<std::byte>& value) {
    return text(std::span{value});
}

struct TempDir {
    std::filesystem::path path;

    TempDir() {
        std::error_code ec;
        auto base = std::filesystem::temp_directory_path(ec);
        if (ec) {
            base = std::filesystem::current_path(ec);
        }
        const auto nonce =
            static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        for (unsigned attempt{0}; attempt < 128; ++attempt) {
            path = base / std::format("mbun-core-io-{}-{}", nonce, attempt);
            if (std::filesystem::create_directory(path, ec)) {
                return;
            }
            ec.clear();
        }
        report_failure("could not create temporary test directory");
    }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

void test_partial_loops() {
    // source: src/sys/File.zig File.writeAll/readAll and Rust file.rs equivalents.
    std::string output;
    int writeCalls{0};
    auto written = io::write_all_chunks(
        [&](std::span<const std::byte> chunk) -> io::Result<std::size_t> {
            ++writeCalls;
            const std::size_t amount{std::min<std::size_t>(3, chunk.size())};
            output.append(reinterpret_cast<const char*>(chunk.data()), amount);
            return amount;
        },
        bytes("partial-write"));
    check_ok(written, "write_all_chunks accepts partial writes");
    if (written) {
        check(*written == 13, "write_all_chunks reports the full byte count");
    }
    check(output == "partial-write", "write_all_chunks advances the input span");
    check(writeCalls > 1, "write_all_chunks retries after a partial write");

    int zeroWriteCalls{0};
    auto noProgress = io::write_all_chunks(
        [&](std::span<const std::byte>) -> io::Result<std::size_t> {
            ++zeroWriteCalls;
            return 0;
        },
        bytes("x"));
    check_ok(noProgress, "write_all_chunks preserves Bun's zero-write early-success contract");
    if (noProgress) {
        check(*noProgress == 0, "zero-write success reports only completed bytes");
    }
    check(zeroWriteCalls == 1, "zero-write success terminates without spinning");

    constexpr std::string_view INPUT{"partial-read"};
    std::size_t inputOffset{0};
    std::array<std::byte, 32> buffer{};
    int readCalls{0};
    auto read = io::read_all_chunks(
        [&](std::span<std::byte> destination) -> io::Result<std::size_t> {
            ++readCalls;
            const std::size_t amount{
                std::min({std::size_t{2}, destination.size(), INPUT.size() - inputOffset})};
            std::memcpy(destination.data(), INPUT.data() + inputOffset, amount);
            inputOffset += amount;
            return amount;
        },
        buffer);
    check_ok(read, "read_all_chunks accepts partial reads");
    if (read) {
        check(*read == INPUT.size(), "read_all_chunks stops at EOF and reports bytes read");
        check(text(std::span{buffer}.first(*read)) == INPUT,
              "read_all_chunks advances the destination span");
    }
    check(readCalls > 1, "read_all_chunks repeats until EOF");
}

void test_large_offsets_and_handle_lifetime() {
    TempDir temp;
    const auto path = temp.path / "sparse.bin";
    io::OpenOptions create{
        .access = io::Access::ReadWrite,
        .create = true,
        .truncate = true,
    };
    auto opened = io::File::open(path, create);
    if (!check_ok(opened, "open sparse large-offset file")) {
        return;
    }

    constexpr std::uint64_t LARGE_OFFSET{(std::uint64_t{1} << 33) + 123};
    check_ok(opened->write_all_at(LARGE_OFFSET, bytes("Z")), "write beyond 32-bit offset");
    auto cursor = opened->position();
    check_ok(cursor, "cursor after large positioned write");
    if (cursor) {
        check(*cursor == 0, "large positioned write preserves the shared cursor");
    }
    std::array<std::byte, 1> value{};
    auto read = opened->read_all_at(LARGE_OFFSET, value);
    check_ok(read, "read beyond 32-bit offset");
    if (read) {
        check(*read == 1 && text(value) == "Z", "large positioned I/O retains exact data");
    }
    auto stat = opened->metadata();
    check_ok(stat, "metadata for sparse large-offset file");
    if (stat) {
        check(stat->size == LARGE_OFFSET + 1, "large positioned write extends sparse file size");
    }
    check_ok(opened->close(), "close sparse large-offset file");

#if defined(__linux__)
    auto countDescriptors = [] {
        std::error_code ec;
        std::size_t count{0};
        for (std::filesystem::directory_iterator it{"/proc/self/fd", ec}, end; !ec && it != end;
             it.increment(ec)) {
            ++count;
        }
        return count;
    };
    const std::size_t before{countDescriptors()};
    for (int iteration{0}; iteration < 256; ++iteration) {
        auto file = io::File::open(path);
        if (!check_ok(file, "open handle for RAII leak probe")) {
            break;
        }
    }
    const std::size_t after{countDescriptors()};
    check(after <= before + 1, "File destruction does not leak native descriptors");
#endif
}

void test_platform_contracts() {
    using enum io::PlatformKind;
    namespace contract = io::platform_contract;

    check(contract::max_single_io_size(Linux) == 0x7ffff000ULL,
          "Linux single syscall length follows Bun MAX_COUNT");
    check(contract::max_single_io_size(Darwin) ==
              static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()),
          "Darwin single syscall length is capped at INT32_MAX");
    check(contract::max_single_io_size(Windows) ==
              static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()),
          "Windows single I/O length is capped at DWORD_MAX");
    check(contract::uses_nocancel_io(Darwin),
          "Darwin dispatch selects the non-cancellation syscall family");
    check(!contract::uses_nocancel_io(Linux) && !contract::uses_nocancel_io(Windows),
          "non-Darwin dispatch does not claim NOCANCEL symbols");
    check(contract::positioned_io_retries_eintr(Darwin),
          "Darwin positioned NOCANCEL I/O retains Bun's EINTR retry contract");
    check(contract::truncate_preserves_cursor(Windows),
          "Windows truncate contract preserves the shared file cursor");

    io::OpenOptions appendAndTruncate{
        .access = io::Access::WriteOnly,
        .truncate = true,
        .append = true,
    };
    check(contract::windows_open_needs_generic_write(appendAndTruncate),
          "Windows append+truncate requests write-data access");
    io::OpenOptions appendOnly{
        .access = io::Access::WriteOnly,
        .append = true,
    };
    check(contract::windows_open_needs_generic_write(appendOnly),
          "Windows append handle retains rights for a later truncate call");

    check(contract::map_posix_error(ENAMETOOLONG) == io::ErrorCode::NameTooLong,
          "ENAMETOOLONG maps to NameTooLong");
    check(contract::map_posix_error(ESPIPE) == io::ErrorCode::IllegalSeek,
          "ESPIPE maps to IllegalSeek");
    check(contract::map_posix_error(ENOTEMPTY) == io::ErrorCode::DirectoryNotEmpty,
          "ENOTEMPTY maps to DirectoryNotEmpty");

    constexpr std::uint32_t ERROR_ACCESS_DENIED_VALUE{5};
    constexpr std::uint32_t ERROR_WRITE_PROTECT_VALUE{19};
    constexpr std::uint32_t ERROR_DIR_NOT_EMPTY_VALUE{145};
    constexpr std::uint32_t ERROR_FILENAME_EXCED_RANGE_VALUE{206};
    check(contract::map_windows_error(ERROR_ACCESS_DENIED_VALUE, io::Operation::Write) ==
              io::ErrorCode::BadDescriptor,
          "Windows write ACCESS_DENIED maps to Bun's EBADF-compatible code");
    check(contract::map_windows_error(ERROR_ACCESS_DENIED_VALUE, io::Operation::Open) ==
              io::ErrorCode::PermissionDenied,
          "Windows open ACCESS_DENIED remains PermissionDenied");
    check(contract::map_windows_error(ERROR_WRITE_PROTECT_VALUE, io::Operation::Write) ==
              io::ErrorCode::WriteProtected,
          "Windows write-protected media has a distinct error code");
    check(contract::map_windows_error(ERROR_DIR_NOT_EMPTY_VALUE, io::Operation::Remove) ==
              io::ErrorCode::DirectoryNotEmpty,
          "Windows non-empty directory maps distinctly");
    check(contract::map_windows_error(ERROR_FILENAME_EXCED_RANGE_VALUE, io::Operation::Open) ==
              io::ErrorCode::NameTooLong,
          "Windows overlong filename maps to NameTooLong");
}

void test_open_read_write_seek_stat() {
    TempDir temp;
    const auto path = temp.path / "basic.bin";
    std::string content;
    content.reserve(256 * 1024);
    for (int i{0}; i < 4096; ++i) {
        content += "https://bun.sh/io/partial-write-vector\n";
    }

    // source: bun-write.test.js > large file > write large file (bytes)
    io::OpenOptions create{
        .access = io::Access::ReadWrite,
        .create = true,
        .truncate = true,
    };
    auto opened = io::File::open(path, create);
    if (!check_ok(opened, "open create/truncate read-write file")) {
        return;
    }
    io::File file{std::move(*opened)};
    check(!opened->is_open(), "moving File transfers descriptor ownership");
    check(file.is_open(), "moved-to File owns descriptor");
    check_ok(file.write_all(bytes(content)), "write_all persists a large byte span");

    auto position = file.position();
    check_ok(position, "position after write");
    if (position) {
        check(*position == content.size(), "write advances the file position");
    }

    auto stat = file.metadata();
    check_ok(stat, "fstat metadata");
    if (stat) {
        check(stat->is_file(), "metadata identifies a regular file");
        check(stat->size == content.size(), "metadata reports the byte size");
        check(stat->hardLinks >= 1, "metadata reports at least one hard link");
    }

    auto seeked = file.seek(0, io::SeekOrigin::Begin);
    check_ok(seeked, "seek to beginning");
    std::array<std::byte, 17> prefix{};
    auto prefixRead = file.read_all(prefix);
    check_ok(prefixRead, "read_all fills a fixed buffer");
    if (prefixRead) {
        check(*prefixRead == prefix.size(), "read_all fills the requested prefix");
        check(text(prefix) == content.substr(0, prefix.size()), "read_all returns exact bytes");
    }

    std::array<std::byte, 8> positioned{};
    auto at = file.read_all_at(0, positioned);
    check_ok(at, "positioned read from offset zero");
    if (at) {
        check(*at == positioned.size(), "positioned read fills its destination");
        check(text(positioned) == content.substr(0, positioned.size()),
              "positioned read returns offset-zero bytes");
    }
    position = file.position();
    check_ok(position, "position after positioned read");
    if (position) {
        check(*position == prefix.size(), "positioned read does not mutate the cursor");
    }

    check_ok(file.write_all_at(1, bytes("XY")), "positioned write");
    position = file.position();
    check_ok(position, "position after positioned write");
    if (position) {
        check(*position == prefix.size(), "positioned write does not mutate the cursor");
    }

    check_ok(file.truncate(9), "truncate open file");
    stat = file.metadata();
    check_ok(stat, "metadata after truncate");
    if (stat) {
        check(stat->size == 9, "truncate updates file size");
    }
    check_ok(file.sync(), "sync file data and metadata");
    check_ok(file.close(), "explicit close succeeds");
    check_ok(file.close(), "close is idempotent");
    check(!file.is_open(), "closed File no longer owns a descriptor");

    auto whole = io::read_file(path);
    check_ok(whole, "read_file after positioned write and truncate");
    if (whole) {
        check(text(*whole) == content.substr(0, 1) + "XY" + content.substr(3, 6),
              "positioned write and truncate preserve surrounding bytes");
    }
}

void test_path_helpers_append_and_eof() {
    TempDir temp;
    const auto path = temp.path / std::filesystem::path{u8"unicode-文件.txt"};

    // source: fs.test.ts > writeFileSync in append should not truncate the file
    check_ok(io::write_file(path, bytes("BEGIN")), "write_file creates unicode path");
    io::OpenOptions append{
        .access = io::Access::WriteOnly,
        .create = true,
        .append = true,
    };
    auto opened = io::File::open(path, append);
    if (check_ok(opened, "open append mode")) {
        check_ok(opened->write_all(bytes("-END")), "append write");
        check_ok(opened->close(), "close append file");
    }

    auto contents = io::read_file(path);
    check_ok(contents, "read_file unicode path");
    if (contents) {
        check(text(*contents) == "BEGIN-END", "append mode preserves existing bytes");
    }

    auto reader = io::File::open(path);
    if (!check_ok(reader, "open reader")) {
        return;
    }
    std::array<std::byte, 64> buffer{};
    auto amount = reader->read_all(buffer);
    check_ok(amount, "read_all larger than file");
    if (amount) {
        check(*amount == 9, "read_all stops at EOF");
    }
    auto eof = reader->read(buffer);
    check_ok(eof, "read at EOF");
    if (eof) {
        check(*eof == 0, "EOF is represented by zero bytes, not an error");
    }

    // source: bun-write.test.js > Bun.file empty file
    const auto emptyPath = temp.path / "empty";
    check_ok(io::write_file(emptyPath, {}), "write empty file");
    auto empty = io::read_file(emptyPath);
    check_ok(empty, "read empty file");
    if (empty) {
        check(empty->empty(), "empty file produces an empty byte vector");
    }
}

void test_errors_and_atomic_replace() {
    TempDir temp;
    const auto missing = temp.path / "missing" / "file.txt";

    // source: bun-write.test.js > Bun.file not found returns ENOENT
    auto absent = io::File::open(missing);
    check(!absent, "opening a missing file fails");
    if (!absent) {
        check(absent.error().code == io::ErrorCode::NotFound,
              "native missing-file error maps to NotFound");
        check(absent.error().operation == io::Operation::Open,
              "missing-file error retains the operation");
        check(absent.error().path() == missing, "missing-file error retains the path");
        check(absent.error().nativeCode != 0, "missing-file error retains native errno/code");
    }

    io::OpenOptions invalid{.access = io::Access::ReadOnly, .truncate = true};
    auto invalidOpen = io::File::open(temp.path / "invalid", invalid);
    check(!invalidOpen && invalidOpen.error().code == io::ErrorCode::InvalidArgument,
          "truncate without write access is rejected before syscall");

    const auto longName = temp.path / std::string(240, 'n');
    auto longAtomic = io::atomic_write_file(longName, bytes("long-name"));
    check_ok(longAtomic, "atomic write uses a short temp name independent of destination NAME_MAX");
    auto longContents = io::read_file(longName);
    check_ok(longContents, "read long-name atomic destination");
    if (longContents) {
        check(text(*longContents) == "long-name", "long-name atomic write publishes exact data");
    }

    const auto tooLong = temp.path / std::string(300, 'x');
    auto nameError = io::File::open(tooLong);
    check(!nameError && nameError.error().code == io::ErrorCode::NameTooLong,
          "native overlong component maps to NameTooLong");

#if !defined(_WIN32)
    const auto fifo = temp.path / "seek-pipe";
    if (::mkfifo(fifo.c_str(), 0600) == 0) {
        io::OpenOptions readWrite{.access = io::Access::ReadWrite};
        auto pipe = io::File::open(fifo, readWrite);
        if (check_ok(pipe, "open FIFO for illegal-seek mapping")) {
            auto seek = pipe->seek(0, io::SeekOrigin::Begin);
            check(!seek && seek.error().code == io::ErrorCode::IllegalSeek,
                  "ESPIPE from FIFO seek maps to IllegalSeek");
        }
    } else {
        report_failure("create FIFO for illegal-seek mapping");
    }
#endif

    const auto target = temp.path / "atomic.txt";
    check_ok(io::write_file(target, bytes("old")), "seed atomic target");
    check_ok(io::atomic_write_file(target, bytes("replacement")), "atomic replace existing file");
    auto replaced = io::read_file(target);
    check_ok(replaced, "read atomically replaced file");
    if (replaced) {
        check(text(*replaced) == "replacement", "atomic replace publishes complete new content");
    }

    std::error_code ec;
    const auto blocked = temp.path / "blocked";
    std::filesystem::create_directory(blocked, ec);
    auto failedReplace = io::atomic_write_file(blocked, bytes("must-not-publish"));
    check(!failedReplace, "atomic replace reports rename-over-directory failure");

    bool foundTemporary{false};
    for (std::filesystem::directory_iterator it{temp.path, ec}, end; !ec && it != end;
         it.increment(ec)) {
        const auto name = it->path().filename().string();
        foundTemporary |= name.starts_with(".mbun-") && name.ends_with(".tmp");
    }
    check(!foundTemporary, "atomic replace removes temporary files on failure");

    auto targetStat = io::metadata(target);
    check_ok(targetStat, "path metadata after atomic replace");
    if (targetStat) {
        check(targetStat->is_file() && targetStat->size == 11,
              "path metadata describes atomically replaced file");
    }
    auto dirStat = io::metadata(temp.path);
    check_ok(dirStat, "path metadata for directory");
    if (dirStat) {
        check(dirStat->is_directory(), "metadata identifies a directory");
    }
}

}  // namespace

int main() {
    test_partial_loops();
    test_open_read_write_seek_stat();
    test_large_offsets_and_handle_lifetime();
    test_platform_contracts();
    test_path_helpers_append_and_eof();
    test_errors_and_atomic_replace();

    if (gFailures > MAX_FAILURE_PRINTS) {
        std::println("  ... {} more failures not shown", gFailures - MAX_FAILURE_PRINTS);
    }
    std::println("test_core_io: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
