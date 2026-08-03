// Windows path facts used by syscall consumers.
// Ref: bun-ref/src/fs/path.rs and bun-zig-src/src/windows_sys/externs.zig.
// UTF-16 conversion and GetFullPathNameW are DEFERRED(S-windows).
export module mbun.windows_sys.path;

import std;

export namespace mbun::windows_sys {

inline constexpr char16_t WINDOWS_SEPARATOR{u'\\'};

constexpr bool is_drive_letter(char16_t value) noexcept {
    return (value >= u'A' && value <= u'Z') || (value >= u'a' && value <= u'z');
}

constexpr bool has_extended_prefix(std::u16string_view path) noexcept {
    return path.size() >= 4 && path[0] == u'\\' && path[1] == u'\\' &&
           (path[2] == u'?' || path[2] == u'.') && path[3] == u'\\';
}

constexpr bool is_unc_path(std::u16string_view path) noexcept {
    return path.size() >= 2 && path[0] == u'\\' && path[1] == u'\\';
}

constexpr bool is_absolute_path(std::u16string_view path) noexcept {
    if (path.empty()) return false;
    if (is_unc_path(path)) return true;
    if (path.front() == u'\\' || path.front() == u'/') return true;
    return path.size() >= 3 && is_drive_letter(path[0]) && path[1] == u':' &&
           (path[2] == u'\\' || path[2] == u'/');
}

constexpr char16_t preferred_separator() noexcept { return WINDOWS_SEPARATOR; }

} // namespace mbun::windows_sys
