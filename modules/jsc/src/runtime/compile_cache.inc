// compile_cache.inc — node's module compile cache (NODE_COMPILE_CACHE,
// module.enableCompileCache/getCompileCacheDir/flushCompileCache).
//
// Ported 1:1 from the vendored upstream implementation, compat/node/src/
// compile_cache.cc + src/node_modules.cc's {Enable,Flush}CompileCache: the
// directory layout, the cache key, the on-disk record format, the accept/
// reject/refresh state machine and every NODE_DEBUG_NATIVE=COMPILE_CACHE trace
// string are the upstream ones, so a cache directory written here has the same
// shape (one $TAG subdirectory, one file per cached module) and the same
// diagnostics as node's.
//
// Directory structure (upstream comment, unchanged):
//   - compile cache directory (from NODE_COMPILE_CACHE / enableCompileCache)
//     - $VERSION-$ARCH-$CACHE_DATA_VERSION_TAG-$UID
//       - $FILENAME_AND_MODULE_TYPE_HASH: a hash of filename + module type
//
// On-disk record (upstream comment, unchanged):
//   [uint32_t] magic number
//   [uint32_t] code size
//   [uint32_t] code hash
//   [uint32_t] cache size
//   [uint32_t] cache hash
//   .... compile cache content ....
//
// DEFERRED — the cache CONTENT. V8's code cache is a serialised compilation
// artifact; JSC's C API exposes no equivalent (JSC serialises bytecode only
// through its own private CachedBytecode path, which the embedding C API does
// not surface). So the payload stored here is a source fingerprint rather than
// bytecode, exactly as mbun's vm.Script cachedData already does (see
// builtins/node_vm.cppm "cachedData: a source fingerprint, not bytecode"). The
// consequence is precise and worth stating: the cache is a real cache in every
// respect that is OBSERVABLE — it is written, read back, validated against the
// source, invalidated when the source changes, relocatable in portable mode,
// and reports the upstream statuses — but it cannot yet make a second run
// compile faster, because there is no bytecode to skip re-parsing. Swapping the
// payload for real bytecode is a change to serialize_cache_payload_() alone.

// zlib's crc32 (IEEE 802.3), which is what upstream hashes both cache keys and
// cache/code content with. Reimplemented rather than pulled from the compress
// member so this file has no link dependency; the polynomial and the pre/post
// conditioning are zlib's, so the values match node's byte for byte.
inline std::uint32_t cc_crc32(std::uint32_t crc, const unsigned char* buf, std::size_t len) {
    static const auto table{[] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i{0}; i < 256; ++i) {
            std::uint32_t c{i};
            for (int k{0}; k < 8; ++k) c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }()};
    crc ^= 0xFFFFFFFFu;
    for (std::size_t i{0}; i < len; ++i) crc = table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

inline std::uint32_t cc_hash(std::string_view data) {
    return cc_crc32(0, reinterpret_cast<const unsigned char*>(data.data()), data.size());
}

// Upstream Uint32ToHex: 8 lowercase hex digits, no prefix, no padding logic
// beyond the fixed width.
inline std::string cc_uint32_to_hex(std::uint32_t v) {
    std::string s;
    s.reserve(8);
    for (int i{28}; i >= 0; i -= 4) {
        const char digit{static_cast<char>((v >> i) & 0xF)};
        s.push_back(static_cast<char>(digit + (digit < 10 ? '0' : 'a' - 10)));
    }
    return s;
}

// The runtime version that keys the cache directory. mbun reports itself as
// this Node version through process.version (bindings_install.inc), and the
// cache tag has to agree with the identity the cache was produced under.
inline constexpr const char* kCompileCacheRuntimeVersion{"24.3.0"};

