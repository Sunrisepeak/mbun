import std;
import mbun.patch_jsc;

namespace {
int checks{};
int failures{};

void check(bool condition, std::string_view message) {
    ++checks;
    if (!condition) {
        ++failures;
        std::println("FAIL: {}", message);
    }
}

constexpr std::string_view PATCH =
    "diff --git a/a.txt b/a.txt\n"
    "--- a/a.txt\n"
    "+++ b/a.txt\n"
    "@@ -1 +1 @@\n"
    "-old\n"
    "+new\n";

constexpr std::string_view INVALID_PATCH =
    "diff --git a/a.txt b/a.txt\n"
    "--- a/a.txt\n"
    "+++ b/a.txt\n"
    "@@ -1,2 +1 @@\n"
    "-old\n"
    "+new\n";

constexpr std::string_view ESCAPED_PATCH =
    "diff --git a/a.txt b/a.txt\n"
    "--- a/a.txt\n"
    "+++ b/a.txt\n"
    "@@ -1 +1 @@\n"
    "-quote\"slash\\\n"
    "+tab\tline\n";

template <typename ResultType>
ResultType successful_apply_result() {
    return ResultType { std::in_place };
}

std::string without_capacities(std::string_view json) {
    constexpr std::string_view PREFIX = ",\"capacity\":";
    std::string normalized;
    normalized.reserve(json.size());
    for (std::size_t cursor { 0 }; cursor < json.size();) {
        if (json.substr(cursor).starts_with(PREFIX)) {
            cursor += PREFIX.size();
            while (cursor < json.size()
                   && json[cursor] >= '0' && json[cursor] <= '9') {
                ++cursor;
            }
            continue;
        }
        normalized.push_back(json[cursor++]);
    }
    return normalized;
}

void test_descriptors_and_parse() {
    mbun::patch_jsc::Binding binding;
    auto descriptors { binding.descriptors() };
    check(descriptors.size() == 3, "three patch testing bindings are registered");
    check(descriptors[2].name == "makeDiff" && descriptors[2].arity == 2,
          "makeDiff descriptor matches Bun testing API");
    auto result { binding.parse(PATCH) };
    check(result.has_value(), "parse delegates to mbun.patch");
    check((std::same_as<decltype(result), mbun::patch_jsc::Result<std::string>>),
          "parse exposes Bun's JSON string result contract");
    constexpr std::string_view EXPECTED =
        "{\"parts\":{\"items\":[{\"file_patch\":{\"path\":\"a.txt\",\"hunks\":{\"items\":["
        "{\"header\":{\"original\":{\"start\":1,\"len\":1},\"patched\":{\"start\":1,\"len\":1}},"
        "\"parts\":{\"items\":[{\"type\":\"deletion\",\"lines\":{\"items\":[\"old\"]},"
        "\"no_newline_at_end_of_file\":false},{\"type\":\"insertion\",\"lines\":{\"items\":[\"new\"]},"
        "\"no_newline_at_end_of_file\":false}]}}]},\"before_hash\":null,\"after_hash\":null}}]}}";
    check(result && without_capacities(*result) == EXPECTED,
          "parse returns the complete PatchFile JSON shape");
    auto empty { binding.parse("not a patch") };
    check(empty && *empty == "{\"parts\":{\"items\":[],\"capacity\":0}}",
          "non-diff input serializes Bun's empty PatchFile");
    auto escaped { binding.parse(ESCAPED_PATCH) };
    check(escaped && escaped->contains("quote\\\"slash\\\\")
              && escaped->contains("tab\\tline"),
          "parse JSON-escapes patch strings");
    auto invalid { binding.parse(INVALID_PATCH) };
    check(!invalid && invalid.error().kind == mbun::patch_jsc::ErrorKind::Parse,
          "invalid patch reports a parse error");
}

void test_injected_backend() {
    auto backend { mbun::patch_jsc::deferred_backend() };
    mbun::patch_jsc::Binding binding;
    auto deferred { binding.apply(PATCH, ".", backend) };
    check(!deferred && deferred.error().kind == mbun::patch_jsc::ErrorKind::Unavailable,
          "default apply backend is explicitly deferred");
    using ApplyResult = mbun::patch_jsc::NativeBackend::Apply::result_type;
    backend.apply = [](std::string_view, std::string_view)
        -> ApplyResult {
        return successful_apply_result<ApplyResult>();
    };
    auto applied { binding.apply(PATCH, ".", backend) };
    check((std::same_as<decltype(applied), mbun::patch_jsc::Result<bool>>),
          "apply exposes Bun's boolean result contract");
    check(applied && *applied,
          "successful apply returns true");
    backend.apply = [](std::string_view, std::string_view) -> ApplyResult {
        return std::unexpected(mbun::patch_jsc::Error {
            .kind = mbun::patch_jsc::ErrorKind::Apply,
            .operation = "apply",
            .message = "write failed",
        });
    };
    auto failed { binding.apply(PATCH, ".", backend) };
    check(!failed && failed.error().kind == mbun::patch_jsc::ErrorKind::Apply,
          "apply preserves backend errors");

    backend.make_diff = [](std::string_view, std::string_view)
        -> mbun::patch_jsc::Result<std::string> {
        return "diff --git a/a.txt b/a.txt\n";
    };
    auto diff { binding.make_diff("old", "new", backend) };
    check(diff && *diff == "diff --git a/a.txt b/a.txt\n",
          "makeDiff returns the backend string");
}
}

int main() {
    test_descriptors_and_parse();
    test_injected_backend();
    std::println("test_patch_jsc: {} checks, {} failures", checks, failures);
    return failures == 0 ? 0 : 1;
}
