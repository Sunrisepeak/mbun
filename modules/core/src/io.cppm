// io.cppm — mbun.core.io: owning file handles and synchronous filesystem I/O.
//
// Algorithm and boundary references:
//   bun Rust src/sys/{file.rs,Error.rs,PosixStat.rs,tmp.rs,lib.rs}
//   bun Zig  src/sys/{File.zig,Error.zig,PosixStat.zig,tmp.zig,sys.zig}
//
// This module is the engine-independent syscall layer. JSC, node:fs, Bun.file,
// async polling, streams, TTYs, and sockets are deliberately layered above it.
module;

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#if defined(__linux__)
#include <sys/random.h>
#include <sys/syscall.h>
#endif
#include <sys/types.h>
#include <unistd.h>
#endif

export module mbun.core.io;

import std;

export namespace mbun::core::io {

enum class ErrorCode : std::uint8_t {
    Unknown,
    NotFound,
    PermissionDenied,
    AlreadyExists,
    BadDescriptor,
    InvalidArgument,
    IsDirectory,
    NotDirectory,
    TooManyOpenFiles,
    NoSpace,
    FileTooLarge,
    WouldBlock,
    CrossDevice,
    ReadOnlyFilesystem,
    BrokenPipe,
    OutOfMemory,
    NoProgress,
    NotSupported,
    NameTooLong,
    IllegalSeek,
    DirectoryNotEmpty,
    WriteProtected,
};

enum class Operation : std::uint8_t {
    Open,
    Close,
    Read,
    Write,
    Seek,
    Stat,
    Truncate,
    Sync,
    Rename,
    Remove,
};

struct ErrorContext {
    std::filesystem::path path{};
    std::filesystem::path destination{};
};

struct Error {
    ErrorCode code{ErrorCode::Unknown};
    Operation operation{Operation::Open};
    int nativeCode{0};
    std::shared_ptr<const ErrorContext> context{};

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        static const std::filesystem::path EMPTY{};
        return context ? context->path : EMPTY;
    }

    [[nodiscard]] const std::filesystem::path& destination() const noexcept {
        static const std::filesystem::path EMPTY{};
        return context ? context->destination : EMPTY;
    }
};

template <typename T>
using Result = std::expected<T, Error>;

enum class FileType : std::uint8_t {
    Unknown,
    Regular,
    Directory,
    Symlink,
    BlockDevice,
    CharacterDevice,
    NamedPipe,
    Socket,
};

struct Timestamp {
    std::int64_t seconds{0};
    std::uint32_t nanoseconds{0};
};

struct Metadata {
    FileType type{FileType::Unknown};
    std::uint64_t size{0};
    std::uint64_t device{0};
    std::uint64_t inode{0};
    std::uint64_t hardLinks{0};
    std::uint32_t permissions{0};
    Timestamp accessed{};
    Timestamp modified{};
    Timestamp changed{};
    Timestamp created{};

    [[nodiscard]] bool is_file() const noexcept {
        return type == FileType::Regular;
    }
    [[nodiscard]] bool is_directory() const noexcept {
        return type == FileType::Directory;
    }
};

enum class Access : std::uint8_t { ReadOnly, WriteOnly, ReadWrite };

struct OpenOptions {
    Access access{Access::ReadOnly};
    bool create{false};
    bool truncate{false};
    bool exclusive{false};
    bool append{false};
    bool nonblock{false};
    std::uint32_t mode{0664};
};

enum class PlatformKind : std::uint8_t { Linux, Darwin, Windows };

// Pure platform decisions are exported so every target's ABI contract can be
// tested even when that operating system is not available to the test runner.
// The syscall branches below consume the same decisions.
namespace platform_contract {

[[nodiscard]] constexpr std::size_t max_single_io_size(PlatformKind platform) noexcept {
    switch (platform) {
        case PlatformKind::Linux:
            return 0x7ffff000ULL;
        case PlatformKind::Darwin:
            return static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max());
        case PlatformKind::Windows:
            return static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());
    }
    std::unreachable();
}

[[nodiscard]] constexpr bool uses_nocancel_io(PlatformKind platform) noexcept {
    return platform == PlatformKind::Darwin;
}

[[nodiscard]] constexpr bool positioned_io_retries_eintr(PlatformKind platform) noexcept {
    return platform != PlatformKind::Windows;
}

[[nodiscard]] constexpr bool truncate_preserves_cursor(PlatformKind) noexcept {
    return true;
}

[[nodiscard]] constexpr bool windows_open_needs_generic_write(const OpenOptions& options) noexcept {
    return options.access != Access::ReadOnly;
}

[[nodiscard]] constexpr ErrorCode map_posix_error(int code) noexcept {
    switch (code) {
        case ENOENT:
            return ErrorCode::NotFound;
        case EACCES:
        case EPERM:
            return ErrorCode::PermissionDenied;
        case EEXIST:
            return ErrorCode::AlreadyExists;
        case EBADF:
            return ErrorCode::BadDescriptor;
        case EINVAL:
            return ErrorCode::InvalidArgument;
        case EISDIR:
            return ErrorCode::IsDirectory;
        case ENOTDIR:
            return ErrorCode::NotDirectory;
        case EMFILE:
        case ENFILE:
            return ErrorCode::TooManyOpenFiles;
        case ENOSPC:
            return ErrorCode::NoSpace;
        case EFBIG:
        case EOVERFLOW:
            return ErrorCode::FileTooLarge;
        case EAGAIN:
            return ErrorCode::WouldBlock;
        case EXDEV:
            return ErrorCode::CrossDevice;
        case EROFS:
            return ErrorCode::ReadOnlyFilesystem;
        case EPIPE:
            return ErrorCode::BrokenPipe;
        case ENOMEM:
            return ErrorCode::OutOfMemory;
        case ENOSYS:
        case EOPNOTSUPP:
            return ErrorCode::NotSupported;
        case ENAMETOOLONG:
            return ErrorCode::NameTooLong;
        case ESPIPE:
            return ErrorCode::IllegalSeek;
        case ENOTEMPTY:
            return ErrorCode::DirectoryNotEmpty;
        default:
            return ErrorCode::Unknown;
    }
}

[[nodiscard]] constexpr ErrorCode map_windows_error(std::uint32_t code,
                                                    Operation operation) noexcept {
    switch (code) {
        case 2:    // ERROR_FILE_NOT_FOUND
        case 3:    // ERROR_PATH_NOT_FOUND
        case 123:  // ERROR_INVALID_NAME
            return ErrorCode::NotFound;
        case 206:  // ERROR_FILENAME_EXCED_RANGE
            return ErrorCode::NameTooLong;
        case 145:  // ERROR_DIR_NOT_EMPTY
            return ErrorCode::DirectoryNotEmpty;
        case 5:  // ERROR_ACCESS_DENIED; Bun maps failed writes to EBADF
            return operation == Operation::Write ? ErrorCode::BadDescriptor
                                                 : ErrorCode::PermissionDenied;
        case 19:  // ERROR_WRITE_PROTECT
            return ErrorCode::WriteProtected;
        case 6:  // ERROR_INVALID_HANDLE
            return ErrorCode::BadDescriptor;
        case 80:   // ERROR_FILE_EXISTS
        case 183:  // ERROR_ALREADY_EXISTS
            return ErrorCode::AlreadyExists;
        case 87:   // ERROR_INVALID_PARAMETER
        case 131:  // ERROR_NEGATIVE_SEEK
            return ErrorCode::InvalidArgument;
        case 112:  // ERROR_DISK_FULL
        case 39:   // ERROR_HANDLE_DISK_FULL
            return ErrorCode::NoSpace;
        case 17:  // ERROR_NOT_SAME_DEVICE
            return ErrorCode::CrossDevice;
        case 8:   // ERROR_NOT_ENOUGH_MEMORY
        case 14:  // ERROR_OUTOFMEMORY
            return ErrorCode::OutOfMemory;
        case 109:  // ERROR_BROKEN_PIPE
        case 232:  // ERROR_NO_DATA
            return ErrorCode::BrokenPipe;
        case 50:   // ERROR_NOT_SUPPORTED
        case 120:  // ERROR_CALL_NOT_IMPLEMENTED
            return ErrorCode::NotSupported;
        default:
            return ErrorCode::Unknown;
    }
}

}  // namespace platform_contract