// node checks the permission model before creating the cache directory, and
// reports a FAILED status (not a throw) when either side is denied. gPermission
// is the process-wide model from common.inc, and it only answers meaningfully
// once --permission has turned it on: with the model off every fs scope reads
// as ungranted (nothing was ever allowed), which is the opposite of what an
// ordinary run means. node asks its own permission object only under the
// Permission Model too, so the gate goes here.
inline bool compile_cache_permission_granted_(const std::string& path, bool write) {
    if (!gPermission.enabled()) return true;
    return gPermission.is_granted(
        write ? mbun::permission::Scope::FileSystemWrite : mbun::permission::Scope::FileSystemRead,
        path);
}

// CACHED_CODE_TYPES, upstream src/compile_cache.h.
enum class CachedCodeType : std::uint8_t { kCommonJS = 0, kESM = 1, kStrippedTypeScript = 2 };

// COMPILE_CACHE_STATUS, upstream src/compile_cache.h. Mirrored by
// Module.constants.compileCacheStatus in builtins/node_module.cppm.
enum class CompileCacheEnableStatus : std::uint8_t {
    kFailed = 0,
    kEnabled = 1,
    kAlreadyEnabled = 2,
    kDisabled = 3,
};

struct CompileCacheEntry {
    std::string cache;       // the payload; empty + !has_cache means "not initialized"
    bool has_cache{false};
    std::uint32_t cache_key{0};
    std::uint32_t code_hash{0};
    std::uint32_t code_size{0};
    std::string cache_filename;
    std::string source_filename;
    CachedCodeType type{CachedCodeType::kCommonJS};
    bool refreshed{false};
    bool persisted{false};

    const char* type_name() const {
        switch (type) {
            case CachedCodeType::kCommonJS: return "CommonJS";
            case CachedCodeType::kESM: return "ESM";
            case CachedCodeType::kStrippedTypeScript: return "StrippedTypeScript";
        }
        return "CommonJS";
    }
};

class CompileCacheHandler {
  public:
    struct EnableResult {
        CompileCacheEnableStatus status{CompileCacheEnableStatus::kFailed};
        std::string cache_directory;
        std::string message;
    };

    // Process-lived, like node's env->compile_cache_handler().
    static CompileCacheHandler& get() {
        static CompileCacheHandler handler;
        return handler;
    }

    bool enabled() const { return !dir_with_tag_.empty(); }
    const std::string& base_directory() const { return base_dir_; }

    // Upstream CompileCacheHandler::Enable.
    EnableResult enable(const std::string& dir, bool portable) {
        EnableResult result;
        const std::string cache_tag{cache_version_tag_()};
        std::error_code ec;
        std::filesystem::path base{dir};
        if (!base.is_absolute()) base = std::filesystem::current_path(ec) / base;
        const std::string absolute_base{base.lexically_normal().string()};
        const std::string dir_with_tag{
            (std::filesystem::path{absolute_base} / cache_tag).string()};

        debug_(std::format("[compile cache] resolved path {} + {} -> {}\n", dir, cache_tag,
                           dir_with_tag));

        // node consults the permission model for both write and read on the
        // tagged directory before creating it; --permission users get a FAILED
        // status with these exact messages rather than a thrown error.
        if (!compile_cache_permission_granted_(dir_with_tag, /*write=*/true)) {
            result.message = "Skipping compile cache because write permission for " +
                             dir_with_tag + " is not granted";
            result.status = CompileCacheEnableStatus::kFailed;
            return result;
        }
        if (!compile_cache_permission_granted_(dir_with_tag, /*write=*/false)) {
            result.message = "Skipping compile cache because read permission for " +
                             dir_with_tag + " is not granted";
            result.status = CompileCacheEnableStatus::kFailed;
            return result;
        }

        ec.clear();
        std::filesystem::create_directories(std::filesystem::path{dir_with_tag}, ec);
        const bool exists{std::filesystem::is_directory(std::filesystem::path{dir_with_tag})};
        debug_(std::format("[compile cache] creating cache directory {}...{}\n", dir_with_tag,
                           exists ? "success" : ec.message()));
        if (!exists) {
            result.message = "Cannot create cache directory: " + ec.message();
            result.status = CompileCacheEnableStatus::kFailed;
            return result;
        }

        result.cache_directory = absolute_base;
        base_dir_ = absolute_base;
        dir_with_tag_ = dir_with_tag;
        portable_ = portable;
        result.status = CompileCacheEnableStatus::kEnabled;

        // node flushes what it accumulated at process shutdown (in addition to
        // any explicit module.flushCompileCache()). atexit is the same hook
        // node_os.inc uses to restore the tty mode, and runs on the normal
        // exit(3) path process.exit() takes.
        static bool registered{false};
        if (!registered) {
            registered = true;
            std::atexit([] { CompileCacheHandler::get().persist(); });
        }
        return result;
    }

