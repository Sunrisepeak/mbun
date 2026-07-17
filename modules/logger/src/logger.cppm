// Initial pure core port from bun-ref/src/ast/lib.rs and bun-zig-src/src/logger/logger.zig.
// JSC conversion and IO backends remain DEFERRED(S1).
export module mbun.logger;
import std;

namespace mbun::logger {

export enum class Kind : std::uint8_t { error, warning, note, debug, verbose };
export enum class Level : std::int8_t { verbose, debug, info, warning, error };

export constexpr bool should_print(Kind kind, Level threshold) noexcept {
    switch (threshold) {
    case Level::error: return kind == Kind::error || kind == Kind::note;
    case Level::warning: return kind == Kind::error || kind == Kind::warning || kind == Kind::note;
    case Level::info:
    case Level::debug: return kind != Kind::verbose;
    case Level::verbose: return true;
    }
    return false;
}

export constexpr std::string_view label(Kind kind) noexcept {
    switch (kind) {
    case Kind::error: return "error";
    case Kind::warning: return "warn";
    case Kind::note: return "note";
    case Kind::debug: return "debug";
    case Kind::verbose: return "verbose";
    }
    return {};
}

export constexpr std::optional<Level> parse_level(std::string_view text) noexcept {
    if (text == "verbose") return Level::verbose;
    if (text == "debug") return Level::debug;
    if (text == "info") return Level::info;
    if (text == "warn") return Level::warning;
    if (text == "error") return Level::error;
    return std::nullopt;
}

export struct Loc {
    static constexpr std::int32_t EMPTY{-1};
    std::int32_t start{EMPTY};
    constexpr bool is_empty() const noexcept { return start == EMPTY; }
    constexpr std::size_t index() const noexcept { return static_cast<std::size_t>(std::max(start, std::int32_t{0})); }
};

export struct Range {
    Loc loc{};
    std::int32_t length{0};
    constexpr bool is_empty() const noexcept { return loc.is_empty() && length == 0; }
    constexpr bool contains(std::int32_t offset) const noexcept { return offset >= loc.start && offset < loc.start + length; }
    std::string_view in(std::string_view source) const noexcept {
        if (loc.start < 0 || length <= 0 || static_cast<std::size_t>(loc.start) >= source.size()) return {};
        const auto begin{static_cast<std::size_t>(loc.start)};
        return source.substr(begin, std::min(static_cast<std::size_t>(length), source.size() - begin));
    }
};

export struct Location {
    std::string file{};
    std::string namespace_name{"file"};
    std::optional<std::string> line_text{};
    std::size_t length{0};
    std::size_t offset{0};
    std::int32_t line{-1};
    std::int32_t column{-1};
};

export struct Data { std::string text{}; std::optional<Location> location{}; };
export enum class MetadataKind : std::uint8_t { build, resolve };
export struct Metadata { MetadataKind kind{MetadataKind::build}; std::string specifier{}; };

export struct Event {
    Kind kind{Kind::error};
    Data data{};
    Metadata metadata{};
    std::vector<Data> notes{};
    bool redact_sensitive_information{false};
};

export class Sink {
public:
    virtual ~Sink() = default;
    virtual void accept(const Event&) = 0;
};

export class VectorSink final : public Sink {
    std::vector<Event> events_{};
public:
    void accept(const Event& event) override { events_.push_back(event); }
    const std::vector<Event>& events() const noexcept { return events_; }
};

export class Provider {
    Level level_{Level::warning};
    Sink* sink_{nullptr};
    std::vector<Event> events_{};
    std::uint32_t warnings_{0};
    std::uint32_t errors_{0};
public:
    explicit Provider(Level level = Level::warning, Sink* sink = nullptr) : level_{level}, sink_{sink} {}
    bool add(Event event) {
        if (!should_print(event.kind, level_)) return false;
        if (event.kind == Kind::error) ++errors_;
        if (event.kind == Kind::warning) ++warnings_;
        if (sink_) sink_->accept(event);
        events_.push_back(std::move(event));
        return true;
    }
    bool add(Kind kind, std::string_view text, std::optional<Location> location = std::nullopt) {
        return add(Event{kind, Data{std::string{text}, std::move(location)}});
    }
    Level level() const noexcept { return level_; }
    void set_level(Level level) noexcept { level_ = level; }
    bool has_errors() const noexcept { return errors_ != 0; }
    bool has_any() const noexcept { return warnings_ != 0 || errors_ != 0; }
    std::uint32_t warning_count() const noexcept { return warnings_; }
    std::uint32_t error_count() const noexcept { return errors_; }
    const std::vector<Event>& events() const noexcept { return events_; }
    void reset() noexcept { events_.clear(); warnings_ = 0; errors_ = 0; }
};

} // namespace mbun::logger
