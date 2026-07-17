export module mbun.runtime_socket.address;

import std;

export namespace mbun::runtime_socket {

enum class AddressFamily : std::uint8_t { ipv4, ipv6, unix };

enum class AddressError : std::uint8_t {
    empty_host,
    malformed_endpoint,
    invalid_port,
};

class Address {
private:
    AddressFamily family_ { AddressFamily::ipv4 };
    std::string host_ {};
    std::uint16_t port_ { 0 };
    std::uint32_t flow_label_ { 0 };
    std::uint32_t scope_id_ { 0 };

    Address(AddressFamily family, std::string host, std::uint16_t port)
        : family_ { family }
        , host_ { std::move(host) }
        , port_ { port }
    {
    }

    static std::expected<std::uint16_t, AddressError> parse_port_(std::string_view text) {
        if (text.empty())
            return std::uint16_t { 0 };
        std::uint32_t value { 0 };
        const auto [end, error] { std::from_chars(text.data(), text.data() + text.size(), value) };
        if (error != std::errc {} || end != text.data() + text.size()
            || value > std::numeric_limits<std::uint16_t>::max())
            return std::unexpected { AddressError::invalid_port };
        return static_cast<std::uint16_t>(value);
    }

public:
    Address() = default;

    static Address ipv4(std::string host, std::uint16_t port = 0) {
        return Address { AddressFamily::ipv4, std::move(host), port };
    }

    static Address ipv6(std::string host, std::uint16_t port = 0,
                        std::uint32_t flowLabel = 0, std::uint32_t scopeId = 0) {
        Address result { AddressFamily::ipv6, std::move(host), port };
        result.flow_label_ = flowLabel;
        result.scope_id_ = scopeId;
        return result;
    }

    static Address unix_domain(std::string path) {
        return Address { AddressFamily::unix, std::move(path), 0 };
    }

    // Ref: Bun Rust SocketAddress::init/parse and Zig SocketAddress.init.
    // JS/JSC validation remains above this pure native value layer.
    static std::expected<Address, AddressError> parse(std::string_view endpoint) {
        if (endpoint.empty())
            return std::unexpected { AddressError::empty_host };
        if (endpoint.front() == '/')
            return unix_domain(std::string { endpoint });

        if (endpoint.front() == '[') {
            const auto close { endpoint.find(']') };
            if (close == std::string_view::npos || close == 1)
                return std::unexpected { AddressError::malformed_endpoint };
            std::uint16_t port { 0 };
            if (close + 1 < endpoint.size()) {
                if (endpoint[close + 1] != ':')
                    return std::unexpected { AddressError::malformed_endpoint };
                auto parsed { parse_port_(endpoint.substr(close + 2)) };
                if (!parsed)
                    return std::unexpected { parsed.error() };
                port = *parsed;
            }
            return ipv6(std::string { endpoint.substr(1, close - 1) }, port);
        }

        const auto first_colon { endpoint.find(':') };
        const auto last_colon { endpoint.rfind(':') };
        if (first_colon != std::string_view::npos && first_colon != last_colon)
            return ipv6(std::string { endpoint });
        if (first_colon == std::string_view::npos)
            return ipv4(std::string { endpoint });
        auto parsed { parse_port_(endpoint.substr(first_colon + 1)) };
        if (!parsed)
            return std::unexpected { parsed.error() };
        if (first_colon == 0)
            return std::unexpected { AddressError::empty_host };
        return ipv4(std::string { endpoint.substr(0, first_colon) }, *parsed);
    }

    AddressFamily family() const { return family_; }
    std::string_view host() const { return host_; }
    std::uint16_t port() const { return port_; }
    std::uint32_t flow_label() const { return flow_label_; }
    std::uint32_t scope_id() const { return scope_id_; }

    std::string to_string() const {
        if (family_ == AddressFamily::unix)
            return host_;
        std::string result;
        if (family_ == AddressFamily::ipv6)
            result = '[' + host_ + ']';
        else
            result = host_;
        if (port_ != 0)
            result += ':' + std::to_string(port_);
        return result;
    }
};

}