    // Upstream CompileCacheHandler::GetOrInsert, but handing back the cache KEY
    // rather than a pointer into the store. Upstream can pass the entry pointer
    // straight to MaybeSave because compilation and saving are adjacent; mbun's
    // entry point compiles and RUNS in one step, so user code -- including a
    // module.flushCompileCache(), which empties the store -- gets to run between
    // the two. A key survives that; a pointer does not.
    std::optional<std::uint32_t> get_or_insert(std::string_view code, const std::string& filename,
                                               CachedCodeType type) {
        if (dir_with_tag_.empty()) return std::nullopt;

        std::string file_path{filename};
        // Portable mode keys on the module's position RELATIVE to the cache
        // directory, so moving the whole tree (cache included) keeps the cache
        // valid. Upstream only does this for absolute paths.
        if (portable_ && std::filesystem::path{file_path}.is_absolute()) {
            const std::string relative{
                std::filesystem::path{file_path}
                    .lexically_normal()
                    .lexically_relative(std::filesystem::path{dir_with_tag_}.lexically_normal())
                    .generic_string()};
            if (!relative.empty()) {
                file_path = relative;
                debug_(std::format("[compile cache] using relative path {} from {}\n", file_path,
                                   base_dir_));
            }
        }

        const std::uint32_t key{cache_key_(file_path, type)};
        const std::uint32_t code_hash{cc_hash(code)};

        if (auto it{store_.find(key)}; it != store_.end() && it->second.code_hash == code_hash) {
            return key;
        }

        // A code-hash mismatch means the source changed: discard the stale
        // entry and build a fresh one in its place.
        CompileCacheEntry& entry{store_[key]};
        entry = CompileCacheEntry{};
        entry.code_hash = code_hash;
        entry.code_size = static_cast<std::uint32_t>(code.size());
        entry.cache_key = key;
        entry.cache_filename =
            (std::filesystem::path{dir_with_tag_} / cc_uint32_to_hex(key)).string();
        entry.source_filename = filename;
        entry.type = type;
        read_cache_file_(&entry);
        return key;
    }

    // Save against a key looked up fresh. An entry that a flush removed while
    // the module was running is simply gone: the next compilation of that file
    // re-inserts it, which is exactly what upstream's cleared store does.
    void maybe_save(std::uint32_t key, std::string_view code, bool rejected) {
        if (auto it{store_.find(key)}; it != store_.end()) {
            maybe_save(&it->second, code, rejected);
        }
    }

    // Upstream CompileCacheHandler::MaybeSaveImpl. `rejected` is the engine's
    // verdict on a cache it was handed; mbun validates the payload itself (see
    // the DEFERRED note at the top) and passes the same signal through.
    void maybe_save(CompileCacheEntry* entry, std::string_view code, bool rejected) {
        if (entry == nullptr) return;
        debug_(std::format("[compile cache] V8 code cache for {} {} was {}, ", entry->type_name(),
                           entry->source_filename,
                           rejected ? "rejected" : (!entry->has_cache ? "not initialized"
                                                                     : "accepted")));
        if (entry->has_cache && !rejected) {
            debug_("keeping the in-memory entry\n");
            return;
        }
        debug_(std::format("{} the in-memory entry\n", !entry->has_cache ? "initializing"
                                                                        : "refreshing"));
        entry->cache = serialize_cache_payload_(code);
        entry->has_cache = true;
        entry->refreshed = true;
    }

