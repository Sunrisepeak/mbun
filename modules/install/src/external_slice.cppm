// external_slice.cppm — mbun.install.external_slice
//
// Mechanical port of bun src/install/ExternalSlice.rs +
// bun_install_types::resolver_hooks::ExternalSlice / ExternalStringMap.
//
// An `(off, len)` index pair into a flat backing buffer (lockfile string-bytes /
// dependencies / resolutions), generic over element type. Storage is two u32s;
// Rust carries a `PhantomData<T>` marker — C++ templates need no runtime marker,
// so `ExternalSlice<T>` is just `{off, len}`.
export module mbun.install.external_slice;

import std;

namespace mbun::install {

// (off, len) window into a flat `std::span<const T>` backing buffer.
export template <class T>
struct ExternalSlice {
    std::uint32_t off{0};
    std::uint32_t len{0};

    constexpr ExternalSlice() = default;
    constexpr ExternalSlice(std::uint32_t off_, std::uint32_t len_) : off{off_}, len{len_} {}

    static constexpr ExternalSlice invalid() {
        return ExternalSlice{std::numeric_limits<std::uint32_t>::max(),
                             std::numeric_limits<std::uint32_t>::max()};
    }

    bool is_invalid() const {
        return off == std::numeric_limits<std::uint32_t>::max() &&
               len == std::numeric_limits<std::uint32_t>::max();
    }

    // id ∈ [off, off+len).
    bool contains(std::uint32_t id) const {
        return id >= off &&
               static_cast<std::uint64_t>(id) <
                   static_cast<std::uint64_t>(len) + static_cast<std::uint64_t>(off);
    }

    std::uint32_t begin() const {
        return off;
    }

    std::uint32_t end() const {
        return off + len;
    }

    // Release-mode clamp (mirrors the Rust `in_.len().min(off+len)` guard) so a
    // corrupt lockfile slice degrades to a short span instead of UB.
    std::span<const T> get(std::span<const T> in) const {
        std::size_t start{std::min<std::size_t>(off, in.size())};
        std::size_t stop{std::min<std::size_t>(static_cast<std::size_t>(off) + len, in.size())};
        return in.subspan(start, stop - start);
    }

    // Derive the slice from a sub-span of `buf`. Element-index arithmetic
    // matches Rust's pointer-difference / size_of division.
    static ExternalSlice init(std::span<const T> buf, std::span<const T> in) {
        std::size_t off_elems{static_cast<std::size_t>(in.data() - buf.data())};
        return ExternalSlice{static_cast<std::uint32_t>(off_elems),
                             static_cast<std::uint32_t>(in.size())};
    }

    friend bool operator==(const ExternalSlice&, const ExternalSlice&) = default;
};

// A handle into the lockfile string bytes (bun's ExternalString is itself an
// (off, len) window into the string buffer). Element type of ExternalStringList.
export struct ExternalString {
    std::uint32_t off{0};
    std::uint32_t len{0};
    friend bool operator==(const ExternalString&, const ExternalString&) = default;
};

export using ExternalStringList = ExternalSlice<ExternalString>;
export using ExternalPackageNameHashList = ExternalSlice<std::uint64_t>;

// name/value parallel string lists (e.g. package.json `scripts`, overrides).
export struct ExternalStringMap {
    ExternalStringList name{};
    ExternalStringList value{};
    friend bool operator==(const ExternalStringMap&, const ExternalStringMap&) = default;
};

}  // namespace mbun::install
