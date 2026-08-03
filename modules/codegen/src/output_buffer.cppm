// Output buffer seam inspired by bun's js_printer BufferWriter.
// ref: bun-zig-src/src/js_printer/js_printer.zig BufferWriter
export module mbun.codegen.output_buffer;

import std;

export namespace mbun::codegen {

class OutputBuffer {
private:
    std::string bytes_;
    std::size_t line_ { 0 };
    std::size_t column_ { 0 };

public:
    explicit OutputBuffer(std::size_t reserve = 0) {
        bytes_.reserve(reserve);
    }

    void append(std::string_view text) {
        bytes_.append(text);
        auto newline { text.rfind('\n') };
        if (newline == std::string_view::npos) {
            column_ += text.size();
            return;
        }
        line_ += static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n'));
        column_ = text.size() - newline - 1;
    }

    void append_char(char value) {
        bytes_.push_back(value);
        if (value == '\n') {
            ++line_;
            column_ = 0;
        } else {
            ++column_;
        }
    }

    [[nodiscard]] std::string_view view() const noexcept { return bytes_; }
    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }
    [[nodiscard]] std::size_t line() const noexcept { return line_; }
    [[nodiscard]] std::size_t column() const noexcept { return column_; }

    std::string take() && { return std::move(bytes_); }
};

}  // namespace mbun::codegen