enum class SeekOrigin : std::uint8_t { Begin, Current, End };

}  // namespace mbun::core::io

namespace mbun::core::io::detail {

constexpr Error error(ErrorCode code, Operation operation, int nativeCode = 0) noexcept {
    return Error{.code = code, .operation = operation, .nativeCode = nativeCode};
}

#if defined(_WIN32)
ErrorCode map_native_code(int code, Operation operation) noexcept {
    if (const auto mapped{
            platform_contract::map_windows_error(static_cast<std::uint32_t>(code), operation)};
        mapped != ErrorCode::Unknown) {
        return mapped;
    }
    switch (static_cast<unsigned long>(code)) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_NAME:
            return ErrorCode::NotFound;
        case ERROR_PRIVILEGE_NOT_HELD:
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
            return ErrorCode::PermissionDenied;
        case ERROR_FILE_EXISTS:
        case ERROR_ALREADY_EXISTS:
            return ErrorCode::AlreadyExists;
        case ERROR_INVALID_HANDLE:
            return ErrorCode::BadDescriptor;
        case ERROR_INVALID_PARAMETER:
        case ERROR_NEGATIVE_SEEK:
            return ErrorCode::InvalidArgument;
        case ERROR_DIRECTORY:
            return ErrorCode::NotDirectory;
        case ERROR_TOO_MANY_OPEN_FILES:
            return ErrorCode::TooManyOpenFiles;
        case ERROR_DISK_FULL:
        case ERROR_HANDLE_DISK_FULL:
            return ErrorCode::NoSpace;
        case ERROR_FILE_TOO_LARGE:
            return ErrorCode::FileTooLarge;
        case ERROR_NO_SYSTEM_RESOURCES:
        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_OUTOFMEMORY:
            return ErrorCode::OutOfMemory;
        case ERROR_BROKEN_PIPE:
        case ERROR_NO_DATA:
            return ErrorCode::BrokenPipe;
        case ERROR_NOT_SUPPORTED:
        case ERROR_CALL_NOT_IMPLEMENTED:
            return ErrorCode::NotSupported;
        case ERROR_NOT_SAME_DEVICE:
            return ErrorCode::CrossDevice;
        default:
            return ErrorCode::Unknown;
    }
}

int last_native_error() noexcept {
    return static_cast<int>(::GetLastError());
}
#else
ErrorCode map_native_code(int code, Operation) noexcept {
    if (const auto mapped{platform_contract::map_posix_error(code)}; mapped != ErrorCode::Unknown) {
        return mapped;
    }
    switch (code) {
#if defined(EDQUOT)
        case EDQUOT:
#endif
            return ErrorCode::NoSpace;
#if EWOULDBLOCK != EAGAIN
        case EWOULDBLOCK:
#endif
            return ErrorCode::WouldBlock;
#if defined(ENOTSUP) && ENOTSUP != EOPNOTSUPP
        case ENOTSUP:
#endif
            return ErrorCode::NotSupported;
        default:
            return ErrorCode::Unknown;
    }
}

int last_native_error() noexcept {
    return errno;
}
#endif

[[gnu::cold]] Error native_error(Operation operation, int code) noexcept {
    return error(map_native_code(code, operation), operation, code);
}

[[gnu::cold]] Error native_error(Operation operation, int code, const std::filesystem::path& path,
                                 const std::filesystem::path& destination = {}) noexcept {
    Error result{.code = map_native_code(code, operation),
                 .operation = operation,
                 .nativeCode = code};
    try {
        result.context =
            std::make_shared<ErrorContext>(ErrorContext{.path = path, .destination = destination});
    } catch (...) {
        // Error reporting is a cold best-effort path. Preserve the native
        // error even when allocating its optional path context fails.
    }
    return result;
}

[[gnu::cold]] Error with_paths(Error error, const std::filesystem::path& path,
                               const std::filesystem::path& destination = {}) noexcept {
    try {
        error.context =
            std::make_shared<ErrorContext>(ErrorContext{.path = path, .destination = destination});
    } catch (...) {
        error.context.reset();
    }
    return error;
}

Error allocation_error(Operation operation) noexcept {
    return Error{
        .code = ErrorCode::OutOfMemory,
        .operation = operation,
#if defined(_WIN32)
        .nativeCode = static_cast<int>(ERROR_NOT_ENOUGH_MEMORY),
#else
        .nativeCode = ENOMEM,
#endif
    };
}

bool can_write(Access access) noexcept {
    return access != Access::ReadOnly;
}

Result<void> validate_open_options(const OpenOptions& options, const std::filesystem::path& path) {
    if ((options.truncate || options.append) && !can_write(options.access)) {
        return std::unexpected(
            with_paths(Error{.code = ErrorCode::InvalidArgument, .operation = Operation::Open},
                       path));
    }
    if (options.exclusive && !options.create) {
        return std::unexpected(
            with_paths(Error{.code = ErrorCode::InvalidArgument, .operation = Operation::Open},
                       path));
    }
    return {};
}

#if defined(_WIN32)
using NativeHandle = HANDLE;
inline const NativeHandle INVALID_NATIVE_HANDLE{INVALID_HANDLE_VALUE};

using NtCreateFileFunction = decltype(&::NtCreateFile);
using RtlNtStatusToDosErrorFunction = decltype(&::RtlNtStatusToDosError);

NtCreateFileFunction nt_create_file_function() noexcept {
    static const auto function = reinterpret_cast<NtCreateFileFunction>(
        ::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "NtCreateFile"));
    return function;
}

int nt_status_error(NTSTATUS status) noexcept {
    static const auto function = reinterpret_cast<RtlNtStatusToDosErrorFunction>(
        ::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "RtlNtStatusToDosError"));
    return function ? static_cast<int>(function(status)) : static_cast<int>(ERROR_NOT_SUPPORTED);
}

Result<std::uint64_t> random_nonce() noexcept {
    using RtlGenRandomFunction = BOOLEAN(WINAPI*)(PVOID, ULONG);
    static const HMODULE ADVAPI{::LoadLibraryW(L"advapi32.dll")};
    static const auto function =
        ADVAPI
            ? reinterpret_cast<RtlGenRandomFunction>(::GetProcAddress(ADVAPI, "SystemFunction036"))
            : nullptr;
    std::uint64_t value{0};
    if (!function || function(&value, sizeof(value)) == FALSE) {
        return std::unexpected(
            native_error(Operation::Open, static_cast<int>(ERROR_NOT_SUPPORTED)));
    }
    return value;
}

Result<NativeHandle> create_relative_file(NativeHandle directory, std::wstring_view name,
                                          std::uint32_t mode) noexcept {
    if (name.size() > std::numeric_limits<USHORT>::max() / sizeof(wchar_t)) {
        return std::unexpected(error(ErrorCode::NameTooLong, Operation::Open));
    }
    const auto createFile{nt_create_file_function()};
    if (!createFile) {
        return std::unexpected(
            native_error(Operation::Open, static_cast<int>(ERROR_NOT_SUPPORTED)));
    }
    UNICODE_STRING unicode{
        .Length = static_cast<USHORT>(name.size() * sizeof(wchar_t)),
        .MaximumLength = static_cast<USHORT>(name.size() * sizeof(wchar_t)),
        .Buffer = const_cast<PWSTR>(name.data()),
    };
    OBJECT_ATTRIBUTES attributes{
        .Length = sizeof(OBJECT_ATTRIBUTES),
        .RootDirectory = directory,
        .ObjectName = &unicode,
        .Attributes = OBJ_CASE_INSENSITIVE,
        .SecurityDescriptor = nullptr,
        .SecurityQualityOfService = nullptr,
    };
    IO_STATUS_BLOCK io{};
    NativeHandle handle{INVALID_NATIVE_HANDLE};
    ULONG fileAttributes{FILE_ATTRIBUTE_TEMPORARY};
    if ((mode & 0200) == 0) {
        fileAttributes |= FILE_ATTRIBUTE_READONLY;
    }
    const NTSTATUS status{
        createFile(&handle, GENERIC_WRITE | DELETE | SYNCHRONIZE, &attributes, &io, nullptr,
                   fileAttributes, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                   FILE_CREATE,
                   FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT,
                   nullptr, 0)};
    if (status < 0) {
        return std::unexpected(native_error(Operation::Open, nt_status_error(status)));
    }
    return handle;
}

