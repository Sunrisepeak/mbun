export module mbun.runtime_valkey.backend;

import std;

export namespace mbun::runtime_valkey {

enum class ConnectionStatus : unsigned char { disconnected, connecting, connected };

struct Backend {
    using Connect = std::function<bool()>;
    using Write = std::function<bool(std::string_view)>;
    using Close = std::function<void()>;

    Connect connect {};
    Write write {};
    Close close {};

    [[nodiscard]] auto can_connect() const noexcept -> bool { return static_cast<bool>(connect); }
    [[nodiscard]] auto can_write() const noexcept -> bool { return static_cast<bool>(write); }
};

}
