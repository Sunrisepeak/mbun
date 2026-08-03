// Shared renderer sink. Mirrors bun md/output.rs's sticky OOM contract.
export module mbun.md.output;

import std;

export namespace mbun::md {

class OutputBuffer {
public:
    void write(std::string_view data) { if (oom_) return; try { bytes_.append(data); } catch (const std::bad_alloc&) { oom_ = true; } }
    void write_byte(char value) { if (oom_) return; try { bytes_.push_back(value); } catch (const std::bad_alloc&) { oom_ = true; } }
    [[nodiscard]] bool oom() const { return oom_; }
    [[nodiscard]] std::string take() { return std::exchange(bytes_, {}); }

private:
    std::string bytes_;
    bool oom_{false};
};

} // namespace mbun::md
