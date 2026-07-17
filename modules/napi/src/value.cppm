export module mbun.napi.value;

import std;
import mbun.napi.env;

namespace mbun::napi {

// ABI-neutral handle; JSC/native pointers deliberately do not cross this seam.
export class Value {
private:
    std::uint64_t id_{0};

public:
    constexpr Value() noexcept = default;
    explicit constexpr Value(std::uint64_t id) noexcept : id_{id} {}
    [[nodiscard]] constexpr std::uint64_t id() const noexcept { return id_; }
    [[nodiscard]] constexpr bool is_empty() const noexcept { return id_ == 0; }
    // Explicit (not `= default` friend): a defaulted by-value friend operator==
    // on an exported class ICEs GCC 16 when instantiated across a module import.
    [[nodiscard]] constexpr bool operator==(const Value& other) const noexcept { return id_ == other.id_; }
};

export class ValueStore {
private:
    std::uint64_t nextId_{1};

public:
    [[nodiscard]] Value allocate() noexcept { return Value{nextId_++}; }
    [[nodiscard]] bool owns(Value value) const noexcept { return value.id() != 0 && value.id() < nextId_; }
};

export [[nodiscard]] inline std::expected<Value, Status> require_value(const Env& env, Value value) {
    if (env.is_exception_pending()) return std::unexpected{Status::pending_exception};
    if (value.is_empty()) return std::unexpected{Status::invalid_arg};
    return value;
}

} // namespace mbun::napi
