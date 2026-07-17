// RAII library/symbol ownership modeled after bun FFI.dylib + Function state.
export module mbun.runtime_ffi.library;

import std;
import mbun.runtime_ffi.backend;
import mbun.runtime_ffi.error;

export namespace mbun::runtime_ffi {

class Symbol {
private:
    void* address_{};
    std::string name_{};

public:
    Symbol() = default;
    Symbol(void* address, std::string_view name) : address_{address}, name_{name} {}

    void* address() const { return address_; }
    std::string_view name() const { return name_; }
    explicit operator bool() const { return address_ != nullptr; }
};

class Library {
private:
    void* handle_{};
    Backend backend_{};
    std::string name_{};

    Library(void* handle, std::string_view name, Backend backend)
        : handle_{handle}, backend_{std::move(backend)}, name_{name} {}

public:
    Library() = delete;
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;
    Library(Library&& other) noexcept
        : handle_{std::exchange(other.handle_, nullptr)}, backend_{std::move(other.backend_)},
          name_{std::move(other.name_)} {}
    Library& operator=(Library&& other) noexcept {
        if (this != &other) {
            close_();
            handle_ = std::exchange(other.handle_, nullptr);
            backend_ = std::move(other.backend_);
            name_ = std::move(other.name_);
        }
        return *this;
    }
    ~Library() { close_(); }

    static std::expected<Library, Error> open(std::string_view name,
                                               Backend backend = deferred_backend()) {
        if (!backend.load) {
            return std::unexpected(Error::backend_unavailable("load"));
        }
        auto loaded{backend.load(name)};
        if (!loaded) {
            return std::unexpected(std::move(loaded.error()));
        }
        if (*loaded == nullptr) {
            return std::unexpected(Error::missing_library(name));
        }
        return Library{*loaded, name, std::move(backend)};
    }

    std::expected<Symbol, Error> symbol(std::string_view name) const {
        if (!handle_) {
            return std::unexpected(Error::missing_library(name_, "library is closed"));
        }
        if (!backend_.lookup) {
            return std::unexpected(Error::backend_unavailable("lookup"));
        }
        auto found{backend_.lookup(handle_, name)};
        if (!found) {
            return std::unexpected(std::move(found.error()));
        }
        if (*found == nullptr) {
            return std::unexpected(Error::missing_symbol(name));
        }
        return Symbol{*found, name};
    }

    bool is_open() const { return handle_ != nullptr; }
    std::string_view name() const { return name_; }

private:
    void close_() {
        if (handle_ != nullptr && backend_.close) {
            backend_.close(handle_);
        }
        handle_ = nullptr;
    }
};

}  // namespace mbun::runtime_ffi