Result<void> rename_relative(NativeHandle file, NativeHandle directory,
                             std::wstring_view destination) noexcept {
    if (destination.size() > std::numeric_limits<DWORD>::max() / sizeof(wchar_t)) {
        return std::unexpected(error(ErrorCode::NameTooLong, Operation::Rename));
    }
    const std::size_t byteLength{destination.size() * sizeof(wchar_t)};
    const std::size_t infoSize{offsetof(FILE_RENAME_INFO, FileName) + byteLength};
    std::vector<std::byte> storage;
    try {
        storage.resize(infoSize);
    } catch (...) {
        return std::unexpected(allocation_error(Operation::Rename));
    }
    auto* info{reinterpret_cast<FILE_RENAME_INFO*>(storage.data())};
    info->ReplaceIfExists = TRUE;
    info->RootDirectory = directory;
    info->FileNameLength = static_cast<DWORD>(byteLength);
    std::memcpy(info->FileName, destination.data(), byteLength);
    if (::SetFileInformationByHandle(file, FileRenameInfo, info, static_cast<DWORD>(infoSize)) ==
        0) {
        return std::unexpected(native_error(Operation::Rename, last_native_error()));
    }
    return {};
}

Result<void> clear_file_attributes(NativeHandle file, DWORD attributes) noexcept {
    FILE_BASIC_INFO basic{};
    if (::GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof(basic)) == 0) {
        return std::unexpected(native_error(Operation::Stat, last_native_error()));
    }
    basic.FileAttributes &= ~attributes;
    if (basic.FileAttributes == 0) {
        basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
    }
    if (::SetFileInformationByHandle(file, FileBasicInfo, &basic, sizeof(basic)) == 0) {
        return std::unexpected(native_error(Operation::Rename, last_native_error()));
    }
    return {};
}

Result<void> mark_delete(NativeHandle file) noexcept {
    if (auto cleared{
            clear_file_attributes(file, FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_TEMPORARY)};
        !cleared) {
        return cleared;
    }
    FILE_DISPOSITION_INFO disposition{.DeleteFile = TRUE};
    if (::SetFileInformationByHandle(file, FileDispositionInfo, &disposition,
                                     sizeof(disposition)) == 0) {
        return std::unexpected(native_error(Operation::Remove, last_native_error()));
    }
    return {};
}

Timestamp timestamp(FILETIME value) noexcept {
    constexpr std::uint64_t WINDOWS_TO_UNIX_EPOCH{116444736000000000ULL};
    const std::uint64_t ticks{(static_cast<std::uint64_t>(value.dwHighDateTime) << 32) |
                              value.dwLowDateTime};
    if (ticks < WINDOWS_TO_UNIX_EPOCH) {
        return {};
    }
    const std::uint64_t unixTicks{ticks - WINDOWS_TO_UNIX_EPOCH};
    return Timestamp{.seconds = static_cast<std::int64_t>(unixTicks / 10'000'000ULL),
                     .nanoseconds =
                         static_cast<std::uint32_t>((unixTicks % 10'000'000ULL) * 100ULL)};
}

