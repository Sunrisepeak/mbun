// Incremental source-map seam for codegen backends.
// ref: bun-ref/src/sourcemap/{Chunk.rs,lib.rs}; bun-zig-src/src/sourcemap/Chunk.zig
export module mbun.codegen.source_map;

import std;

export namespace mbun::codegen {

struct SourceMapState {
    std::int32_t generated_line { 0 };
    std::int32_t generated_column { 0 };
    std::int32_t source_index { 0 };
    std::int32_t original_line { 0 };
    std::int32_t original_column { 0 };
};

struct SourceMapSegment {
    SourceMapState state;
    std::string name;
};

class SourceMapBuilder {
private:
    std::vector<std::string> sources_;
    std::vector<SourceMapSegment> segments_;
    SourceMapState end_state_;

public:
    std::uint32_t add_source(std::string_view source) {
        sources_.emplace_back(source);
        return static_cast<std::uint32_t>(sources_.size() - 1);
    }

    void add_mapping(SourceMapState state, std::string_view name = {}) {
        segments_.push_back(SourceMapSegment { state, std::string(name) });
        end_state_ = state;
    }

    void advance_generated(std::string_view emitted) {
        for (char value : emitted) {
            if (value == '\n') {
                ++end_state_.generated_line;
                end_state_.generated_column = 0;
            } else {
                ++end_state_.generated_column;
            }
        }
    }

    [[nodiscard]] const auto& sources() const noexcept { return sources_; }
    [[nodiscard]] const auto& segments() const noexcept { return segments_; }
    [[nodiscard]] const SourceMapState& end_state() const noexcept { return end_state_; }
    [[nodiscard]] bool empty() const noexcept { return segments_.empty(); }
};

}  // namespace mbun::codegen
