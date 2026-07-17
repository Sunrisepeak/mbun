import std;
import mbun.analytics;

namespace {
int checks { 0 };
int failures { 0 };

void expect(bool value, std::string_view name) {
    ++checks;
    if (!value) {
        ++failures;
        std::println("FAIL {}", name);
    }
}

void test_schema_reader() {
    const std::uint32_t value { 0x01020304 };
    const auto* begin { reinterpret_cast<const std::byte*>(&value) };
    mbun::analytics::Reader reader { std::span { begin, sizeof(value) } };
    auto decoded { reader.read_native<std::uint32_t>() };
    expect(decoded.has_value() && *decoded == value, "reader.native");
    expect(reader.remaining() == 0, "reader.remaining");
    expect(!reader.read_byte().has_value(), "reader.eof");
}

void test_provider_backend_seam() {
    mbun::analytics::RecordingBackend backend;
    mbun::analytics::Provider provider { backend };
    provider.emit(mbun::analytics::Event::make(mbun::analytics::EventKind::bundle_start));
    expect(backend.events().empty(), "provider.disabled-by-default");
    provider.set_enabled(mbun::analytics::TriState::yes);
    provider.emit(mbun::analytics::Event::make(mbun::analytics::EventKind::bundle_success));
    expect(backend.events().size() == 1, "provider.submit");
    expect(backend.events().front().header.kind == mbun::analytics::EventKind::bundle_success,
           "provider.event-kind");
}

void test_schema_values() {
    const mbun::analytics::Platform platform {
        .os = mbun::analytics::OperatingSystem::linux,
        .arch = mbun::analytics::Architecture::x64,
        .version = "6.12",
    };
    expect(platform.os == mbun::analytics::OperatingSystem::linux, "platform.os");
    expect(platform.version == "6.12", "platform.version");
}
}

int main() {
    test_schema_reader();
    test_provider_backend_seam();
    test_schema_values();
    std::println("analytics checks={} failures={}", checks, failures);
    return failures == 0 ? 0 : 1;
}