    // Upstream CompileCacheHandler::Persist. Writes to a temporary file next to
    // the target and renames, so a concurrent reader never sees a torn record.
    void persist() {
        if (dir_with_tag_.empty()) return;
        for (auto& pair : store_) {
            CompileCacheEntry* entry{&pair.second};
            const char* type_name{entry->type_name()};
            if (!entry->has_cache) {
                debug_(std::format(
                    "[compile cache] skip persisting {} {} because the cache was not initialized\n",
                    type_name, entry->source_filename));
                continue;
            }
            if (!entry->refreshed) {
                debug_(std::format(
                    "[compile cache] skip persisting {} {} because cache was the same\n", type_name,
                    entry->source_filename));
                continue;
            }
            if (entry->persisted) {
                debug_(std::format(
                    "[compile cache] skip persisting {} {} because cache was already persisted\n",
                    type_name, entry->source_filename));
                continue;
            }

            const std::uint32_t cache_size{static_cast<std::uint32_t>(entry->cache.size())};
            const std::uint32_t cache_hash{cc_hash(entry->cache)};
            const std::array<std::uint32_t, kHeaderCount> headers{
                kCacheMagicNumber, entry->code_size, cache_size, entry->code_hash, cache_hash};

            // Write to a sibling temporary file and rename, so a concurrent
            // reader either sees the old record or the new one, never a torn
            // one (upstream uses uv_fs_mkstemp for the same reason; pid plus a
            // per-process counter is unique across the racing writers this
            // guards against, which are separate processes).
            debug_(std::format("[compile cache] Creating temporary file for cache of {} ({})...",
                               entry->source_filename, type_name));
            static unsigned long long tmp_counter{0};
            const std::string tmp_path{entry->cache_filename + "." +
                                       std::to_string(static_cast<long long>(::getpid())) + "." +
                                       std::to_string(tmp_counter++)};
            std::ofstream out{tmp_path, std::ios::binary | std::ios::trunc};
            if (!out) {
                debug_(std::format("failed. {}\n", std::strerror(errno)));
                continue;
            }
            debug_(std::format(" -> {}\n", tmp_path));
            debug_(std::format(
                "[compile cache] writing cache for {} {} to temporary file {} [{} {} {} {} {}]...",
                type_name, entry->source_filename, tmp_path, headers[0], headers[1], headers[2],
                headers[3], headers[4]));

            out.write(reinterpret_cast<const char*>(headers.data()),
                      static_cast<std::streamsize>(headers.size() * sizeof(std::uint32_t)));
            out.write(entry->cache.data(), static_cast<std::streamsize>(entry->cache.size()));
            out.close();
            if (!out) {
                debug_(std::format("failed: {}\n", std::strerror(errno)));
                std::error_code rm;
                std::filesystem::remove(std::filesystem::path{tmp_path}, rm);
                continue;
            }
            debug_("success\n");

            debug_(std::format("[compile cache] Renaming {} to {}...", tmp_path,
                               entry->cache_filename));
            std::error_code ec;
            std::filesystem::rename(std::filesystem::path{tmp_path},
                                    std::filesystem::path{entry->cache_filename}, ec);
            if (ec) {
                debug_(std::format("failed: {}\n", ec.message()));
                std::error_code rm;
                std::filesystem::remove(std::filesystem::path{tmp_path}, rm);
                continue;
            }
            debug_("success\n");
            entry->persisted = true;
        }
        debug_("[compile cache] Clear deserialized cache.\n");
        store_.clear();
    }

    bool is_debug() const { return is_debug_; }

    void debug_(std::string_view message) const {
        if (!is_debug_) return;
        std::fwrite(message.data(), 1, message.size(), stderr);
        std::fflush(stderr);
    }

  private:
    // Used for identifying and verifying a file is a compile cache file
    // (upstream kCacheMagicNumber).
    static constexpr std::uint32_t kCacheMagicNumber{0x8adfdbb2};
    static constexpr std::size_t kHeaderCount{5};

