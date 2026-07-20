// test_standalone_exe.cpp — mbun.bundler.standalone_exe: the `--compile`
// single-file executable container.
//
// Sources (assertion semantics preserved):
//   test/bundler/compile-process-execargv.test.ts — a compiled executable runs
//     the embedded program and reports an EMPTY process.execArgv unless the build
//     baked flags in with --compile-exec-argv.
//   test/bundler/compile-argv.test.ts > "compile/CompileExecArgvDualBehavior" /
//     "compile/CompileExecArgvNoLeak" — the baked flags round-trip into
//     process.execArgv and never leak into process.argv.
//   test/bundler/bundler_compile_autoload.test.ts > "compile/AutoloadDotenv*" —
//     the autoload switches are part of what the executable carries.
// Those are end-to-end runs; this pins the pure container they rest on so a
// regression is caught here rather than only in the corpus.
//
// Blueprint: bun-ref/src/standalone_graph/StandaloneModuleGraph.rs `inject` /
// `fromExecutable` (payload appended after the host image, found by seeking to
// EOF for a magic trailer).

import std;
import mbun.bundler.standalone_exe;

namespace {

int gChecks{0};
int gFailures{0};

void check(bool condition, std::string_view message) {
    ++gChecks;
    if (!condition) {
        ++gFailures;
        std::println("FAIL: {}", message);
    }
}

template <typename T>
void check_eq(const T& actual, const T& expected, std::string_view message) {
    ++gChecks;
    if (!(actual == expected)) {
        ++gFailures;
        std::println("FAIL: {}\n  expected: {}\n  actual:   {}", message, expected, actual);
    }
}

namespace exe = mbun::bundler::standalone_exe;

// A plain (uncompiled) mbun must be recognised as carrying nothing: unpack() runs
// on every single mbun start, so a false positive would break every command.
void test_plain_image_has_no_program() {
    check(!exe::unpack("").has_value(), "an empty image carries no program");
    check(!exe::unpack("\x7f" "ELF ordinary binary contents").has_value(),
          "an ordinary binary carries no program");
    check_eq(std::string{exe::host_image("plain host bytes")}, std::string{"plain host bytes"},
             "host_image() leaves an image without a trailer untouched");
}

void test_round_trip() {
    exe::Program program{};
    program.code = "console.log(\"PASS\");\n";
    program.entryName = "entry.ts";
    const std::string image{exe::pack("HOST-IMAGE", program)};

    check(image.starts_with("HOST-IMAGE"), "the host image is the prefix of the executable");
    check(image.ends_with(exe::MAGIC), "the executable ends with the magic trailer");

    const auto read{exe::unpack(image)};
    check(read.has_value(), "a packed executable unpacks");
    if (!read) {
        return;
    }
    check_eq(read->code, program.code, "the program code round-trips");
    check_eq(read->entryName, program.entryName, "the entry name round-trips");
    check(read->execArgv.empty(), "execArgv defaults to empty (compile-process-execargv)");
    check(read->autoloadDotenv && read->autoloadBunfig && read->autoloadTsconfig &&
              read->autoloadPackageJson,
          "the autoload switches default to on");
}

void test_exec_argv_round_trip() {
    exe::Program program{};
    program.code = "0";
    program.entryName = "entry.ts";
    program.execArgv = {"--title=CompileExecArgvDualBehavior", "--smol"};
    const auto read{exe::unpack(exe::pack("HOST", program))};
    check(read.has_value(), "an executable with baked exec argv unpacks");
    if (read) {
        check_eq(read->execArgv, program.execArgv, "--compile-exec-argv round-trips");
    }
}

void test_autoload_switches_round_trip() {
    exe::Program program{};
    program.code = "0";
    program.entryName = "entry.ts";
    program.autoloadDotenv = false;
    program.autoloadTsconfig = false;
    const auto read{exe::unpack(exe::pack("HOST", program))};
    check(read.has_value(), "an executable with autoload switches unpacks");
    if (read) {
        check(!read->autoloadDotenv, "--no-compile-autoload-dotenv round-trips");
        check(read->autoloadBunfig, "an untouched switch stays on");
        check(!read->autoloadTsconfig, "--no-compile-autoload-tsconfig round-trips");
        check(read->autoloadPackageJson, "an untouched switch stays on");
    }
}

// Payload bytes are opaque: an entry name or argv value containing the magic, a
// quote or a NUL must not confuse the length-prefixed metadata block.
void test_binary_safe_payload() {
    exe::Program program{};
    program.code = std::string{"const s = \"mbunSTA1\";\0 more"} + "\n";
    program.entryName = std::string{"we\0ird\"name.ts", 14};
    program.execArgv = {std::string{"--a=\0b", 6}, "mbunSTA1"};
    const auto read{exe::unpack(exe::pack("HOST", program))};
    check(read.has_value(), "a payload containing the magic bytes still unpacks");
    if (read) {
        check_eq(read->code, program.code, "NUL-bearing code round-trips");
        check_eq(read->entryName, program.entryName, "a quoted/NUL entry name round-trips");
        check_eq(read->execArgv, program.execArgv, "NUL-bearing argv round-trips");
    }
}

// Recompiling from an already-compiled mbun must reuse the base image, not nest
// payloads: otherwise every `--compile` run would grow the binary.
void test_recompile_strips_the_previous_payload() {
    exe::Program first{};
    first.code = "first";
    first.entryName = "a.ts";
    const std::string once{exe::pack("HOST-IMAGE", first)};

    exe::Program second{};
    second.code = "second";
    second.entryName = "b.ts";
    const std::string twice{exe::pack(once, second)};

    check_eq(std::string{exe::host_image(once)}, std::string{"HOST-IMAGE"},
             "host_image() strips an embedded payload");
    check_eq(twice.size(), exe::pack("HOST-IMAGE", second).size(),
             "recompiling does not nest payloads");
    const auto read{exe::unpack(twice)};
    check(read.has_value(), "the recompiled executable unpacks");
    if (read) {
        check_eq(read->code, std::string{"second"}, "the newest program wins");
    }
}

// A truncated or hand-edited image must be rejected, never read out of bounds.
void test_corrupt_images_are_rejected() {
    exe::Program program{};
    program.code = "console.log(1)";
    program.entryName = "entry.ts";
    const std::string image{exe::pack("HOST-IMAGE", program)};

    // Truncating the front makes the recorded sizes exceed what is left.
    check(!exe::unpack(image.substr(image.size() - exe::TRAILER_SIZE)).has_value(),
          "a trailer with no room for its payload is rejected");
    // A trailer claiming an absurd program size must not be trusted.
    std::string forged{image};
    for (std::size_t i{0}; i < 8; ++i) {
        forged[forged.size() - exe::TRAILER_SIZE + i] = static_cast<char>(0xff);
    }
    check(!exe::unpack(forged).has_value(), "an out-of-range program size is rejected");
    // Damaging the last magic byte (the version) rejects a foreign layout.
    std::string wrongVersion{image};
    wrongVersion.back() = '0';
    check(!exe::unpack(wrongVersion).has_value(), "a different container version is rejected");
}

// Startup reads the trailer alone (the mbun image is hundreds of megabytes), so
// the split read_trailer/read_payload path must agree with whole-image unpack().
void test_split_read_matches_whole_image() {
    exe::Program program{};
    program.code = "console.log(1)";
    program.entryName = "entry.ts";
    program.execArgv = {"--smol"};
    const std::string image{exe::pack("HOST-IMAGE", program)};

    check(!exe::read_trailer("too short").has_value(), "a wrong-sized trailer is rejected");
    check(!exe::read_trailer(std::string(exe::TRAILER_SIZE, 'x')).has_value(),
          "bytes without the magic are not a trailer");

    const auto sizes{exe::read_trailer(std::string_view{image}.substr(image.size() - exe::TRAILER_SIZE))};
    check(sizes.has_value(), "the trailer of a packed executable is readable on its own");
    if (!sizes) {
        return;
    }
    const std::size_t payloadSize{static_cast<std::size_t>(sizes->programSize + sizes->metadataSize)};
    const auto read{exe::read_payload(
        std::string_view{image}.substr(image.size() - exe::TRAILER_SIZE - payloadSize, payloadSize),
        *sizes)};
    check(read.has_value(), "the payload named by the trailer decodes on its own");
    if (read) {
        check_eq(read->code, program.code, "the split read yields the same code");
        check_eq(read->execArgv, program.execArgv, "the split read yields the same exec argv");
    }
}

}  // namespace

int main() {
    test_split_read_matches_whole_image();
    test_plain_image_has_no_program();
    test_round_trip();
    test_exec_argv_round_trip();
    test_autoload_switches_round_trip();
    test_binary_safe_payload();
    test_recompile_strips_the_previous_payload();
    test_corrupt_images_are_rejected();

    if (gFailures != 0) {
        std::println("bundler.standalone_exe: {}/{} checks failed", gFailures, gChecks);
        return 1;
    }
    std::println("bundler.standalone_exe: {} checks passed", gChecks);
    return 0;
}
