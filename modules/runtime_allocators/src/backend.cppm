export module mbun.runtime_allocators.backend;

import std;

export namespace mbun::runtime_allocators {

// Ref: bun-ref/src/runtime/allocators/mod.rs and LinuxMemFdAllocator.rs,
// plus bun-zig-src/src/runtime/allocators.  The native vtable and memfd
// syscalls stay behind this portable seam until the runtime/platform layers
// expose their ABI.

enum class Error : std::uint8_t { invalid_alignment, out_of_memory, invalid_block };

using AllocateFn = std::expected<std::byte*, Error> (*)(std::size_t, std::size_t, void*) noexcept;
using DeallocateFn = void (*)(std::byte*, std::size_t, std::size_t, void*) noexcept;

struct Backend {
    AllocateFn allocate_fn { nullptr };
    DeallocateFn deallocate_fn { nullptr };
    void* context { nullptr };
    std::string_view name { "invalid" };

    [[nodiscard]] std::expected<std::byte*, Error> allocate(std::size_t size,
                                                              std::size_t alignment) const noexcept {
        if (allocate_fn == nullptr) {
            return std::unexpected { Error::out_of_memory };
        }
        return allocate_fn(size, alignment, context);
    }

    void deallocate(std::byte* pointer, std::size_t size, std::size_t alignment) const noexcept {
        if (deallocate_fn != nullptr && pointer != nullptr) {
            deallocate_fn(pointer, size, alignment, context);
        }
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return allocate_fn != nullptr && deallocate_fn != nullptr;
    }
};

[[nodiscard]] inline std::expected<std::byte*, Error> system_allocate(std::size_t size,
                                                                        std::size_t alignment,
                                                                        void*) noexcept {
    if (alignment == 0 || !std::has_single_bit(alignment)) {
        return std::unexpected { Error::invalid_alignment };
    }
    try {
        auto* pointer { ::operator new(std::max(size, 1uz), std::align_val_t { alignment },
                                       std::nothrow) };
        return pointer == nullptr ? std::expected<std::byte*, Error> {
                                        std::unexpected { Error::out_of_memory }}
                                  : static_cast<std::byte*>(pointer);
    } catch (...) {
        return std::unexpected { Error::out_of_memory };
    }
}

inline void system_deallocate(std::byte* pointer, std::size_t, std::size_t alignment, void*) noexcept {
    ::operator delete(pointer, std::align_val_t { alignment });
}

[[nodiscard]] inline Backend system_backend() noexcept {
    return { system_allocate, system_deallocate, nullptr, "system" };
}

class CountingBackend {
private:
    std::size_t rejectAbove_ { 0 };
    std::size_t attempts_ { 0 };

    static std::expected<std::byte*, Error> allocate_(std::size_t size, std::size_t alignment,
                                                        void* context) noexcept {
        auto& self { *static_cast<CountingBackend*>(context) };
        ++self.attempts_;
        if (size > self.rejectAbove_) {
            return std::unexpected { Error::out_of_memory };
        }
        return system_allocate(size, alignment, nullptr);
    }

    static void deallocate_(std::byte* pointer, std::size_t size, std::size_t alignment,
                            void*) noexcept {
        system_deallocate(pointer, size, alignment, nullptr);
    }

public:
    explicit CountingBackend(std::size_t rejectAbove) noexcept : rejectAbove_ { rejectAbove } {}
    [[nodiscard]] Backend backend() noexcept {
        return { allocate_, deallocate_, this, "counting" };
    }
    [[nodiscard]] std::size_t attempts() const noexcept { return attempts_; }
};

}
