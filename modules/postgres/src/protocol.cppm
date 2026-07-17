// PostgreSQL frontend/backend framing primitives. Transport is DEFERRED(S-net).
// Reference: bun-ref src/sql/postgres/PostgresProtocol.rs and Zig equivalent.
export module mbun.postgres.protocol;

import std;

namespace mbun::postgres {

export struct WireFrame {
    std::uint8_t tag {};
    std::vector<std::byte> payload {};
};

export inline constexpr std::array<std::byte, 5> make_fixed_frame(std::uint8_t tag) {
    return { std::byte { tag }, std::byte { 0 }, std::byte { 0 }, std::byte { 0 }, std::byte { 4 } };
}

export inline constexpr auto CLOSE_COMPLETE = make_fixed_frame('3');
export inline constexpr auto EMPTY_QUERY_RESPONSE = make_fixed_frame('I');
export inline constexpr auto TERMINATE = make_fixed_frame('X');
export inline constexpr auto BIND_COMPLETE = make_fixed_frame('2');
export inline constexpr auto PARSE_COMPLETE = make_fixed_frame('1');
export inline constexpr auto COPY_DONE = make_fixed_frame('c');
export inline constexpr auto SYNC = make_fixed_frame('S');
export inline constexpr auto FLUSH = make_fixed_frame('H');
export inline constexpr auto NO_DATA = make_fixed_frame('n');

export inline std::vector<std::byte> write_query(std::string_view query) {
    // PostgreSQL message length includes itself and the terminating NUL.
    auto length { static_cast<std::uint32_t>(sizeof(std::uint32_t) + query.size() + 1) };
    std::vector<std::byte> frame;
    frame.reserve(sizeof(std::uint8_t) + sizeof(length) + query.size() + 1);
    frame.push_back(std::byte { 'Q' });
    frame.push_back(std::byte { static_cast<unsigned char>(length >> 24) });
    frame.push_back(std::byte { static_cast<unsigned char>(length >> 16) });
    frame.push_back(std::byte { static_cast<unsigned char>(length >> 8) });
    frame.push_back(std::byte { static_cast<unsigned char>(length) });
    for (auto c : query) frame.push_back(std::byte { static_cast<unsigned char>(c) });
    frame.push_back(std::byte { 0 });
    return frame;
}

} // namespace mbun::postgres
