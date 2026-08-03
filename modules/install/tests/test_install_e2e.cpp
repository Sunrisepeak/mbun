// test_install_e2e.cpp — end-to-end registry install pipeline tests.
//
// Spins up an in-process 127.0.0.1 HTTP registry (no external network) that
// serves generated packuments plus real .tgz tarballs built in-process (ustar
// writer + mbun.core.compress gzip), with genuine sha512 dist.integrity
// values, then drives install_project() end to end:
//
//   - semver range selection (^1.0.0 picks 1.1.0, not 2.0.0),
//   - node_modules layout + extracted file contents,
//   - .bin symlink creation from the packument bin field,
//   - transitive dependency installation (foo → bar),
//   - scoped package layout (node_modules/@scope/pkg),
//   - dist-tag resolution ("beta"),
//   - remote tarball dependency (bin/version from extracted package.json),
//   - integrity tamper → IntegrityCheckFailed with bun's wording,
//   - manifest 404 → ManifestFetchFailed,
//   - file: dependency regression + --frozen-lockfile behavior unchanged,
//   - independent packages actually download concurrently (the install runs on
//     async_http::Engine, one epoll loop multiplexing every request).
//
// Reference semantics: .mbun/bun-ref/src/install/PackageManager/
// {PackageManagerEnqueue,runTasks}.rs + extract_tarball.rs.

#if !defined(_WIN32)
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

import std;
import mbun.core.compress;
import mbun.install.command;
import mbun.install.integrity;

namespace command = mbun::install::command;

