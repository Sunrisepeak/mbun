export module mbun.sys_bindings.errno;

import std;

namespace mbun::sys_bindings::errno_ {

// Ref: bun-ref/src/errno/*.rs and bun-zig-src/src/errno/*.zig. Keep the
// platform numeric value separate from the portable category.
export enum class Category : std::uint8_t {
    unknown,
    again,
    canceled,
    bad_file_descriptor,
    interrupted,
    no_entry,
    permission_denied,
    invalid_argument,
    not_supported,
    out_of_memory,
};

export struct Code {
    std::int32_t native {};
    Category category {Category::unknown};
};

export constexpr Category classify(std::errc value) {
    switch (value) {
    case std::errc::resource_unavailable_try_again: return Category::again;
    case std::errc::operation_canceled: return Category::canceled;
    case std::errc::bad_file_descriptor: return Category::bad_file_descriptor;
    case std::errc::interrupted: return Category::interrupted;
    case std::errc::no_such_file_or_directory: return Category::no_entry;
    case std::errc::permission_denied: return Category::permission_denied;
    case std::errc::invalid_argument: return Category::invalid_argument;
    case std::errc::not_supported: return Category::not_supported;
    case std::errc::not_enough_memory: return Category::out_of_memory;
    default: return Category::unknown;
    }
}

} // namespace mbun::sys_bindings::errno_