    CompileCacheHandler() {
        // node gates these traces on the COMPILE_CACHE debug category, which is
        // selected by a comma-separated NODE_DEBUG_NATIVE list.
        const char* spec{std::getenv("NODE_DEBUG_NATIVE")};
        is_debug_ = spec != nullptr && std::string_view{spec}.find("COMPILE_CACHE") !=
                                           std::string_view::npos;
    }

    // Upstream GetCacheKey: crc32 over the type byte, then the file path.
    static std::uint32_t cache_key_(std::string_view filename, CachedCodeType type) {
        const auto raw{static_cast<std::uint8_t>(type)};
        std::uint32_t crc{cc_crc32(0, &raw, sizeof(raw))};
        return cc_crc32(crc, reinterpret_cast<const unsigned char*>(filename.data()),
                        filename.size());
    }

    // Upstream GetCacheVersionTag. The version/arch/uid split exists so that
    // two builds, two architectures or two users never read each other's
    // records; the payload tag stands in for V8's CachedDataVersionTag, which
    // has no JSC equivalent, and is bumped whenever the payload format changes.
    static std::string cache_version_tag_() {
#if defined(__aarch64__) || defined(_M_ARM64)
        constexpr const char* arch{"arm64"};
#elif defined(__x86_64__) || defined(_M_X64)
        constexpr const char* arch{"x64"};
#else
        constexpr const char* arch{"unknown"};
#endif
        // The payload tag stands in for V8's ScriptCompiler::CachedDataVersionTag:
        // bump it whenever serialize_cache_payload_ changes shape, so records
        // written by an older build are keyed elsewhere instead of misread.
        constexpr std::uint32_t payload_tag{1};
        std::string tag{std::string{"v"} + kCompileCacheRuntimeVersion + "-" + arch + "-" +
                        cc_uint32_to_hex(payload_tag)};
        tag += "-" + std::to_string(static_cast<long long>(::getuid()));
        return tag;
    }

    // See the DEFERRED note at the top of the file: a deterministic fingerprint
    // of the source stands in for serialised bytecode. Deterministic is the
    // load-bearing property — a second run over unchanged source must produce
    // byte-identical content so the entry reads back as accepted and is not
    // rewritten.
    static std::string serialize_cache_payload_(std::string_view code) {
        std::string out{"mbun-compile-cache:"};
        out += cc_uint32_to_hex(cc_hash(code));
        out += ':';
        out += cc_uint32_to_hex(static_cast<std::uint32_t>(code.size()));
        return out;
    }

