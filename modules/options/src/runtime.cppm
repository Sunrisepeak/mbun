export module mbun.options.runtime;

import std;

namespace mbun::options {

// Ref: bun options_types/Context.rs and options_types/Context.zig.
export enum class DnsResultOrder : std::uint8_t { Verbatim, V4First, V6First };

export enum class HotReload : std::uint8_t { None, Hot, Watch };

export struct EvalOptions {
    bool enabled{false};
    bool print_result{false};
    std::string script;
};

export struct DebuggerOptions {
    bool enabled{false};
    bool wait_for_connection{false};
    std::string path_or_port;
};

export struct RuntimeOptions {
    bool smol{false};
    bool if_present{false};
    bool expose_gc{false};
    bool preserve_symlinks_main{false};
    bool experimental_http2_fetch{false};
    bool experimental_http3_fetch{false};
    DnsResultOrder dns_result_order{DnsResultOrder::Verbatim};
    HotReload hot_reload{HotReload::None};
    EvalOptions eval{};
    DebuggerOptions debugger{};
    std::vector<std::string> preconnect;
};

} // namespace mbun::options
