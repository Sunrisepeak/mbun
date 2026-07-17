// I/O seam for bake.  Production filesystem/JSC/network implementations are
// deliberately deferred; tests and future integrations inject these callbacks.
export module mbun.runtime_bake.backend;

import std;

export namespace mbun::runtime_bake {

struct ArtifactOutput {
    std::string path;
    std::string bytes;
};

class BakeBackend {
public:
    using SourceReader = std::function<std::expected<std::string, std::string>(std::string_view)>;
    using OutputWriter = std::function<std::expected<void, std::string>(const ArtifactOutput&)>;
    using DiagnosticSink = std::function<void(std::string_view)>;

private:
    SourceReader source_reader_;
    OutputWriter output_writer_;
    DiagnosticSink diagnostic_sink_;

public:
    BakeBackend(SourceReader source_reader, OutputWriter output_writer,
                DiagnosticSink diagnostic_sink = {})
        : source_reader_ { std::move(source_reader) }
        , output_writer_ { std::move(output_writer) }
        , diagnostic_sink_ { std::move(diagnostic_sink) } {}

    [[nodiscard]] std::expected<std::string, std::string> read_source(std::string_view path) const {
        if (!source_reader_) return std::unexpected { "bake source reader is not configured" };
        return source_reader_(path);
    }

    [[nodiscard]] std::expected<void, std::string> write_output(const ArtifactOutput& output) const {
        if (!output_writer_) return std::unexpected { "bake output writer is not configured" };
        return output_writer_(output);
    }

    void report(std::string_view message) const {
        if (diagnostic_sink_) diagnostic_sink_(message);
    }
};

} // namespace mbun::runtime_bake