    // Upstream CompileCacheHandler::ReadCacheFile, including its trace shape:
    // one unterminated "reading cache from ..." line that is finished either by
    // an error string or by the header dump plus " success, size=N".
    void read_cache_file_(CompileCacheEntry* entry) {
        debug_(std::format("[compile cache] reading cache from {} for {} {}...",
                           entry->cache_filename, entry->type_name(), entry->source_filename));

        std::ifstream in{entry->cache_filename, std::ios::binary};
        if (!in) {
            debug_(std::format(" {}\n", std::strerror(ENOENT)));
            return;
        }

        std::array<std::uint32_t, kHeaderCount> headers{};
        in.read(reinterpret_cast<char*>(headers.data()),
                static_cast<std::streamsize>(headers.size() * sizeof(std::uint32_t)));
        if (in.gcount() != static_cast<std::streamsize>(headers.size() * sizeof(std::uint32_t))) {
            debug_(std::format("reading header failed, bytes read {}\n",
                               static_cast<long long>(in.gcount())));
            return;
        }

        debug_(std::format("[{} {} {} {} {}]...", headers[0], headers[1], headers[2], headers[3],
                           headers[4]));

        if (headers[0] != kCacheMagicNumber) {
            debug_(std::format("magic number mismatch: expected {}, actual {}\n", kCacheMagicNumber,
                               headers[0]));
            return;
        }
        if (headers[1] != entry->code_size) {
            debug_(std::format("code size mismatch: expected {}, actual {}\n", entry->code_size,
                               headers[1]));
            return;
        }
        if (headers[3] != entry->code_hash) {
            debug_(std::format("code hash mismatch: expected {}, actual {}\n", entry->code_hash,
                               headers[3]));
            return;
        }

        std::string payload{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
        if (headers[2] != payload.size()) {
            debug_(std::format("cache size mismatch: expected {}, actual {}\n", headers[2],
                               payload.size()));
            return;
        }
        if (const std::uint32_t hash{cc_hash(payload)}; headers[4] != hash) {
            debug_(std::format("cache hash mismatch: expected {}, actual {}\n", headers[4], hash));
            return;
        }

        entry->cache = std::move(payload);
        entry->has_cache = true;
        debug_(std::format(" success, size={}\n", entry->cache.size()));
    }

    bool is_debug_{false};
    bool portable_{false};
    std::string base_dir_;
    std::string dir_with_tag_;
    std::map<std::uint32_t, CompileCacheEntry> store_;
};

// ── __mbunCompileCacheNative ────────────────────────────────────────────────
// The JS seam node:module's enableCompileCache/getCompileCacheDir/
// flushCompileCache go through (builtins/node_module.cppm). Shapes match
// upstream src/node_modules.cc's binding: enable() returns the
// [status, message, directory] triple internal/modules/helpers.js destructures.

// internalBinding('modules').enableCompileCache(directory, portable)
JSValueRef compile_cache_enable_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                                   const JSValueRef argv[], JSValueRef*) {
    const std::string dir{argc > 0 ? val_to_string(ctx, argv[0]) : std::string{}};
    const bool portable{argc > 1 && JSValueToBoolean(ctx, argv[1])};

    auto& handler{CompileCacheHandler::get()};
    CompileCacheHandler::EnableResult result;
    if (handler.enabled()) {
        // node reports ALREADY_ENABLED and leaves the first directory in place;
        // the second directory is never created (a test counts the entries in
        // the temp dir to prove exactly that).
        result.status = CompileCacheEnableStatus::kAlreadyEnabled;
        result.cache_directory = handler.base_directory();
    } else {
        result = handler.enable(dir, portable);
        // Upstream Environment::EnableCompileCache traces whatever explanation
        // Enable() produced -- a denied permission, an uncreatable directory --
        // rather than leaving the caller as the only one who can see it. That
        // trace is the only report a NODE_COMPILE_CACHE run gets, since nothing
        // in JS ever looks at the status on that path.
        if (!result.message.empty()) {
            handler.debug_("[compile cache] " + result.message + "\n");
        }
    }

    const JSValueRef entries[3]{
        JSValueMakeNumber(ctx, static_cast<double>(static_cast<int>(result.status))),
        make_string(ctx, result.message),
        make_string(ctx, result.cache_directory),
    };
    return JSObjectMakeArray(ctx, 3, entries, nullptr);
}

JSValueRef compile_cache_dir_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t,
                                const JSValueRef[], JSValueRef*) {
    auto& handler{CompileCacheHandler::get()};
    if (!handler.enabled()) return JSValueMakeUndefined(ctx);
    return make_string(ctx, handler.base_directory());
}

// internalBinding('modules').flushCompileCache(). node brackets the flush with
// its own pair of traces (src/node_modules.cc FlushCompileCache), and the test
// asserts the per-entry writes land BEFORE "finished" -- i.e. that flushing is
// not deferred to shutdown.
JSValueRef compile_cache_flush_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t,
                                  const JSValueRef[], JSValueRef*) {
    auto& handler{CompileCacheHandler::get()};
    if (handler.enabled()) {
        handler.debug_("[compile cache] module.flushCompileCache() requested.\n");
        handler.persist();
        handler.debug_("[compile cache] module.flushCompileCache() finished.\n");
    }
    return JSValueMakeUndefined(ctx);
}
