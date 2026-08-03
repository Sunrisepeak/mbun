// test_pw_corpus.cpp — MEASUREMENT harness for the printer-wiring probe.
//
// Inert by default: with PW_CORPUS_LIST unset it prints one line and passes, so
// it costs the normal `mcpp test` run nothing.
//
// Deliberately mode-AGNOSTIC: it never names TranspileOptions::printer, so this
// exact source compiles against pristine HEAD *and* against the wired tree. The
// mode is chosen by the MBUN_TRANSPILE_PRINTER env var that transpile() itself
// reads. That is what makes the flag-off proof airtight — the same harness
// source produces the baseline and the flag-off run, so any hash difference is
// the wiring's fault and nothing else's.
//
// Per-file TSV: <ok|ERR> <tab> <fnv1a-of-output> <tab> <output-size> <tab> <path>
//
// FNV-1a, never std::hash (implementation-defined; will not survive a toolchain
// change).
import std;
import mbun.js_parser;

namespace {

std::uint64_t fnv1a(std::string_view s) {
    std::uint64_t h { 1469598103934665603ULL };
    for (const unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

std::string read_file(const std::string& path) {
    std::ifstream in { path, std::ios::binary };
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

int main() {
    const char* list = std::getenv("PW_CORPUS_LIST");
    if (list == nullptr) {
        std::println("test_pw_corpus: inert (PW_CORPUS_LIST unset)");
        return 0;
    }
    // Optional: dump actual output instead of hashing.
    const char* dump = std::getenv("PW_DUMP");
    // Optional: dump the failure REASON instead of hashing, so an ERR population
    // can be attributed by cause rather than just counted. Reads only
    // `TranspileResult::error`, which predates the printer wiring — so this file
    // still compiles unchanged against pristine HEAD and the mode-agnostic
    // property described above is preserved.
    const char* errs = std::getenv("PW_ERR");

    std::ifstream in { list };
    std::string path;
    while (std::getline(in, path)) {
        if (path.empty()) {
            continue;
        }
        const std::string src { read_file(path) };

        mbun::js_parser::TranspileOptions o;
        o.jsx = path.ends_with(".tsx") || path.ends_with(".jsx");

        mbun::js_parser::TranspileResult r;
        bool threw { false };
        try {
            r = mbun::js_parser::transpile(src, o);
        } catch (...) {
            threw = true;
        }

        if (dump != nullptr) {
            std::println("----- {} -----", path);
            std::println("{}", threw ? std::string { "<threw>" } : r.code);
            continue;
        }
        if (errs != nullptr) {
            std::println("{}\t{}\t{}", (threw || !r.ok) ? "ERR" : "ok",
                         threw ? std::string { "<threw>" } : r.error, path);
            continue;
        }
        std::println("{}\t{:016x}\t{}\t{}", (threw || !r.ok) ? "ERR" : "ok",
                     threw ? 0 : fnv1a(r.code), threw ? 0 : r.code.size(), path);
    }
    return 0;
}
