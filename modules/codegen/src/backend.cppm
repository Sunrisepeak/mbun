// Backend seam: code emission is injected so linker/JSC/disk concerns stay out
// of the pure codegen data model.
export module mbun.codegen.backend;

import std;
import mbun.codegen.output_buffer;
import mbun.codegen.source_map;
import mbun.codegen.symbol;

export namespace mbun::codegen {

struct CodegenArtifact {
    std::string code;
    SourceMapState source_map_end;
};

class Backend {
public:
    virtual ~Backend() = default;
    virtual void emit(std::string_view text) = 0;
    virtual void emit_symbol(SymbolId symbol, std::string_view printedName) = 0;
    virtual void emit_mapping(SourceMapState state) = 0;
    [[nodiscard]] virtual CodegenArtifact finish() = 0;
};

class BufferBackend final : public Backend {
private:
    OutputBuffer output_;
    SourceMapBuilder source_map_;
    const SymbolTable* symbols_ { nullptr };

public:
    explicit BufferBackend(const SymbolTable* symbols = nullptr, std::size_t reserve = 0)
        : output_(reserve), symbols_(symbols) {}

    void emit(std::string_view text) override {
        output_.append(text);
        source_map_.advance_generated(text);
    }

    void emit_symbol(SymbolId symbol, std::string_view printedName) override {
        if (symbols_ == nullptr || symbols_->get(symbol) != nullptr) {
            emit(printedName);
        }
    }

    void emit_mapping(SourceMapState state) override { source_map_.add_mapping(state); }

    [[nodiscard]] CodegenArtifact finish() override {
        return CodegenArtifact { std::move(output_).take(), source_map_.end_state() };
    }
};

}  // namespace mbun::codegen
