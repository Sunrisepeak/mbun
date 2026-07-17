// Focused runtime regression coverage for Task 3 Fix Domain A.
// Exercises the real JS APIs directly so Bun.which is not hidden behind the
// upstream which.test.ts Bun shell-tag (`$.nothrow`) gate.
#include <cstdlib>

import std;
import mbun.jsc.runtime;

namespace {

int gFailed{0};

void expect_num(std::string_view source, double expected, std::string_view what) {
    auto result{mbun::jsc::runtime::eval_number(source)};
    if (!result || *result != expected) {
        ++gFailed;
        std::println("  FAIL: {} (got {})", what,
                     result ? std::to_string(*result) : std::string{"evaluation error"});
    }
}

class TempRuntimeFixture {
public:
    TempRuntimeFixture() : originalCwd_{std::filesystem::current_path()} {
        if (const char* path{std::getenv("PATH")}) originalNativePath_ = path;
#if defined(_WIN32)
        _putenv_s("PATH", "");
#else
        ::unsetenv("PATH");
#endif

        const auto stamp{std::chrono::steady_clock::now().time_since_epoch().count()};
        root_ = std::filesystem::temp_directory_path() /
                std::format("mbun-fix-domain-a-{}", stamp);
        binDir_ = root_ / "bin";
        std::filesystem::create_directories(binDir_);

#if defined(_WIN32)
        executableName_ = "mbun-path-fixture.cmd";
        std::ofstream{binDir_ / executableName_} << "@exit /b 0\r\n";
#else
        executableName_ = "mbun-path-fixture";
        std::ofstream{binDir_ / executableName_} << "#!/bin/sh\nexit 0\n";
        std::filesystem::permissions(
            binDir_ / executableName_,
            std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec |
                std::filesystem::perms::others_exec,
            std::filesystem::perm_options::add);
#endif

        std::ofstream{root_ / ".env"} << "PATH=" << binDir_.string() << "\n";
        std::filesystem::current_path(root_);
    }

    ~TempRuntimeFixture() {
        std::error_code ec;
        std::filesystem::current_path(originalCwd_, ec);
        std::filesystem::remove_all(root_, ec);
#if defined(_WIN32)
        _putenv_s("PATH", originalNativePath_.c_str());
#else
        if (originalNativePath_.empty()) {
            ::unsetenv("PATH");
        } else {
            ::setenv("PATH", originalNativePath_.c_str(), 1);
        }
#endif
    }

    const std::string& executable_name() const { return executableName_; }

    void expose_fixture_through_native_path() const {
        const std::string path{binDir_.string()};
#if defined(_WIN32)
        _putenv_s("PATH", path.c_str());
#else
        ::setenv("PATH", path.c_str(), 1);
#endif
    }

private:
    std::filesystem::path originalCwd_;
    std::filesystem::path root_;
    std::filesystem::path binDir_;
    std::string executableName_;
    std::string originalNativePath_;
};

}  // namespace

int main() {
    TempRuntimeFixture fixture;
    mbun::jsc::runtime::apply_dotenv(/*isTest=*/false);

    // A bare ArrayBuffer is byte input, exactly like its Uint8Array view. It
    // must never fall through JS string coercion to "[object ArrayBuffer]".
    constexpr std::string_view expectedSha256{
        "3d1f57c984978ef98a18378c8166c1cb8ede02c03eeb6aee7e2f121dfeee3e56"};
    expect_num(
        std::format(
            "(()=>{{const b=new Uint8Array([0,1,2,255]).buffer;"
            "return Bun.CryptoHasher.hash('sha256',b,'hex')==='{}'&&"
            "new Bun.CryptoHasher('sha256').update(b).digest('hex')==='{}'?1:0}})()",
            expectedSha256, expectedSha256),
        1.0, "CryptoHasher hashes ArrayBuffer backing bytes");
    expect_num(
        std::format("Bun.CryptoHasher.hash('sha256',new Uint8Array([0,1,2,255]),'hex')==='{}'?1:0",
                    expectedSha256),
        1.0, "CryptoHasher preserves ArrayBufferView byte semantics");
    expect_num(
        std::format(
            "(()=>{{const a=new Uint8Array([9,0,1,2,255,8]);"
            "const v=new Uint8Array(a.buffer,1,4);"
            "return Bun.CryptoHasher.hash('sha256',v,'hex')==='{}'?1:0}})()",
            expectedSha256),
        1.0, "CryptoHasher hashes only a subview's offset and length");
    expect_num(
        "Bun.CryptoHasher.hash('sha256','abc','hex')==="
        "'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad'?1:0",
        1.0, "CryptoHasher preserves string UTF-8 semantics");

    const std::string name{fixture.executable_name()};
    expect_num(std::format("globalThis.__domainAPath=process.env.PATH;Bun.which('{}')?1:0", name),
               1.0, "Bun.which sees PATH loaded from .env");
    expect_num(std::format("process.env.PATH=__domainAPath;Bun.which('{}')?1:0", name), 1.0,
               "Bun.which sees runtime process.env.PATH mutation");
    expect_num(std::format("process.env.PATH='';Bun.which('{}')===null?1:0", name), 1.0,
               "Bun.which sees an empty runtime PATH");
    expect_num(
        std::format("Bun.which('{}',{{PATH:__domainAPath}})?1:0", name), 1.0,
        "Bun.which explicit options.PATH overrides runtime process.env.PATH");
    fixture.expose_fixture_through_native_path();
    expect_num(std::format("delete process.env.PATH;Bun.which('{}')===null?1:0", name), 1.0,
               "Bun.which does not native-fallback when process.env lacks PATH");

    if (gFailed != 0) {
        std::println("test_runtime_crypto_which: {} failed", gFailed);
        return 1;
    }
    std::println("test_runtime_crypto_which: ok");
    return 0;
}