namespace {

int gChecks{0};
int gFailures{0};

void check_true(bool cond, std::string_view what) {
    ++gChecks;
    if (!cond) {
        ++gFailures;
        std::println("  FAIL {}", what);
    }
}

void check_contains(std::string_view haystack, std::string_view needle, std::string_view what) {
    ++gChecks;
    if (haystack.find(needle) == std::string_view::npos) {
        ++gFailures;
        std::println("  FAIL {}: \"{}\" not in \"{}\"", what, needle, haystack);
    }
}

#if !defined(_WIN32)

// ── in-process tgz builder (ustar + gzip) ───────────────────────────────────

struct TarFile {
    std::string name;  // e.g. "package/package.json"
    std::string data;
    unsigned mode{0644};
};

void append_tar_entry(std::vector<std::uint8_t>& tar, const TarFile& file) {
    std::array<std::uint8_t, 512> header{};
    auto put{[&header](std::size_t offset, std::string_view text) {
        std::copy(text.begin(), text.end(), header.begin() + offset);
    }};
    put(0, file.name);
    put(100, std::format("{:07o}", file.mode));
    put(108, "0000000");
    put(116, "0000000");
    put(124, std::format("{:011o}", file.data.size()));
    put(136, "00000000000");
    header[156] = '0';  // regular file
    put(257, "ustar");
    header[262] = 0;
    put(263, "00");
    // checksum: bytes summed with the chksum field as spaces.
    std::fill(header.begin() + 148, header.begin() + 156, std::uint8_t{' '});
    unsigned sum{0};
    for (std::uint8_t b : header) {
        sum += b;
    }
    put(148, std::format("{:06o}", sum));
    header[154] = 0;
    header[155] = ' ';

    tar.insert(tar.end(), header.begin(), header.end());
    tar.insert(tar.end(), file.data.begin(), file.data.end());
    tar.insert(tar.end(), (512 - file.data.size() % 512) % 512, std::uint8_t{0});
}

std::vector<std::uint8_t> make_tgz(std::span<const TarFile> files) {
    std::vector<std::uint8_t> tar;
    for (const TarFile& file : files) {
        append_tar_entry(tar, file);
    }
    tar.insert(tar.end(), 1024, std::uint8_t{0});  // end-of-archive
    return mbun::core::compress::gzip_compress({tar.data(), tar.size()});
}

std::string sri_for(std::span<const std::uint8_t> bytes) {
    return mbun::install::Integrity::for_bytes(bytes).to_string();
}

// ── in-process HTTP registry (path → canned response) ───────────────────────

std::string http_response(int status, std::string_view reason, std::string_view body,
                          std::string_view contentType) {
    return std::format(
        "HTTP/1.1 {} {}\r\nContent-Type: {}\r\nContent-Length: {}\r\n\r\n{}",
        status, reason, contentType, body.size(), body);
}

// Handles every accepted connection on its own thread, so the *server* never
// serializes the client. Whether requests overlap is then a property of the
// install pipeline alone — which is the whole point of the concurrency test
// below. `delay` holds each response back so overlap is observable on the
// wall clock: N requests cost ~delay when multiplexed and ~N×delay when not.
class RegistryServer {
private:
    int listenFd_{-1};
    std::uint16_t port_{0};
    std::map<std::string, std::string, std::less<>> responses_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::vector<std::thread> workers_;
    std::mutex workersMutex_;
    std::chrono::milliseconds delay_{0};
    std::atomic<int> accepted_{0};
    std::atomic<int> inFlight_{0};
    std::atomic<int> peakInFlight_{0};

public:
    RegistryServer() {
        listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int one{1};
        (void)::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        ::sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listenFd_, reinterpret_cast<::sockaddr*>(&addr), sizeof addr) != 0 ||
            ::listen(listenFd_, 16) != 0) {
            ::close(listenFd_);
            listenFd_ = -1;
            return;
        }
        ::socklen_t len{sizeof addr};
        (void)::getsockname(listenFd_, reinterpret_cast<::sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        ::timeval tv{0, 200 * 1000};  // accept poll interval
        (void)::setsockopt(listenFd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    }

    RegistryServer(const RegistryServer&) = delete;
    RegistryServer& operator=(const RegistryServer&) = delete;

    ~RegistryServer() {
        stop_ = true;
        if (thread_.joinable()) {
            thread_.join();
        }
        // Join workers only after the acceptor is done touching the list.
        std::lock_guard<std::mutex> lock{workersMutex_};
        for (std::thread& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        if (listenFd_ >= 0) {
            ::close(listenFd_);
        }
    }

    bool ok() const { return listenFd_ >= 0; }
    std::uint16_t port() const { return port_; }
    int accepted() const { return accepted_.load(); }
    // The high-water mark of concurrently-open connections: 1 means the client
    // never overlapped anything, N means it had N requests in flight at once.
    int peak_in_flight() const { return peakInFlight_.load(); }
    void set_delay(std::chrono::milliseconds delay) { delay_ = delay; }

    std::string url(std::string_view path) const {
        return "http://127.0.0.1:" + std::to_string(port_) + std::string{path};
    }

    void add(std::string path, std::string rawResponse) {
        responses_.insert_or_assign(std::move(path), std::move(rawResponse));
    }

    void start() {
        thread_ = std::thread{[this] { serve_(); }};
    }

private:
    void serve_() {
        while (!stop_) {
            int fd{::accept(listenFd_, nullptr, nullptr)};
            if (fd < 0) {
                continue;  // timeout — re-check the stop flag
            }
            ++accepted_;
            std::lock_guard<std::mutex> lock{workersMutex_};
            workers_.emplace_back([this, fd] { handle_(fd); });
        }
    }

    void handle_(int fd) {
        const int now{inFlight_.fetch_add(1) + 1};
        for (int peak{peakInFlight_.load()}; now > peak;) {
            if (peakInFlight_.compare_exchange_weak(peak, now)) {
                break;
            }
        }
        std::string request;
        char buf[8192];
        while (request.find("\r\n\r\n") == std::string::npos) {
            ::ssize_t n{::recv(fd, buf, sizeof buf, 0)};
            if (n <= 0) {
                break;
            }
            request.append(buf, static_cast<std::size_t>(n));
        }
        std::string_view path{};
        if (std::size_t sp1{request.find(' ')}; sp1 != std::string::npos) {
            std::size_t sp2{request.find(' ', sp1 + 1)};
            if (sp2 != std::string::npos) {
                path = std::string_view{request}.substr(sp1 + 1, sp2 - sp1 - 1);
            }
        }
        // Hold the response back, but stay responsive to teardown so a stopped
        // server does not have to wait out a full delay.
        for (std::chrono::milliseconds waited{0}; waited < delay_ && !stop_.load();
             waited += std::chrono::milliseconds{5}) {
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        static const std::string NOT_FOUND{
            http_response(404, "Not Found", R"({"error":"Not found"})", "application/json")};
        auto it{responses_.find(path)};
        const std::string& response{it == responses_.end() ? NOT_FOUND : it->second};
        std::size_t sent{0};
        while (sent < response.size()) {
            ::ssize_t n{::send(fd, response.data() + sent, response.size() - sent, 0)};
            if (n <= 0) {
                break;
            }
            sent += static_cast<std::size_t>(n);
        }
        ::close(fd);
        --inFlight_;
    }
};

// ── fixtures ────────────────────────────────────────────────────────────────

std::filesystem::path make_project(std::string_view name, std::string_view packageJson) {
    static std::size_t counter{0};
    std::filesystem::path dir{std::filesystem::temp_directory_path() /
                              std::format("mbun-e2e-{}-{}-{}", ::getpid(), name, counter++)};
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    std::ofstream{dir / "package.json"} << packageJson;
    return dir;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

std::string version_json(std::string_view name, std::string_view version,
                         std::string_view tarballUrl, std::string_view integrity,
                         std::string_view extraFields) {
    return std::format(
        R"("{}":{{"name":"{}","version":"{}"{}{},"dist":{{"tarball":"{}","integrity":"{}"}}}})",
        version, name, version, extraFields.empty() ? "" : ",", extraFields, tarballUrl,
        integrity);
}

command::InstallOptions options_for(const RegistryServer& server) {
    command::InstallOptions options{};
    options.registry = server.url("/");
    return options;
}

// Populate the shared registry: foo (3 versions, bin + transitive dep on bar),
// bar, @scope/pkg, tagpkg (dist-tag beta), evil (tampered integrity), tarpkg
// (bare remote tarball, no packument).
struct Fixtures {
    std::string fooIndexJs{"module.exports = 'foo@1.1.0';\n"};
    std::string fooCliJs{"#!/usr/bin/env node\nconsole.log('foo-cli');\n"};

    void install(RegistryServer& server) {
        auto add_package{[&server](std::string_view name, std::string_view pathName,
                                   std::span<const std::string> versionEntries,
                                   std::string_view distTags) {
            std::string versions;
            for (const std::string& entry : versionEntries) {
                if (!versions.empty()) {
                    versions.push_back(',');
                }
                versions.append(entry);
            }
            std::string packument{std::format(
                R"({{"name":"{}","dist-tags":{{{}}},"versions":{{{}}}}})", name, distTags,
                versions)};
            server.add(std::string{pathName},
                       http_response(200, "OK", packument, "application/json"));
        }};
        auto add_tarball{[&server](std::string_view path, std::span<const std::uint8_t> tgz) {
            std::string body{reinterpret_cast<const char*>(tgz.data()), tgz.size()};
            server.add(std::string{path},
                       http_response(200, "OK", body, "application/octet-stream"));
        }};

        // foo — 1.0.0 / 1.1.0 / 2.0.0; 1.1.0 has a bin and depends on bar.
        std::vector<TarFile> foo11Files{
            {"package/package.json",
             R"({"name":"foo","version":"1.1.0","bin":{"foo-cli":"./cli.js"},)"
             R"("dependencies":{"bar":"^1.0.0"}})"},
            {"package/index.js", fooIndexJs},
            {"package/cli.js", fooCliJs, 0755},
        };
        std::vector<std::uint8_t> foo11{make_tgz(foo11Files)};
        std::vector<TarFile> foo20Files{
            {"package/package.json", R"({"name":"foo","version":"2.0.0"})"},
        };
        std::vector<std::uint8_t> foo20{make_tgz(foo20Files)};
        std::vector<std::string> fooVersions{
            version_json("foo", "1.0.0", server.url("/tarballs/foo-1.0.0.tgz"), sri_for(foo20),
                         ""),
            version_json("foo", "1.1.0", server.url("/tarballs/foo-1.1.0.tgz"), sri_for(foo11),
                         R"("bin":{"foo-cli":"./cli.js"},"dependencies":{"bar":"^1.0.0"})"),
            version_json("foo", "2.0.0", server.url("/tarballs/foo-2.0.0.tgz"), sri_for(foo20),
                         ""),
        };
        add_package("foo", "/foo", fooVersions, R"("latest":"2.0.0")");
        add_tarball("/tarballs/foo-1.1.0.tgz", foo11);
        add_tarball("/tarballs/foo-2.0.0.tgz", foo20);

        // bar — transitive target.
        std::vector<TarFile> barFiles{
            {"package/package.json", R"({"name":"bar","version":"1.0.0"})"},
            {"package/index.js", "module.exports = 'bar';\n"},
        };
        std::vector<std::uint8_t> bar{make_tgz(barFiles)};
        std::vector<std::string> barVersions{
            version_json("bar", "1.0.0", server.url("/tarballs/bar-1.0.0.tgz"), sri_for(bar), ""),
        };
        add_package("bar", "/bar", barVersions, R"("latest":"1.0.0")");
        add_tarball("/tarballs/bar-1.0.0.tgz", bar);

        // peerpkg — a peer edge onto `bar` and nothing else. Nobody else pulls
        // `bar` in, so installing it is the whole point.
        std::vector<TarFile> peerpkgFiles{
            {"package/package.json",
             R"({"name":"peerpkg","version":"1.0.0","peerDependencies":{"bar":"^1.0.0"}})"},
        };
        std::vector<std::uint8_t> peerpkg{make_tgz(peerpkgFiles)};
        std::vector<std::string> peerpkgVersions{
            version_json("peerpkg", "1.0.0", server.url("/tarballs/peerpkg-1.0.0.tgz"),
                         sri_for(peerpkg), R"("peerDependencies":{"bar":"^1.0.0"})"),
        };
        add_package("peerpkg", "/peerpkg", peerpkgVersions, R"("latest":"1.0.0")");
        add_tarball("/tarballs/peerpkg-1.0.0.tgz", peerpkg);

        // peerfoo — peers on `foo@^2.0.0`. When the root also depends on
        // `foo@^1.0.0` the deferred phase must bind this to the 1.1.0 already
        // resolved rather than pulling 2.0.0 in beside it.
        std::vector<TarFile> peerfooFiles{
            {"package/package.json",
             R"({"name":"peerfoo","version":"1.0.0","peerDependencies":{"foo":"^2.0.0"}})"},
        };
        std::vector<std::uint8_t> peerfoo{make_tgz(peerfooFiles)};
        std::vector<std::string> peerfooVersions{
            version_json("peerfoo", "1.0.0", server.url("/tarballs/peerfoo-1.0.0.tgz"),
                         sri_for(peerfoo), R"("peerDependencies":{"foo":"^2.0.0"})"),
        };
        add_package("peerfoo", "/peerfoo", peerfooVersions, R"("latest":"1.0.0")");
        add_tarball("/tarballs/peerfoo-1.0.0.tgz", peerfoo);

        // peermiss — peers on a name the registry 404s. bun skips every peer it
        // could not resolve without failing the install
        // (PackageManagerResolution.rs:370-374).
        std::vector<TarFile> peermissFiles{
            {"package/package.json",
             R"({"name":"peermiss","version":"1.0.0","peerDependencies":{"ghost":"^1.0.0"}})"},
        };
        std::vector<std::uint8_t> peermiss{make_tgz(peermissFiles)};
        std::vector<std::string> peermissVersions{
            version_json("peermiss", "1.0.0", server.url("/tarballs/peermiss-1.0.0.tgz"),
                         sri_for(peermiss), R"("peerDependencies":{"ghost":"^1.0.0"})"),
        };
        add_package("peermiss", "/peermiss", peermissVersions, R"("latest":"1.0.0")");
        add_tarball("/tarballs/peermiss-1.0.0.tgz", peermiss);

        // selfpeer — peers on `app`, the name the test projects give their root.
        std::vector<TarFile> selfpeerFiles{
            {"package/package.json",
             R"({"name":"selfpeer","version":"1.0.0","peerDependencies":{"app":">= 1.0.0"}})"},
        };
        std::vector<std::uint8_t> selfpeer{make_tgz(selfpeerFiles)};
        std::vector<std::string> selfpeerVersions{
            version_json("selfpeer", "1.0.0", server.url("/tarballs/selfpeer-1.0.0.tgz"),
                         sri_for(selfpeer), R"("peerDependencies":{"app":">= 1.0.0"})"),
        };
        add_package("selfpeer", "/selfpeer", selfpeerVersions, R"("latest":"1.0.0")");
        add_tarball("/tarballs/selfpeer-1.0.0.tgz", selfpeer);

        // @scope/pkg — the manifest is requested with the %2f-encoded name.
        std::vector<TarFile> scopedFiles{
            {"package/package.json", R"({"name":"@scope/pkg","version":"1.0.0"})"},
        };
        std::vector<std::uint8_t> scoped{make_tgz(scopedFiles)};
        std::vector<std::string> scopedVersions{
            version_json("@scope/pkg", "1.0.0", server.url("/tarballs/scope-pkg-1.0.0.tgz"),
                         sri_for(scoped), ""),
        };
        add_package("@scope/pkg", "/@scope%2fpkg", scopedVersions, R"("latest":"1.0.0")");
        add_tarball("/tarballs/scope-pkg-1.0.0.tgz", scoped);

        // tagpkg — "beta" dist-tag points at a prerelease.
        std::vector<TarFile> tagFiles{
            {"package/package.json", R"({"name":"tagpkg","version":"2.0.0-beta.1"})"},
        };
        std::vector<std::uint8_t> tag{make_tgz(tagFiles)};
        std::vector<std::string> tagVersions{
            version_json("tagpkg", "1.0.0", server.url("/tarballs/tagpkg-1.0.0.tgz"),
                         sri_for(tag), ""),
            version_json("tagpkg", "2.0.0-beta.1", server.url("/tarballs/tagpkg-2.0.0-beta.1.tgz"),
                         sri_for(tag), ""),
        };
        add_package("tagpkg", "/tagpkg", tagVersions,
                    R"("latest":"1.0.0","beta":"2.0.0-beta.1")");
        add_tarball("/tarballs/tagpkg-1.0.0.tgz", tag);
        add_tarball("/tarballs/tagpkg-2.0.0-beta.1.tgz", tag);

        // evil — dist.integrity deliberately does NOT match the served bytes.
        std::vector<TarFile> evilFiles{
            {"package/package.json", R"({"name":"evil","version":"1.0.0"})"},
        };
        std::vector<std::uint8_t> evil{make_tgz(evilFiles)};
        std::vector<std::uint8_t> other{0xde, 0xad, 0xbe, 0xef};
        std::vector<std::string> evilVersions{
            version_json("evil", "1.0.0", server.url("/tarballs/evil-1.0.0.tgz"),
                         sri_for(other), ""),
        };
        add_package("evil", "/evil", evilVersions, R"("latest":"1.0.0")");
        add_tarball("/tarballs/evil-1.0.0.tgz", evil);

        // tarpkg — plain remote tarball dependency, no packument at all.
        std::vector<TarFile> tarpkgFiles{
            {"package/package.json",
             R"({"name":"tarpkg","version":"3.2.1","bin":{"tarpkg-cli":"./cli.js"}})"},
            {"package/cli.js", "#!/usr/bin/env node\n", 0755},
        };
        std::vector<std::uint8_t> tarpkg{make_tgz(tarpkgFiles)};
        add_tarball("/extra/tarpkg.tgz", tarpkg);

        // ── the nested-conflict fixture ─────────────────────────────────────
        //
        // A minimal reproduction of the shape real bun 1.4.0 puts on disk for
        // itty-router:
        //
        //     node_modules/ignore                                      -> 5.3.1
        //     node_modules/@typescript-eslint/eslint-plugin/node_modules/ignore
        //                                                              -> 7.0.5
        //
        // (`bun-rust install --ignore-scripts && bun-rust pm ls --all`, with the
        // project's committed bun.lockb in place.) `shared` stands in for
        // `ignore`: the root pins ^2, a dependency needs ^1, and both must exist
        // at once. A flat installer physically cannot produce this — whichever
        // edge resolved second is simply absent — which is what makes it a test
        // of the WIRING and not of the hoister's internal arithmetic.
        std::vector<TarFile> shared1Files{
            {"package/package.json", R"({"name":"shared","version":"1.0.0"})"},
            {"package/index.js", "module.exports = 'shared@1';\n"},
        };
        std::vector<TarFile> shared2Files{
            {"package/package.json", R"({"name":"shared","version":"2.0.0"})"},
            {"package/index.js", "module.exports = 'shared@2';\n"},
        };
        std::vector<std::uint8_t> shared1{make_tgz(shared1Files)};
        std::vector<std::uint8_t> shared2{make_tgz(shared2Files)};
        std::vector<std::string> sharedVersions{
            version_json("shared", "1.0.0", server.url("/tarballs/shared-1.0.0.tgz"),
                         sri_for(shared1), ""),
            version_json("shared", "2.0.0", server.url("/tarballs/shared-2.0.0.tgz"),
                         sri_for(shared2), ""),
        };
        add_package("shared", "/shared", sharedVersions, R"("latest":"2.0.0")");
        add_tarball("/tarballs/shared-1.0.0.tgz", shared1);
        add_tarball("/tarballs/shared-2.0.0.tgz", shared2);

        // needsold — wants the OLD shared, so it cannot use the root's hoisted 2.
        std::vector<TarFile> needsoldFiles{
            {"package/package.json",
             R"({"name":"needsold","version":"1.0.0","dependencies":{"shared":"^1.0.0"}})"},
        };
        std::vector<std::uint8_t> needsold{make_tgz(needsoldFiles)};
        std::vector<std::string> needsoldVersions{
            version_json("needsold", "1.0.0", server.url("/tarballs/needsold-1.0.0.tgz"),
                         sri_for(needsold), R"("dependencies":{"shared":"^1.0.0"})"),
        };
        add_package("needsold", "/needsold", needsoldVersions, R"("latest":"1.0.0")");
        add_tarball("/tarballs/needsold-1.0.0.tgz", needsold);

        // alsoold — a SECOND dependent on shared@^1, and it gets its OWN nested
        // copy. That is not a guess: the oracle shows `ignore@7.0.5` nested
        // twice, under BOTH of its dependents —
        //
        //     node_modules/@typescript-eslint/eslint-plugin/node_modules/ignore -> 7.0.5
        //     node_modules/globby/node_modules/ignore                           -> 7.0.5
        //     node_modules/ignore                                               -> 5.3.1
        //
        // — because `hoist_dependency` walks to the parent tree, finds `ignore`
        // already bound there to a *different* PackageID (5.3.1), and answers
        // DependencyLoop, which lands the package in the dependent's own tree
        // (Tree.rs:1110-1134). One shared nested directory would be a *different*
        // layout from bun's, so this asserts two.
        //
        // Both copies are still ONE PackageID and ONE download: dedupe is keyed
        // on the (name, version) resolution, and the placer links the same store
        // entry into both directories.
        std::vector<TarFile> alsooldFiles{
            {"package/package.json",
             R"({"name":"alsoold","version":"1.0.0","dependencies":{"shared":"1.0.0"}})"},
        };
        std::vector<std::uint8_t> alsoold{make_tgz(alsooldFiles)};
        std::vector<std::string> alsooldVersions{
            version_json("alsoold", "1.0.0", server.url("/tarballs/alsoold-1.0.0.tgz"),
                         sri_for(alsoold), R"("dependencies":{"shared":"1.0.0"})"),
        };
        add_package("alsoold", "/alsoold", alsooldVersions, R"("latest":"1.0.0")");
        add_tarball("/tarballs/alsoold-1.0.0.tgz", alsoold);
    }
};

// ── tests ───────────────────────────────────────────────────────────────────

void test_semver_selection_layout_bins_transitive(RegistryServer& server, const Fixtures& fx) {
    auto project{make_project("semver", R"({"name":"app","dependencies":{"foo":"^1.0.0"}})")};
    auto result{command::install_project(project, options_for(server))};
    check_true(result.has_value(), "e2e: ^1.0.0 install succeeds");
    if (!result) {
        std::println("  (error: {})", result.error().message);
        return;
    }
    check_true(result->installed == 2, "e2e: foo + transitive bar counted");

    // ^1.0.0 must select 1.1.0, not 2.0.0.
    std::string fooJson{read_file(project / "node_modules/foo/package.json")};
    check_contains(fooJson, "\"version\":\"1.1.0\"", "e2e: ^1.0.0 selected 1.1.0");
    check_true(read_file(project / "node_modules/foo/index.js") == fx.fooIndexJs,
               "e2e: extracted index.js content");
    check_true(read_file(project / "node_modules/foo/cli.js") == fx.fooCliJs,
               "e2e: extracted cli.js content");

    // Transitive dependency hoisted to the top-level node_modules.
    std::string barJson{read_file(project / "node_modules/bar/package.json")};
    check_contains(barJson, "\"name\":\"bar\"", "e2e: transitive bar installed");

    // .bin symlink resolves to the package's cli.js.
    const std::filesystem::path binLink{project / "node_modules/.bin/foo-cli"};
    std::error_code ec;
    check_true(std::filesystem::is_symlink(binLink, ec), "e2e: .bin/foo-cli is a symlink");
    check_true(std::filesystem::weakly_canonical(binLink, ec) ==
                   std::filesystem::weakly_canonical(project / "node_modules/foo/cli.js", ec),
               "e2e: .bin/foo-cli resolves to foo/cli.js");
    std::filesystem::remove_all(project);
}

// bun installs peerDependencies by default — `Features::base()` sets
// `peer_dependencies: true` (resolver_hooks.rs:1302) and the root parses with
// `Features::main()`, which inherits it (install_with_manager.rs:1584). A root
// peer nothing else provides is therefore a package that must land on disk.
void test_root_peer_dependency_installed(RegistryServer& server) {
    auto project{make_project("rootpeer", R"({"name":"app","peerDependencies":{"bar":"^1.0.0"}})")};
    auto result{command::install_project(project, options_for(server))};
    check_true(result.has_value(), "e2e: root peer install succeeds");
    if (!result) {
        std::println("  (error: {})", result.error().message);
        std::filesystem::remove_all(project);
        return;
    }
    check_contains(read_file(project / "node_modules/bar/package.json"), "\"name\":\"bar\"",
                   "e2e: root peerDependency installed");
    check_true(result->installed == 1, "e2e: root peer counted once");
    std::filesystem::remove_all(project);
}

// "Duplicate peer & dev dependencies are promoted to whichever appeared first"
// (lockfile/Package.rs:867-884) — the `dependencies` entry wins the name at
// parse time and the peer's own range never resolves. Were it the other way
// round, `^2.0.0` would drag 2.0.0 in over the ^1.0.0 the root asked for. This
// is the root-parse dedupe (`append_peer_dependency_map`); the deferred-phase
// twin is `test_peer_binds_to_existing_resolution`.
void test_root_peer_duplicate_loses_to_dependency(RegistryServer& server) {
    auto project{make_project(
        "rootpeerdup",
        R"({"name":"app","dependencies":{"foo":"^1.0.0"},"peerDependencies":{"foo":"^2.0.0"}})")};
    auto result{command::install_project(project, options_for(server))};
    check_true(result.has_value(), "e2e: duplicated root peer install succeeds");
    if (!result) {
        std::println("  (error: {})", result.error().message);
        std::filesystem::remove_all(project);
        return;
    }
    check_contains(read_file(project / "node_modules/foo/package.json"), "\"version\":\"1.1.0\"",
                   "e2e: dependencies entry beats the duplicate peer range");
    std::filesystem::remove_all(project);
}

// `Features::NPM` keeps `peer_dependencies: true` (resolver_hooks.rs:1340), so a
// registry package's peers resolve like the root's.
void test_transitive_peer_dependency_installed(RegistryServer& server) {
    auto project{make_project("transpeer", R"({"name":"app","dependencies":{"peerpkg":"^1.0.0"}})")};
    auto result{command::install_project(project, options_for(server))};
    check_true(result.has_value(), "e2e: transitive peer install succeeds");
    if (!result) {
        std::println("  (error: {})", result.error().message);
        std::filesystem::remove_all(project);
        return;
    }
    check_contains(read_file(project / "node_modules/bar/package.json"), "\"name\":\"bar\"",
                   "e2e: peer of a registry package installed");
    std::filesystem::remove_all(project);
}

// The deferred phase's reason to exist: peers resolve only after everything else
// has, so an already-resolved package of the same name takes the edge
// (PackageManagerEnqueue.rs:2203-2218). `peerfoo` wants `foo@^2.0.0` and the
// root wants `foo@^1.0.0`; bun binds the peer to the root's 1.1.0 and warns
// rather than installing 2.0.0 — the existing resolution wins the name.
void test_peer_binds_to_existing_resolution(RegistryServer& server) {
    auto project{make_project(
        "peerbind", R"({"name":"app","dependencies":{"peerfoo":"^1.0.0","foo":"^1.0.0"}})")};
    auto result{command::install_project(project, options_for(server))};
    check_true(result.has_value(), "e2e: peer-vs-dependency install succeeds");
    if (!result) {
        std::println("  (error: {})", result.error().message);
        std::filesystem::remove_all(project);
        return;
    }
    check_contains(read_file(project / "node_modules/foo/package.json"), "\"version\":\"1.1.0\"",
                   "e2e: peer bound to the existing foo rather than pulling 2.0.0");
    std::filesystem::remove_all(project);
}

// An unresolvable peer is skipped, not fatal: bun's failed-resolution report
// `continue`s on `is_peer()` before it ever checks `is_optional()`
// (PackageManagerResolution.rs:370-374).
void test_unresolvable_peer_does_not_fail_install(RegistryServer& server) {
    auto project{make_project("peermiss", R"({"name":"app","dependencies":{"peermiss":"^1.0.0"}})")};
    auto result{command::install_project(project, options_for(server))};
    check_true(result.has_value(), "e2e: unresolvable peer does not fail the install");
    if (!result) {
        std::println("  (error: {})", result.error().message);
        std::filesystem::remove_all(project);
        return;
    }
    check_true(!std::filesystem::exists(project / "node_modules/ghost"),
               "e2e: unresolvable peer left uninstalled");
    check_contains(read_file(project / "node_modules/peermiss/package.json"),
                   "\"name\":\"peermiss\"", "e2e: the peer's dependent still installed");
    std::filesystem::remove_all(project);
}

// An OPTIONAL peer is never resolved and never fetched — bun returns at the top
// of the enqueue path for it (PackageManagerEnqueue.rs:666-668). It binds only
// to a package some other edge already supplied. Getting this wrong is what
// makes an install drag in the likes of `@swc/core`: tsup marks all four of its
// peers optional and `bun install` records none of them.
void test_optional_peer_not_installed(RegistryServer& server) {
    auto project{make_project(
        "optpeer",
        R"({"name":"app","peerDependencies":{"bar":"^1.0.0"},)"
        R"("peerDependenciesMeta":{"bar":{"optional":true}}})")};
    auto result{command::install_project(project, options_for(server))};
    check_true(result.has_value(), "e2e: optional peer install succeeds");
    if (!result) {
        std::println("  (error: {})", result.error().message);
        std::filesystem::remove_all(project);
        return;
    }
    check_true(!std::filesystem::exists(project / "node_modules/bar"),
               "e2e: optional peer not fetched");
    std::filesystem::remove_all(project);
}

// The same optional peer, once another edge supplies the name, is simply there —
// binding in a flat node_modules is resolution finding what is already on disk.
void test_optional_peer_binds_when_supplied(RegistryServer& server) {
    auto project{make_project(
        "optpeerbind",
        R"({"name":"app","dependencies":{"bar":"^1.0.0"},)"
        R"("peerDependencies":{"bar":"^1.0.0"},"peerDependenciesMeta":{"bar":{"optional":true}}})")};
    auto result{command::install_project(project, options_for(server))};
    check_true(result.has_value(), "e2e: supplied optional peer install succeeds");
    if (!result) {
        std::println("  (error: {})", result.error().message);
        std::filesystem::remove_all(project);
        return;
    }
    check_contains(read_file(project / "node_modules/bar/package.json"), "\"name\":\"bar\"",
                   "e2e: optional peer bound to the dependency that supplied it");
    std::filesystem::remove_all(project);
}

// bun keeps the root in the lockfile's `package_index`, so a peer edge naming the
// project binds to the project (PackageManagerEnqueue.rs:2203-2218). `selfpeer`
// peers on `app`, which is this project — fetching a *copy of the root* from the
// registry instead is the bug this pins down (`@elysiajs/openapi` peers on
// `elysia` exactly this way).
void test_peer_on_root_package_binds_to_root(RegistryServer& server) {
    auto project{make_project("selfpeer", R"({"name":"app","dependencies":{"selfpeer":"^1.0.0"}})")};
    auto result{command::install_project(project, options_for(server))};
    check_true(result.has_value(), "e2e: peer-on-root install succeeds");
    if (!result) {
        std::println("  (error: {})", result.error().message);
        std::filesystem::remove_all(project);
        return;
    }
    check_true(!std::filesystem::exists(project / "node_modules/app"),
               "e2e: peer naming the root did not self-install a copy of it");
    std::filesystem::remove_all(project);
}

// ── the nested tree, end to end ─────────────────────────────────────────────
//
// This is the test that fails when the WIRING is removed, not just when the
// hoister's arithmetic is wrong. test_lockfile_tree already pins
// `hoist()`'s behavior against hand-built graphs; nothing there notices if
// `registry_install` never calls it. Everything asserted below is a real
// directory produced by a real install through the real pipeline:
//
//   - two versions of one name coexisting requires per-EDGE resolution
//     (stage 2). Put a name-keyed early return back into `enqueue_dep_` and the
//     second edge is never resolved at all, so `node_modules/shared` holds one
//     version and the nested directories do not exist.
//   - the nested directories themselves require `hoist_and_place_` (stage 3).
//     Skip the hoist and everything lands flat in the top-level node_modules.
//
// The layout mirrors `ignore` in the itty-router oracle 1:1 — see the fixture.
void test_nested_conflicting_versions(RegistryServer& server) {
    auto project{make_project(
        "nested",
        R"({"name":"app","dependencies":{"needsold":"^1.0.0","alsoold":"^1.0.0","shared":"^2.0.0"}})")};
    auto result{command::install_project(project, options_for(server))};
    check_true(result.has_value(), "e2e: nested-conflict install succeeds");
    if (!result) {
        std::println("  (error: {})", result.error().message);
        std::filesystem::remove_all(project);
        return;
    }
    // The root's own edge wins the top-level slot.
    check_contains(read_file(project / "node_modules/shared/package.json"),
                   "\"version\":\"2.0.0\"", "e2e: root's shared@^2 hoists to the top level");
    // ...and each dependent that cannot use it nests its own, exactly as bun
    // nests ignore@7.0.5 under both globby and @typescript-eslint/eslint-plugin.
    check_contains(read_file(project / "node_modules/needsold/node_modules/shared/package.json"),
                   "\"version\":\"1.0.0\"",
                   "e2e: needsold's shared@^1 nests under needsold");
    check_contains(read_file(project / "node_modules/alsoold/node_modules/shared/package.json"),
                   "\"version\":\"1.0.0\"",
                   "e2e: alsoold's shared@^1 nests under alsoold too");
    // The nested copies are content, not stubs.
    check_contains(read_file(project / "node_modules/needsold/node_modules/shared/index.js"),
                   "shared@1", "e2e: the nested package's files are actually extracted");
    // The store is an implementation detail and must not survive the install.
    check_true(!std::filesystem::exists(project / "node_modules/.mbun-store"),
               "e2e: the extraction store is cleaned up");
    std::filesystem::remove_all(project);
}

void test_scoped_package(RegistryServer& server) {
    auto project{
        make_project("scoped", R"({"name":"app","dependencies":{"@scope/pkg":"^1.0.0"}})")};
    auto result{command::install_project(project, options_for(server))};
    check_true(result.has_value(), "e2e: scoped install succeeds");
    if (result) {
        std::string json{read_file(project / "node_modules/@scope/pkg/package.json")};
        check_contains(json, "\"name\":\"@scope/pkg\"", "e2e: scoped layout @scope/pkg");
    } else {
        std::println("  (error: {})", result.error().message);
    }
    std::filesystem::remove_all(project);
}

void test_dist_tag(RegistryServer& server) {
    auto project{make_project("disttag", R"({"name":"app","dependencies":{"tagpkg":"beta"}})")};
    auto result{command::install_project(project, options_for(server))};
    check_true(result.has_value(), "e2e: dist-tag install succeeds");
    if (result) {
        std::string json{read_file(project / "node_modules/tagpkg/package.json")};
        check_contains(json, "2.0.0-beta.1", "e2e: dist-tag beta selected the prerelease");
    } else {
        std::println("  (error: {})", result.error().message);
    }
    std::filesystem::remove_all(project);
}

void test_remote_tarball(RegistryServer& server) {
    auto project{make_project(
        "tarball",
        std::format(R"({{"name":"app","dependencies":{{"tarpkg":"{}"}}}})",
                    server.url("/extra/tarpkg.tgz")))};
    auto result{command::install_project(project, options_for(server))};
    check_true(result.has_value(), "e2e: remote tarball install succeeds");
    if (result) {
        std::string json{read_file(project / "node_modules/tarpkg/package.json")};
        check_contains(json, "\"version\":\"3.2.1\"", "e2e: tarball package extracted");
        std::error_code ec;
        check_true(std::filesystem::is_symlink(project / "node_modules/.bin/tarpkg-cli", ec),
                   "e2e: tarball bin linked from extracted package.json");
    } else {
        std::println("  (error: {})", result.error().message);
    }
    std::filesystem::remove_all(project);
}

void test_integrity_tamper_fails(RegistryServer& server) {
    auto project{make_project("evil", R"({"name":"app","dependencies":{"evil":"^1.0.0"}})")};
    auto result{command::install_project(project, options_for(server))};
    check_true(!result.has_value(), "e2e: tampered integrity fails the install");
    if (!result) {
        check_true(result.error().code == command::ErrorCode::IntegrityCheckFailed,
                   "e2e: IntegrityCheckFailed code");
        check_true(result.error().message == "Integrity check failed for tarball: evil",
                   "e2e: bun-aligned integrity failure wording");
    }
    std::error_code ec;
    check_true(!std::filesystem::exists(project / "node_modules/evil", ec),
               "e2e: tampered package is not committed to node_modules");
    std::filesystem::remove_all(project);
}

void test_manifest_404_fails(RegistryServer& server) {
    auto project{make_project("missing", R"({"name":"app","dependencies":{"missing":"^1.0.0"}})")};
    auto result{command::install_project(project, options_for(server))};
    check_true(!result.has_value(), "e2e: missing packument fails the install");
    if (!result) {
        check_true(result.error().code == command::ErrorCode::ManifestFetchFailed,
                   "e2e: ManifestFetchFailed code");
        check_contains(result.error().message, "404", "e2e: 404 surfaced in the message");
    }
    std::filesystem::remove_all(project);
}

void test_no_matching_version(RegistryServer& server) {
    auto project{make_project("nomatch", R"({"name":"app","dependencies":{"foo":"^9.0.0"}})")};
    auto result{command::install_project(project, options_for(server))};
    check_true(!result.has_value(), "e2e: unsatisfiable range fails");
    if (!result) {
        check_true(result.error().code == command::ErrorCode::NoMatchingVersion,
                   "e2e: NoMatchingVersion code");
        check_contains(result.error().message,
                       "No version matching \"^9.0.0\" found for specifier \"foo\"",
                       "e2e: bun-aligned no-match wording");
    }
    std::filesystem::remove_all(project);
}

void test_file_dependency_regression(RegistryServer& server) {
    // Existing file: path must keep working alongside registry deps.
    auto project{make_project("filedep",
                              R"({"name":"app","dependencies":{"local":"file:./local-pkg"}})")};
    std::filesystem::create_directories(project / "local-pkg");
    std::ofstream{project / "local-pkg/package.json"}
        << R"({"name":"local","version":"1.0.0"})";
    std::ofstream{project / "local-pkg/index.js"} << "module.exports = 'local';\n";
    auto result{command::install_project(project, options_for(server))};
    check_true(result.has_value(), "e2e: file: dependency still installs");
    if (result) {
        check_true(result->installed == 1, "e2e: file: install count");
        check_true(read_file(project / "node_modules/local/index.js") ==
                       "module.exports = 'local';\n",
                   "e2e: file: content cloned");
    } else {
        std::println("  (error: {})", result.error().message);
    }
    std::filesystem::remove_all(project);
}

// The point of the concurrent engine, asserted end to end through the real
// install entry point rather than against the engine in isolation.
//
// bun hands its whole discovered frontier to the HTTP thread at once and lets
// the global 64-request budget meter it (schedule_tasks, runTasks.rs:1655-1678;
// DEFAULT_MAX_SIMULTANEOUS_REQUESTS_FOR_BUN_INSTALL, PackageManager.rs:335).
// So N independent packages against a server that holds every response for
// `delay` must cost about one manifest round plus one tarball round — ~2×delay
// — not 2N×delay. Its own server: `delay` and the connection counters must not
// leak into the other tests.
void test_install_downloads_packages_concurrently() {
    RegistryServer server{};
    check_true(server.ok(), "concurrency: server started");
    if (!server.ok()) {
        return;
    }

    constexpr int PACKAGES{8};
    constexpr std::chrono::milliseconds DELAY{100};
    // Independent packages: no transitive edges, so the entire frontier is
    // discoverable at once and nothing *forces* an ordering. Anything serial
    // here is the pipeline's own doing.
    std::string deps;
    for (int i{0}; i < PACKAGES; ++i) {
        const std::string name{std::format("par{}", i)};
        const std::vector<TarFile> files{
            {"package/package.json", std::format(R"({{"name":"{}","version":"1.0.0"}})", name)},
        };
        const std::vector<std::uint8_t> tgz{make_tgz(files)};
        const std::string tarballPath{std::format("/tarballs/{}-1.0.0.tgz", name)};
        server.add("/" + name,
                   http_response(200, "OK",
                                 std::format(R"({{"name":"{}","dist-tags":{{"latest":"1.0.0"}},)"
                                             R"("versions":{{{}}}}})",
                                             name,
                                             version_json(name, "1.0.0", server.url(tarballPath),
                                                          sri_for(tgz), "")),
                                 "application/json"));
        server.add(tarballPath,
                   http_response(200, "OK",
                                 std::string{reinterpret_cast<const char*>(tgz.data()), tgz.size()},
                                 "application/octet-stream"));
        if (!deps.empty()) {
            deps.push_back(',');
        }
        deps.append(std::format(R"("{}":"^1.0.0")", name));
    }
    server.set_delay(DELAY);
    server.start();

    auto project{make_project("concurrent",
                              std::format(R"({{"name":"app","dependencies":{{{}}}}})", deps))};
    const auto started{std::chrono::steady_clock::now()};
    auto result{command::install_project(project, options_for(server))};
    const auto elapsed{std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started)};

    check_true(result.has_value(), "concurrency: all packages install");
    if (!result) {
        std::println("  (error: {})", result.error().message);
        std::filesystem::remove_all(project);
        return;
    }
    check_true(result->installed == PACKAGES, "concurrency: every package installed");
    // One connection per request: keep-alive reuse is the pool's job (T2) and
    // this server closes after every response regardless.
    check_true(server.accepted() == PACKAGES * 2, "concurrency: every request reached the server");

    // The load-bearing pair. Generous margins on purpose: this asserts
    // "overlapped", not a stopwatch.
    const auto serial{DELAY * PACKAGES * 2};
    check_true(elapsed < serial / 4, "concurrency: wall clock is far below serial");
    check_true(server.peak_in_flight() >= PACKAGES / 2,
               "concurrency: requests are genuinely in flight together");
    std::println("  (install-concurrency: {} packages ({} requests) in {}ms; serial would be "
                 "{}ms; peak {} connections in flight)",
                 PACKAGES, PACKAGES * 2, elapsed.count(), serial.count(),
                 server.peak_in_flight());
    std::filesystem::remove_all(project);
}

void test_frozen_lockfile_still_requires_lock(RegistryServer& server) {
    auto project{make_project("frozen", R"({"name":"app","dependencies":{"foo":"^1.0.0"}})")};
    command::InstallOptions options{options_for(server)};
    options.frozenLockfile = true;
    auto result{command::install_project(project, options)};
    check_true(!result.has_value() &&
                   result.error().code == command::ErrorCode::LockfileOutOfDate,
               "e2e: --frozen-lockfile without bun.lock still fails (no regression)");
    std::filesystem::remove_all(project);
}

#endif  // !defined(_WIN32)

}  // namespace

int main() {
#if defined(_WIN32)
    // The blocking executor is DEFERRED on Windows; the e2e pipeline cannot
    // run without it, and pretending otherwise would be a fake green.
    std::println("test_install_e2e: SKIPPED on Windows (http_executor winsock DEFERRED)");
#else
    RegistryServer server{};
    check_true(server.ok(), "e2e: registry server started");
    if (server.ok()) {
        Fixtures fixtures{};
        fixtures.install(server);
        server.start();

        test_semver_selection_layout_bins_transitive(server, fixtures);
        test_nested_conflicting_versions(server);
        test_scoped_package(server);
        test_dist_tag(server);
        test_remote_tarball(server);
        test_integrity_tamper_fails(server);
        test_manifest_404_fails(server);
        test_no_matching_version(server);
        test_file_dependency_regression(server);
        test_root_peer_dependency_installed(server);
        test_root_peer_duplicate_loses_to_dependency(server);
        test_transitive_peer_dependency_installed(server);
        test_peer_binds_to_existing_resolution(server);
        test_unresolvable_peer_does_not_fail_install(server);
        test_optional_peer_not_installed(server);
        test_optional_peer_binds_when_supplied(server);
        test_peer_on_root_package_binds_to_root(server);
        test_frozen_lockfile_still_requires_lock(server);
        test_install_downloads_packages_concurrently();
    }
#endif

    std::println("test_install_e2e: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