Result<Metadata> metadata_from_handle(NativeHandle handle) noexcept {
    BY_HANDLE_FILE_INFORMATION value{};
    if (::GetFileInformationByHandle(handle, &value) == 0) {
        const int code{last_native_error()};
        return std::unexpected(native_error(Operation::Stat, code));
    }

    FileType type{FileType::Regular};
    const DWORD nativeType{::GetFileType(handle)};
    if (nativeType == FILE_TYPE_CHAR) {
        type = FileType::CharacterDevice;
    } else if (nativeType == FILE_TYPE_PIPE) {
        type = FileType::NamedPipe;
    } else if ((value.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        type = FileType::Symlink;
    } else if ((value.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        type = FileType::Directory;
    } else if (nativeType != FILE_TYPE_DISK) {
        type = FileType::Unknown;
    }

    const std::uint64_t size{(static_cast<std::uint64_t>(value.nFileSizeHigh) << 32) |
                             value.nFileSizeLow};
    const std::uint64_t inode{(static_cast<std::uint64_t>(value.nFileIndexHigh) << 32) |
                              value.nFileIndexLow};
    std::uint32_t permissions{0444};
    if ((value.dwFileAttributes & FILE_ATTRIBUTE_READONLY) == 0) {
        permissions |= 0222;
    }
    if (type == FileType::Directory) {
        permissions |= 0111;
    }
    return Metadata{.type = type,
                    .size = size,
                    .device = value.dwVolumeSerialNumber,
                    .inode = inode,
                    .hardLinks = value.nNumberOfLinks,
                    .permissions = permissions,
                    .accessed = timestamp(value.ftLastAccessTime),
                    .modified = timestamp(value.ftLastWriteTime),
                    .changed = timestamp(value.ftLastWriteTime),
                    .created = timestamp(value.ftCreationTime)};
}
#else
using NativeHandle = int;
constexpr NativeHandle INVALID_NATIVE_HANDLE{-1};

#if defined(__APPLE__)
extern "C" {
int openat_nocancel(int, const char*, int, ...) __asm__("openat$NOCANCEL");
ssize_t read_nocancel(int, void*, std::size_t) __asm__("read$NOCANCEL");
ssize_t write_nocancel(int, const void*, std::size_t) __asm__("write$NOCANCEL");
ssize_t pread_nocancel(int, void*, std::size_t, off_t) __asm__("pread$NOCANCEL");
ssize_t pwrite_nocancel(int, const void*, std::size_t, off_t) __asm__("pwrite$NOCANCEL");
int close_nocancel(int) __asm__("close$NOCANCEL");
}
#endif

Timestamp timestamp(std::int64_t seconds, std::int64_t nanoseconds) noexcept {
    return Timestamp{.seconds = seconds,
                     .nanoseconds = static_cast<std::uint32_t>(
                         std::max<std::int64_t>(0,
                                                std::min<std::int64_t>(999'999'999, nanoseconds)))};
}

FileType file_type(mode_t mode) noexcept {
    if (S_ISREG(mode)) {
        return FileType::Regular;
    }
    if (S_ISDIR(mode)) {
        return FileType::Directory;
    }
    if (S_ISLNK(mode)) {
        return FileType::Symlink;
    }
    if (S_ISBLK(mode)) {
        return FileType::BlockDevice;
    }
    if (S_ISCHR(mode)) {
        return FileType::CharacterDevice;
    }
    if (S_ISFIFO(mode)) {
        return FileType::NamedPipe;
    }
#if defined(S_ISSOCK)
    if (S_ISSOCK(mode)) {
        return FileType::Socket;
    }
#endif
    return FileType::Unknown;
}

Metadata metadata_from_stat(const struct stat& value) noexcept {
#if defined(__APPLE__)
    const Timestamp accessed{timestamp(value.st_atimespec.tv_sec, value.st_atimespec.tv_nsec)};
    const Timestamp modified{timestamp(value.st_mtimespec.tv_sec, value.st_mtimespec.tv_nsec)};
    const Timestamp changed{timestamp(value.st_ctimespec.tv_sec, value.st_ctimespec.tv_nsec)};
    const Timestamp created{
        timestamp(value.st_birthtimespec.tv_sec, value.st_birthtimespec.tv_nsec)};
#else
    const Timestamp accessed{timestamp(value.st_atim.tv_sec, value.st_atim.tv_nsec)};
    const Timestamp modified{timestamp(value.st_mtim.tv_sec, value.st_mtim.tv_nsec)};
    const Timestamp changed{timestamp(value.st_ctim.tv_sec, value.st_ctim.tv_nsec)};
    const Timestamp created{};
#endif
    return Metadata{.type = file_type(value.st_mode),
                    .size = static_cast<std::uint64_t>(std::max<off_t>(0, value.st_size)),
                    .device = static_cast<std::uint64_t>(value.st_dev),
                    .inode = static_cast<std::uint64_t>(value.st_ino),
                    .hardLinks = static_cast<std::uint64_t>(value.st_nlink),
                    .permissions = static_cast<std::uint32_t>(value.st_mode & 07777),
                    .accessed = accessed,
                    .modified = modified,
                    .changed = changed,
                    .created = created};
}

// glibc's read/write family are cancellation points. bun's Linux sys layer
// uses direct syscalls on these hot paths; mirror that on supported Linux
// architectures and keep libc as the portable POSIX fallback.
#if defined(__linux__) && defined(__x86_64__)
[[gnu::always_inline]] inline long linux_syscall3(long number, long arg1, long arg2,
                                                  long arg3) noexcept {
    long result;
    asm volatile("syscall"
                 : "=a"(result)
                 : "a"(number), "D"(arg1), "S"(arg2), "d"(arg3)
                 : "rcx", "r11", "memory");
    return result;
}

[[gnu::always_inline]] inline long linux_syscall4(long number, long arg1, long arg2, long arg3,
                                                  long arg4) noexcept {
    register long fourth asm("r10"){arg4};
    long result;
    asm volatile("syscall"
                 : "=a"(result)
                 : "a"(number), "D"(arg1), "S"(arg2), "d"(arg3), "r"(fourth)
                 : "rcx", "r11", "memory");
    return result;
}
#elif defined(__linux__) && defined(__aarch64__)
[[gnu::always_inline]] inline long linux_syscall3(long number, long arg1, long arg2,
                                                  long arg3) noexcept {
    register long x0 asm("x0"){arg1};
    register long x1 asm("x1"){arg2};
    register long x2 asm("x2"){arg3};
    register long x8 asm("x8"){number};
    asm volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
    return x0;
}

[[gnu::always_inline]] inline long linux_syscall4(long number, long arg1, long arg2, long arg3,
                                                  long arg4) noexcept {
    register long x0 asm("x0"){arg1};
    register long x1 asm("x1"){arg2};
    register long x2 asm("x2"){arg3};
    register long x3 asm("x3"){arg4};
    register long x8 asm("x8"){number};
    asm volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
    return x0;
}
#endif

[[gnu::always_inline]] inline ssize_t normalize_linux_result(long result) noexcept {
    if (result < 0 && result >= -4095) [[unlikely]] {
        errno = static_cast<int>(-result);
        return -1;
    }
    return static_cast<ssize_t>(result);
}

[[gnu::always_inline]] inline ssize_t read_native(int fd, void* buffer,
                                                  std::size_t length) noexcept {
#if defined(__APPLE__)
    return read_nocancel(fd, buffer, length);
#elif defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
    return normalize_linux_result(
        linux_syscall3(SYS_read, fd, reinterpret_cast<long>(buffer), static_cast<long>(length)));
#else
    return ::read(fd, buffer, length);
#endif
}

[[gnu::always_inline]] inline ssize_t write_native(int fd, const void* buffer,
                                                   std::size_t length) noexcept {
#if defined(__APPLE__)
    return write_nocancel(fd, buffer, length);
#elif defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
    return normalize_linux_result(
        linux_syscall3(SYS_write, fd, reinterpret_cast<long>(buffer), static_cast<long>(length)));
#else
    return ::write(fd, buffer, length);
#endif
}

[[gnu::always_inline]] inline ssize_t pread_native(int fd, void* buffer, std::size_t length,
                                                   off_t offset) noexcept {
#if defined(__APPLE__)
    return pread_nocancel(fd, buffer, length, offset);
#elif defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
    return normalize_linux_result(linux_syscall4(SYS_pread64, fd, reinterpret_cast<long>(buffer),
                                                 static_cast<long>(length), offset));
#else
    return ::pread(fd, buffer, length, offset);
#endif
}

[[gnu::always_inline]] inline ssize_t pwrite_native(int fd, const void* buffer, std::size_t length,
                                                    off_t offset) noexcept {
#if defined(__APPLE__)
    return pwrite_nocancel(fd, buffer, length, offset);
#elif defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
    return normalize_linux_result(linux_syscall4(SYS_pwrite64, fd, reinterpret_cast<long>(buffer),
                                                 static_cast<long>(length), offset));
#else
    return ::pwrite(fd, buffer, length, offset);
#endif
}
#endif

#if !defined(_WIN32)
Result<std::uint64_t> random_nonce() noexcept {
    std::uint64_t value{0};
#if defined(__linux__)
    std::byte* cursor{reinterpret_cast<std::byte*>(&value)};
    std::size_t remaining{sizeof(value)};
    while (remaining != 0) {
        const ssize_t amount{::getrandom(cursor, remaining, 0)};
        if (amount > 0) {
            cursor += amount;
            remaining -= static_cast<std::size_t>(amount);
            continue;
        }
        if (amount < 0 && errno == EINTR) {
            continue;
        }
        return std::unexpected(native_error(Operation::Open, last_native_error()));
    }
#elif defined(__APPLE__)
    ::arc4random_buf(&value, sizeof(value));
#else
    try {
        std::random_device random;
        value = (static_cast<std::uint64_t>(random()) << 32) ^ random();
    } catch (...) {
        return std::unexpected(allocation_error(Operation::Open));
    }
#endif
    return value;
}
#endif

}  // namespace mbun::core::io::detail

export namespace mbun::core::io {

template <typename WriteOnce>
Result<std::size_t> write_all_chunks(WriteOnce&& writeOnce, std::span<const std::byte> input) {
    std::size_t total{0};
    while (!input.empty()) {
        auto written{std::invoke(writeOnce, input)};
        if (!written) {
            return std::unexpected(std::move(written.error()));
        }
        if (*written > input.size()) [[unlikely]] {
            return std::unexpected(detail::error(ErrorCode::InvalidArgument, Operation::Write));
        }
        if (*written == 0) [[unlikely]] {
            return total;
        }
        input = input.subspan(*written);
        total += *written;
    }
    return total;
}

template <typename ReadOnce>
Result<std::size_t> read_all_chunks(ReadOnce&& readOnce, std::span<std::byte> output) {
    std::size_t total{0};
    while (!output.empty()) {
        auto amount{std::invoke(readOnce, output)};
        if (!amount) {
            return std::unexpected(std::move(amount.error()));
        }
        if (*amount > output.size()) [[unlikely]] {
            return std::unexpected(detail::error(ErrorCode::InvalidArgument, Operation::Read));
        }
        if (*amount == 0) {
            break;
        }
        output = output.subspan(*amount);
        total += *amount;
    }
    return total;
}

class File {
private:
    detail::NativeHandle handle_{detail::INVALID_NATIVE_HANDLE};
    bool append_{false};

    explicit File(detail::NativeHandle handle, bool append = false) noexcept
        : handle_{handle}, append_{append} {}

    void close_discard_() noexcept {
        if (!is_open()) {
            return;
        }
#if defined(_WIN32)
        ::CloseHandle(handle_);
#elif defined(__APPLE__)
        (void)detail::close_nocancel(handle_);
#else
        // close(2) may already have released the descriptor when reporting
        // EINTR. Retrying can close an unrelated, newly-reused descriptor.
        (void)::close(handle_);
#endif
        handle_ = detail::INVALID_NATIVE_HANDLE;
    }
public:
    File() noexcept = default;
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    File(File&& other) noexcept
        : handle_{std::exchange(other.handle_, detail::INVALID_NATIVE_HANDLE)},
          append_{std::exchange(other.append_, false)} {}

    File& operator=(File&& other) noexcept {
        if (this != &other) {
            close_discard_();
            handle_ = std::exchange(other.handle_, detail::INVALID_NATIVE_HANDLE);
            append_ = std::exchange(other.append_, false);
        }
        return *this;
    }

    ~File() {
        close_discard_();
    }

    // Take ownership of an already-open native descriptor (e.g. one end of a
    // socketpair(2)). The handle is closed when the File is destroyed/closed.
    [[nodiscard]] static File adopt(detail::NativeHandle handle) noexcept {
        return File{handle, false};
    }

    [[nodiscard]] static Result<File> open(const std::filesystem::path& path,
                                           OpenOptions options = {}) {
        if (auto valid{detail::validate_open_options(options, path)}; !valid) {
            return std::unexpected(std::move(valid.error()));
        }
#if defined(_WIN32)
        DWORD desiredAccess{0};
        switch (options.access) {
            case Access::ReadOnly:
                desiredAccess = GENERIC_READ;
                break;
            case Access::WriteOnly:
                desiredAccess = options.append ? FILE_APPEND_DATA : GENERIC_WRITE;
                if (platform_contract::windows_open_needs_generic_write(options)) {
                    desiredAccess |= GENERIC_WRITE;
                }
                break;
            case Access::ReadWrite:
                desiredAccess = GENERIC_READ | (options.append ? FILE_APPEND_DATA : GENERIC_WRITE);
                if (platform_contract::windows_open_needs_generic_write(options)) {
                    desiredAccess |= GENERIC_WRITE;
                }
                break;
        }
        DWORD creation{OPEN_EXISTING};
        if (options.create && options.exclusive) {
            creation = CREATE_NEW;
        } else if (options.create && options.truncate) {
            creation = CREATE_ALWAYS;
        } else if (options.create) {
            creation = OPEN_ALWAYS;
        } else if (options.truncate) {
            creation = TRUNCATE_EXISTING;
        }
        const HANDLE handle{::CreateFileW(path.c_str(), desiredAccess,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                          nullptr, creation, FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (handle == INVALID_HANDLE_VALUE) {
            const int code{detail::last_native_error()};
            return std::unexpected(detail::native_error(Operation::Open, code, path));
        }
        return File{handle, options.append};
#else
        int flags{0};
        switch (options.access) {
            case Access::ReadOnly:
                flags |= O_RDONLY;
                break;
            case Access::WriteOnly:
                flags |= O_WRONLY;
                break;
            case Access::ReadWrite:
                flags |= O_RDWR;
                break;
        }
        flags |= options.create ? O_CREAT : 0;
        flags |= options.truncate ? O_TRUNC : 0;
        flags |= options.exclusive ? O_EXCL : 0;
        flags |= options.append ? O_APPEND : 0;
#if defined(O_NONBLOCK)
        flags |= options.nonblock ? O_NONBLOCK : 0;
#endif
#if defined(O_CLOEXEC)
        flags |= O_CLOEXEC;
#endif
        int handle{-1};
#if defined(__APPLE__)
        handle = detail::openat_nocancel(AT_FDCWD, path.c_str(), flags,
                                         static_cast<mode_t>(options.mode & 07777));
        if (handle < 0) {
            const int code{detail::last_native_error()};
            return std::unexpected(detail::native_error(Operation::Open, code, path));
        }
#else
        while ((handle = ::open(path.c_str(), flags, static_cast<mode_t>(options.mode & 07777))) <
               0) {
            if (errno == EINTR) {
                continue;
            }
            const int code{detail::last_native_error()};
            return std::unexpected(detail::native_error(Operation::Open, code, path));
        }
#endif
        return File{handle, options.append};
#endif
    }

    [[nodiscard]] bool is_open() const noexcept {
        return handle_ != detail::INVALID_NATIVE_HANDLE;
    }

#if !defined(_WIN32)
    // Raw POSIX descriptor for callers needing fstat/statx on the open fd
    // (node:fs fstatSync bridges the virtual fd table to the real fd here).
    [[nodiscard]] int native_fd() const noexcept { return handle_; }
#endif

    Result<void> close() noexcept {
        if (!is_open()) {
            return {};
        }
        const auto handle{std::exchange(handle_, detail::INVALID_NATIVE_HANDLE)};
#if defined(_WIN32)
        if (::CloseHandle(handle) == 0) {
            return std::unexpected(
                detail::native_error(Operation::Close, detail::last_native_error()));
        }
#elif defined(__APPLE__)
        if (detail::close_nocancel(handle) != 0) {
            return std::unexpected(
                detail::native_error(Operation::Close, detail::last_native_error()));
        }
#else
        if (::close(handle) != 0 && errno != EINTR) {
            return std::unexpected(
                detail::native_error(Operation::Close, detail::last_native_error()));
        }
#endif
        return {};
    }

    [[gnu::always_inline]] inline Result<std::size_t> read(std::span<std::byte> output) noexcept {
        if (!is_open()) {
            return std::unexpected(detail::error(ErrorCode::BadDescriptor, Operation::Read));
        }
        if (output.empty()) {
            return 0;
        }
#if defined(_WIN32)
        const DWORD length{static_cast<DWORD>(
            std::min(output.size(), platform_contract::max_single_io_size(PlatformKind::Windows)))};
        for (;;) {
            DWORD amount{0};
            if (::ReadFile(handle_, output.data(), length, &amount, nullptr) != 0) {
                return static_cast<std::size_t>(amount);
            }
            const DWORD code{::GetLastError()};
            if (code == ERROR_BROKEN_PIPE || code == ERROR_HANDLE_EOF) {
                return 0;
            }
            if (code == ERROR_OPERATION_ABORTED) {
                continue;
            }
            return std::unexpected(detail::native_error(Operation::Read, static_cast<int>(code)));
        }
#else
        constexpr PlatformKind PLATFORM{
#if defined(__APPLE__)
            PlatformKind::Darwin
#else
            PlatformKind::Linux
#endif
        };
        const std::size_t length{
            std::min(output.size(), platform_contract::max_single_io_size(PLATFORM))};
        for (;;) {
            const ssize_t amount{detail::read_native(handle_, output.data(), length)};
            if (amount >= 0) {
                return static_cast<std::size_t>(amount);
            }
            if (errno == EINTR
#if defined(__APPLE__)
                && false
#endif
            ) {
                continue;
            }
            return std::unexpected(
                detail::native_error(Operation::Read, detail::last_native_error()));
        }
#endif
    }

    [[gnu::always_inline]] inline Result<std::size_t>
    write(std::span<const std::byte> input) noexcept {
        if (!is_open()) {
            return std::unexpected(detail::error(ErrorCode::BadDescriptor, Operation::Write));
        }
        if (input.empty()) {
            return 0;
        }
#if defined(_WIN32)
        const DWORD length{static_cast<DWORD>(
            std::min(input.size(), platform_contract::max_single_io_size(PlatformKind::Windows)))};
        DWORD amount{0};
        OVERLAPPED appendOffset{};
        OVERLAPPED* offset{nullptr};
        if (append_) {
            appendOffset.Offset = std::numeric_limits<DWORD>::max();
            appendOffset.OffsetHigh = std::numeric_limits<DWORD>::max();
            offset = &appendOffset;
        }
        if (::WriteFile(handle_, input.data(), length, &amount, offset) != 0) {
            return static_cast<std::size_t>(amount);
        }
        return std::unexpected(detail::native_error(Operation::Write, detail::last_native_error()));
#else
        constexpr PlatformKind PLATFORM{
#if defined(__APPLE__)
            PlatformKind::Darwin
#else
            PlatformKind::Linux
#endif
        };
        const std::size_t length{
            std::min(input.size(), platform_contract::max_single_io_size(PLATFORM))};
        for (;;) {
            const ssize_t amount{detail::write_native(handle_, input.data(), length)};
            if (amount >= 0) {
                return static_cast<std::size_t>(amount);
            }
            if (errno == EINTR
#if defined(__APPLE__)
                && false
#endif
            ) {
                continue;
            }
            return std::unexpected(
                detail::native_error(Operation::Write, detail::last_native_error()));
        }
#endif
    }

    [[gnu::always_inline]] inline Result<std::size_t>
    read_all(std::span<std::byte> output) noexcept {
        return read_all_chunks([this](std::span<std::byte> chunk) { return read(chunk); }, output);
    }

    [[gnu::always_inline]] inline Result<void>
    write_all(std::span<const std::byte> input) noexcept {
        auto amount{
            write_all_chunks([this](std::span<const std::byte> chunk) { return write(chunk); },
                             input)};
        if (!amount) {
            return std::unexpected(std::move(amount.error()));
        }
        return {};
    }

    [[gnu::always_inline]] inline Result<std::size_t>
    read_at(std::uint64_t offset, std::span<std::byte> output) noexcept {
        if (!is_open()) {
            return std::unexpected(detail::error(ErrorCode::BadDescriptor, Operation::Read));
        }
        if (output.empty()) {
            return 0;
        }
        if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return std::unexpected(detail::error(ErrorCode::InvalidArgument, Operation::Read));
        }
#if defined(_WIN32)
        const DWORD length{static_cast<DWORD>(
            std::min(output.size(), platform_contract::max_single_io_size(PlatformKind::Windows)))};
        for (;;) {
            OVERLAPPED overlapped{};
            overlapped.Offset = static_cast<DWORD>(offset);
            overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
            DWORD amount{0};
            if (::ReadFile(handle_, output.data(), length, &amount, &overlapped) != 0) {
                return static_cast<std::size_t>(amount);
            }
            const DWORD code{::GetLastError()};
            if (code == ERROR_BROKEN_PIPE || code == ERROR_HANDLE_EOF) {
                return 0;
            }
            if (code == ERROR_OPERATION_ABORTED) {
                continue;
            }
            return std::unexpected(detail::native_error(Operation::Read, static_cast<int>(code)));
        }
#else
        constexpr PlatformKind PLATFORM{
#if defined(__APPLE__)
            PlatformKind::Darwin
#else
            PlatformKind::Linux
#endif
        };
        const std::size_t length{
            std::min(output.size(), platform_contract::max_single_io_size(PLATFORM))};
        for (;;) {
            const ssize_t amount{
                detail::pread_native(handle_, output.data(), length, static_cast<off_t>(offset))};
            if (amount >= 0) {
                return static_cast<std::size_t>(amount);
            }
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(
                detail::native_error(Operation::Read, detail::last_native_error()));
        }
#endif
    }

    [[gnu::always_inline]] inline Result<std::size_t>
    read_all_at(std::uint64_t offset, std::span<std::byte> output) noexcept {
        return read_all_chunks(
            [this, &offset](std::span<std::byte> chunk) -> Result<std::size_t> {
                auto amount{read_at(offset, chunk)};
                if (amount) {
                    if (*amount > std::numeric_limits<std::uint64_t>::max() - offset) [[unlikely]] {
                        return std::unexpected(
                            detail::error(ErrorCode::InvalidArgument, Operation::Read));
                    }
                    offset += *amount;
                }
                return amount;
            },
            output);
    }

    [[gnu::always_inline]] inline Result<std::size_t>
    write_at(std::uint64_t offset, std::span<const std::byte> input) noexcept {
        if (!is_open()) {
            return std::unexpected(detail::error(ErrorCode::BadDescriptor, Operation::Write));
        }
        if (input.empty()) {
            return 0;
        }
        if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return std::unexpected(detail::error(ErrorCode::InvalidArgument, Operation::Write));
        }
#if defined(_WIN32)
        const DWORD length{static_cast<DWORD>(
            std::min(input.size(), platform_contract::max_single_io_size(PlatformKind::Windows)))};
        OVERLAPPED overlapped{};
        overlapped.Offset = static_cast<DWORD>(offset);
        overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD amount{0};
        if (::WriteFile(handle_, input.data(), length, &amount, &overlapped) != 0) {
            return static_cast<std::size_t>(amount);
        }
        return std::unexpected(detail::native_error(Operation::Write, detail::last_native_error()));
#else
        constexpr PlatformKind PLATFORM{
#if defined(__APPLE__)
            PlatformKind::Darwin
#else
            PlatformKind::Linux
#endif
        };
        const std::size_t length{
            std::min(input.size(), platform_contract::max_single_io_size(PLATFORM))};
        for (;;) {
            const ssize_t amount{
                detail::pwrite_native(handle_, input.data(), length, static_cast<off_t>(offset))};
            if (amount >= 0) {
                return static_cast<std::size_t>(amount);
            }
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(
                detail::native_error(Operation::Write, detail::last_native_error()));
        }
#endif
    }

    [[gnu::always_inline]] inline Result<void>
    write_all_at(std::uint64_t offset, std::span<const std::byte> input) noexcept {
        auto amount{write_all_chunks(
            [this, &offset](std::span<const std::byte> chunk) -> Result<std::size_t> {
                auto written{write_at(offset, chunk)};
                if (written) {
                    if (*written > std::numeric_limits<std::uint64_t>::max() - offset)
                        [[unlikely]] {
                        return std::unexpected(
                            detail::error(ErrorCode::InvalidArgument, Operation::Write));
                    }
                    offset += *written;
                }
                return written;
            },
            input)};
        if (!amount) {
            return std::unexpected(std::move(amount.error()));
        }
        return {};
    }

    Result<std::uint64_t> seek(std::int64_t offset, SeekOrigin origin) noexcept {
        if (!is_open()) {
            return std::unexpected(detail::error(ErrorCode::BadDescriptor, Operation::Seek));
        }
#if defined(_WIN32)
        DWORD method{FILE_BEGIN};
        switch (origin) {
            case SeekOrigin::Begin:
                method = FILE_BEGIN;
                break;
            case SeekOrigin::Current:
                method = FILE_CURRENT;
                break;
            case SeekOrigin::End:
                method = FILE_END;
                break;
        }
        LARGE_INTEGER distance{};
        distance.QuadPart = offset;
        LARGE_INTEGER position{};
        if (::SetFilePointerEx(handle_, distance, &position, method) == 0) {
            return std::unexpected(
                detail::native_error(Operation::Seek, detail::last_native_error()));
        }
        return static_cast<std::uint64_t>(position.QuadPart);
#else
        int whence{SEEK_SET};
        switch (origin) {
            case SeekOrigin::Begin:
                whence = SEEK_SET;
                break;
            case SeekOrigin::Current:
                whence = SEEK_CUR;
                break;
            case SeekOrigin::End:
                whence = SEEK_END;
                break;
        }
        for (;;) {
            const off_t position{::lseek(handle_, static_cast<off_t>(offset), whence)};
            if (position >= 0) {
                return static_cast<std::uint64_t>(position);
            }
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(
                detail::native_error(Operation::Seek, detail::last_native_error()));
        }
#endif
    }

    Result<std::uint64_t> position() noexcept {
        return seek(0, SeekOrigin::Current);
    }

    Result<Metadata> metadata() const noexcept {
        if (!is_open()) {
            return std::unexpected(detail::error(ErrorCode::BadDescriptor, Operation::Stat));
        }
#if defined(_WIN32)
        return detail::metadata_from_handle(handle_);
#else
        struct stat value{};
        for (;;) {
            if (::fstat(handle_, &value) == 0) {
                return detail::metadata_from_stat(value);
            }
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(
                detail::native_error(Operation::Stat, detail::last_native_error()));
        }
#endif
    }

    Result<void> truncate(std::uint64_t size) noexcept {
        if (!is_open()) {
            return std::unexpected(detail::error(ErrorCode::BadDescriptor, Operation::Truncate));
        }
        if (size > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return std::unexpected(detail::error(ErrorCode::InvalidArgument, Operation::Truncate));
        }
#if defined(_WIN32)
        FILE_END_OF_FILE_INFO end{};
        end.EndOfFile.QuadPart = static_cast<LONGLONG>(size);
        if (::SetFileInformationByHandle(handle_, FileEndOfFileInfo, &end, sizeof(end)) == 0) {
            return std::unexpected(
                detail::native_error(Operation::Truncate, detail::last_native_error()));
        }
#else
        for (;;) {
            if (::ftruncate(handle_, static_cast<off_t>(size)) == 0) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(
                detail::native_error(Operation::Truncate, detail::last_native_error()));
        }
#endif
        return {};
    }

    Result<void> sync() noexcept {
        if (!is_open()) {
            return std::unexpected(detail::error(ErrorCode::BadDescriptor, Operation::Sync));
        }
#if defined(_WIN32)
        if (::FlushFileBuffers(handle_) == 0) {
            return std::unexpected(
                detail::native_error(Operation::Sync, detail::last_native_error()));
        }
#else
        for (;;) {
            if (::fsync(handle_) == 0) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(
                detail::native_error(Operation::Sync, detail::last_native_error()));
        }
#if defined(__APPLE__)
        if (::fcntl(handle_, F_FULLFSYNC) != 0) {
            return std::unexpected(
                detail::native_error(Operation::Sync, detail::last_native_error()));
        }
#endif
#endif
        return {};
    }
};

Result<Metadata> metadata(const std::filesystem::path& path) {
#if defined(_WIN32)
    const HANDLE handle{::CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
    if (handle == INVALID_HANDLE_VALUE) {
        const int code{detail::last_native_error()};
        return std::unexpected(detail::native_error(Operation::Stat, code, path));
    }
    auto result{detail::metadata_from_handle(handle)};
    ::CloseHandle(handle);
    if (!result) {
        result.error() = detail::with_paths(std::move(result.error()), path);
    }
    return result;
#else
    struct stat value{};
    for (;;) {
        if (::stat(path.c_str(), &value) == 0) {
            return detail::metadata_from_stat(value);
        }
        if (errno == EINTR) {
            continue;
        }
        const int code{detail::last_native_error()};
        return std::unexpected(detail::native_error(Operation::Stat, code, path));
    }
#endif
}

Result<std::vector<std::byte>> read_file(const std::filesystem::path& path) {
    auto opened{File::open(path)};
    if (!opened) {
        return std::unexpected(std::move(opened.error()));
    }
    File file{std::move(*opened)};

    std::uint64_t sizeHint{0};
    if (auto stat{file.metadata()}; stat && stat->is_file()) {
        sizeHint = stat->size;
    }
    if (sizeHint > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max() - 16)) {
        return std::unexpected(
            detail::with_paths(Error{.code = ErrorCode::FileTooLarge, .operation = Operation::Read},
                               path));
    }

    std::vector<std::byte> output;
    try {
        output.resize(std::max<std::size_t>(64, static_cast<std::size_t>(sizeHint) + 16));
    } catch (const std::bad_alloc&) {
        return std::unexpected(detail::allocation_error(Operation::Read));
    } catch (const std::length_error&) {
        return std::unexpected(
            detail::with_paths(Error{.code = ErrorCode::FileTooLarge, .operation = Operation::Read},
                               path));
    }

    std::size_t total{0};
    for (;;) {
        if (total == output.size()) {
            const std::size_t growth{std::max<std::size_t>(16, output.size() / 2)};
            if (growth > output.max_size() - output.size()) {
                return std::unexpected(detail::with_paths(Error{.code = ErrorCode::FileTooLarge,
                                                                .operation = Operation::Read},
                                                          path));
            }
            try {
                output.resize(output.size() + growth);
            } catch (const std::bad_alloc&) {
                return std::unexpected(detail::allocation_error(Operation::Read));
            } catch (const std::length_error&) {
                return std::unexpected(detail::with_paths(Error{.code = ErrorCode::FileTooLarge,
                                                                .operation = Operation::Read},
                                                          path));
            }
        }
        auto amount{file.read_at(total, std::span{output}.subspan(total))};
        if (!amount) {
            return std::unexpected(detail::with_paths(std::move(amount.error()), path));
        }
        if (*amount == 0) {
            output.resize(total);
            return output;
        }
        total += *amount;
    }
}

Result<void> write_file(const std::filesystem::path& path, std::span<const std::byte> input,
                        std::uint32_t mode = 0664) {
    auto opened{File::open(path, OpenOptions{.access = Access::WriteOnly,
                                             .create = true,
                                             .truncate = true,
                                             .mode = mode})};
    if (!opened) {
        return std::unexpected(std::move(opened.error()));
    }
    auto result{opened->write_all(input)};
    if (!result) {
        return std::unexpected(detail::with_paths(std::move(result.error()), path));
    }
    return {};
}

Result<void> atomic_write_file(const std::filesystem::path& path, std::span<const std::byte> input,
                               std::uint32_t mode = 0664) {
#if !defined(_WIN32)
    std::filesystem::path parent;
    std::filesystem::path filename;
    try {
        parent = path.parent_path();
        filename = path.filename();
        if (parent.empty()) {
            parent = ".";
        }
    } catch (...) {
        return std::unexpected(detail::allocation_error(Operation::Open));
    }
    if (filename.empty() || filename == "." || filename == "..") {
        return std::unexpected(detail::with_paths(Error{.code = ErrorCode::InvalidArgument,
                                                        .operation = Operation::Open},
                                                  path));
    }

    int directoryFlags{O_RDONLY};
#if defined(O_DIRECTORY)
    directoryFlags |= O_DIRECTORY;
#endif
#if defined(O_CLOEXEC)
    directoryFlags |= O_CLOEXEC;
#endif
#if defined(O_NOFOLLOW)
    directoryFlags |= O_NOFOLLOW;
#endif
    int directory{-1};
#if defined(__APPLE__)
    directory = detail::openat_nocancel(AT_FDCWD, parent.c_str(), directoryFlags, 0);
#else
    while ((directory = ::open(parent.c_str(), directoryFlags)) < 0 && errno == EINTR) {}
#endif
    if (directory < 0) {
        return std::unexpected(
            detail::native_error(Operation::Open, detail::last_native_error(), parent));
    }

    auto closeDescriptor = [](int descriptor) noexcept {
#if defined(__APPLE__)
        return detail::close_nocancel(descriptor);
#else
        return ::close(descriptor);
#endif
    };
    auto closeDirectory = [&] { (void)closeDescriptor(std::exchange(directory, -1)); };

    constexpr std::size_t TEMP_NAME_SIZE{27};
    const long nameMax{::fpathconf(directory, _PC_NAME_MAX)};
    if (nameMax >= 0 && static_cast<std::size_t>(nameMax) < TEMP_NAME_SIZE) {
        closeDirectory();
        return std::unexpected(
            detail::with_paths(Error{.code = ErrorCode::NameTooLong, .operation = Operation::Open},
                               parent));
    }

    std::array<char, 32> temporaryName{};
    int temporary{-1};
    for (unsigned attempt{0}; attempt < 128; ++attempt) {
        auto nonce{detail::random_nonce()};
        if (!nonce) {
            closeDirectory();
            return std::unexpected(detail::with_paths(std::move(nonce.error()), parent, path));
        }
        (void)std::snprintf(temporaryName.data(), temporaryName.size(), ".mbun-%016llx.tmp",
                            static_cast<unsigned long long>(*nonce));
        int flags{O_WRONLY | O_CREAT | O_EXCL};
#if defined(O_CLOEXEC)
        flags |= O_CLOEXEC;
#endif
#if defined(O_NOFOLLOW)
        flags |= O_NOFOLLOW;
#endif
#if defined(__APPLE__)
        temporary = detail::openat_nocancel(directory, temporaryName.data(), flags,
                                            static_cast<mode_t>(mode & 07777));
#else
        while ((temporary = ::openat(directory, temporaryName.data(), flags,
                                     static_cast<mode_t>(mode & 07777))) < 0 &&
               errno == EINTR) {}
#endif
        if (temporary >= 0) {
            break;
        }
        const int code{detail::last_native_error()};
        if (code != EEXIST) {
            closeDirectory();
            return std::unexpected(detail::native_error(Operation::Open, code, parent, path));
        }
    }
    if (temporary < 0) {
        closeDirectory();
        return std::unexpected(detail::with_paths(Error{.code = ErrorCode::AlreadyExists,
                                                        .operation = Operation::Open},
                                                  parent, path));
    }

    bool published{false};
    auto cleanup = [&] {
        if (temporary >= 0) {
            (void)closeDescriptor(std::exchange(temporary, -1));
        }
        if (!published) {
            (void)::unlinkat(directory, temporaryName.data(), 0);
        }
        closeDirectory();
    };
    std::size_t offset{0};
    while (offset < input.size()) {
        constexpr PlatformKind PLATFORM{
#if defined(__APPLE__)
            PlatformKind::Darwin
#else
            PlatformKind::Linux
#endif
        };
        const std::size_t length{
            std::min(input.size() - offset, platform_contract::max_single_io_size(PLATFORM))};
        const ssize_t amount{detail::write_native(temporary, input.data() + offset, length)};
        if (amount > 0) {
            offset += static_cast<std::size_t>(amount);
            continue;
        }
        if (amount == 0) {
            break;  // Bun writeAll treats zero progress as early success.
        }
        if (errno == EINTR
#if defined(__APPLE__)
            && false
#endif
        ) {
            continue;
        }
        const int code{detail::last_native_error()};
        cleanup();
        return std::unexpected(detail::native_error(Operation::Write, code, parent, path));
    }

    int syncResult{0};
    do {
        syncResult = ::fsync(temporary);
    } while (syncResult != 0 && errno == EINTR);
    if (syncResult == 0) {
#if defined(__APPLE__)
        syncResult = ::fcntl(temporary, F_FULLFSYNC);
#endif
    }
    if (syncResult != 0) {
        const int code{detail::last_native_error()};
        cleanup();
        return std::unexpected(detail::native_error(Operation::Sync, code, parent, path));
    }
    if (closeDescriptor(std::exchange(temporary, -1)) != 0) {
        const int code{detail::last_native_error()};
        cleanup();
        return std::unexpected(detail::native_error(Operation::Close, code, parent, path));
    }

    int renameResult{0};
    do {
        renameResult = ::renameat(directory, temporaryName.data(), directory, filename.c_str());
    } while (renameResult != 0 && errno == EINTR);
    if (renameResult != 0) {
        const int code{detail::last_native_error()};
        cleanup();
        return std::unexpected(detail::native_error(Operation::Rename, code, parent, path));
    }
    published = true;

    do {
        syncResult = ::fsync(directory);
    } while (syncResult != 0 && errno == EINTR);
    if (syncResult != 0) {
        const int code{detail::last_native_error()};
        cleanup();
        return std::unexpected(detail::native_error(Operation::Sync, code, parent, path));
    }
    cleanup();
    return {};
#else
    std::filesystem::path parent;
    std::wstring filename;
    try {
        parent = path.parent_path();
        filename = path.filename().native();
        if (parent.empty()) {
            parent = L".";
        }
    } catch (...) {
        return std::unexpected(detail::allocation_error(Operation::Open));
    }
    if (filename.empty() || filename == L"." || filename == L"..") {
        return std::unexpected(detail::with_paths(Error{.code = ErrorCode::InvalidArgument,
                                                        .operation = Operation::Open},
                                                  path));
    }

    const HANDLE directory{::CreateFileW(parent.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                         nullptr, OPEN_EXISTING,
                                         FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                         nullptr)};
    if (directory == INVALID_HANDLE_VALUE) {
        return std::unexpected(
            detail::native_error(Operation::Open, detail::last_native_error(), parent));
    }

    std::array<wchar_t, 32> temporaryName{};
    HANDLE temporary{INVALID_HANDLE_VALUE};
    for (unsigned attempt{0}; attempt < 128; ++attempt) {
        auto nonce{detail::random_nonce()};
        if (!nonce) {
            (void)::CloseHandle(directory);
            return std::unexpected(detail::with_paths(std::move(nonce.error()), parent, path));
        }
        (void)std::swprintf(temporaryName.data(), temporaryName.size(), L".mbun-%016llx.tmp",
                            static_cast<unsigned long long>(*nonce));
        auto opened{detail::create_relative_file(directory, temporaryName.data(), mode)};
        if (opened) {
            temporary = *opened;
            break;
        }
        if (opened.error().code != ErrorCode::AlreadyExists) {
            (void)::CloseHandle(directory);
            return std::unexpected(detail::with_paths(std::move(opened.error()), parent, path));
        }
    }
    if (temporary == INVALID_HANDLE_VALUE) {
        (void)::CloseHandle(directory);
        return std::unexpected(detail::with_paths(Error{.code = ErrorCode::AlreadyExists,
                                                        .operation = Operation::Open},
                                                  parent, path));
    }

    bool published{false};
    auto cleanup = [&]() -> Result<void> {
        Result<void> result{};
        if (!published) {
            result = detail::mark_delete(temporary);
        }
        if (::CloseHandle(temporary) == 0 && result) {
            result = std::unexpected(
                detail::native_error(Operation::Close, detail::last_native_error()));
        }
        if (::CloseHandle(directory) == 0 && result) {
            result = std::unexpected(
                detail::native_error(Operation::Close, detail::last_native_error()));
        }
        return result;
    };
    auto failAfterCleanup = [&](Error error) -> Result<void> {
        if (auto cleaned{cleanup()}; !cleaned) {
            return std::unexpected(detail::with_paths(std::move(cleaned.error()), parent, path));
        }
        return std::unexpected(detail::with_paths(std::move(error), parent, path));
    };
    std::size_t offset{0};
    while (offset < input.size()) {
        const DWORD length{static_cast<DWORD>(
            std::min(input.size() - offset,
                     platform_contract::max_single_io_size(PlatformKind::Windows)))};
        DWORD amount{0};
        if (::WriteFile(temporary, input.data() + offset, length, &amount, nullptr) == 0) {
            const int code{detail::last_native_error()};
            return failAfterCleanup(detail::native_error(Operation::Write, code));
        }
        if (amount == 0) {
            break;
        }
        offset += amount;
    }
    if (::FlushFileBuffers(temporary) == 0) {
        const int code{detail::last_native_error()};
        return failAfterCleanup(detail::native_error(Operation::Sync, code));
    }
    if (auto renamed{detail::rename_relative(temporary, directory, filename)}; !renamed) {
        return failAfterCleanup(std::move(renamed.error()));
    }
    published = true;
    if (auto cleared{detail::clear_file_attributes(temporary, FILE_ATTRIBUTE_TEMPORARY)};
        !cleared) {
        Error error{detail::with_paths(std::move(cleared.error()), parent, path)};
        (void)cleanup();
        return std::unexpected(std::move(error));
    }
    if (::FlushFileBuffers(directory) == 0) {
        const int code{detail::last_native_error()};
        Error error{detail::native_error(Operation::Sync, code, parent, path)};
        (void)cleanup();
        return std::unexpected(std::move(error));
    }
    if (auto closed{cleanup()}; !closed) {
        return std::unexpected(detail::with_paths(std::move(closed.error()), parent, path));
    }
    return {};
#endif
}

}  // namespace mbun::core::io
