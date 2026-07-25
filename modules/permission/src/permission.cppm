// mbun.permission — node's Permission Model, translated.
//
// Blueprint (all under compat/node/):
//   src/permission/permission_base.h   the PERMISSIONS scope table + labels/flags
//   src/permission/permission.cc       Permission (enable, apply, is_granted, drop)
//   src/permission/fs_permission.cc/.h FSPermission + the RadixTree path matcher
//   src/permission/{child_process,worker,wasi,inspector,addon,net,ffi}_permission.cc
//   src/env.cc:920-985                 which scopes are applied for which flags
//   src/path.cc PathResolve            the resolution every fs path goes through
//
// WHY a separate member: the model is pure logic (a scope table, a radix tree, a
// path normaliser) and it is the security-critical half of the feature, so it is
// unit-testable off the JS engine — `mcpp test` in this directory exercises the
// wildcard/traversal/drop semantics directly against node's own test vectors
// (test-permission-fs-wildcard, test-permission-drop-fs-*). The jsc member owns
// the other half: the syscall boundary that consults this model.
//
// Two asymmetries in node's design that are easy to get backwards, and which the
// tests below pin down:
//
//  1. `Apply` means DENY for the single-state scopes. env.cc calls
//     `Apply({"*"}, kChildProcess)` when `--allow-child-process` is ABSENT, and
//     ChildProcessPermission::Apply sets `deny_all_ = true`. Net is the opposite:
//     `Apply` is called when `--allow-net` is PRESENT and sets `allow_net_ = true`.
//  2. A granted fs path that is a DIRECTORY at startup is stored with `/*`
//     appended (fs_permission.cc WildcardIfDir), so the grant covers the subtree.
//     That stat happens once, at Apply/Drop time — not per lookup.
export module mbun.permission;

import std;

export namespace mbun::permission {

// ── the scope table (node src/permission/permission_base.h PERMISSIONS) ──────
enum class Scope {
    FileSystem,
    FileSystemRead,
    FileSystemWrite,
    ChildProcess,
    Wasi,
    WorkerThreads,
    Inspector,
    Net,
    Addon,
    Ffi,
    // node's kPermissionsRoot: "not a scope". StringToPermission returns it for
    // an unknown label and both Has and Drop then answer false / no-op.
    Root,
};

// The label a script passes to process.permission.has() ("fs.read"), NOT the
// name reported on the error (see scope_name).
constexpr Scope scope_from_label(std::string_view label) noexcept {
    if (label == "fs") return Scope::FileSystem;
    if (label == "fs.read") return Scope::FileSystemRead;
    if (label == "fs.write") return Scope::FileSystemWrite;
    if (label == "child") return Scope::ChildProcess;
    if (label == "wasi") return Scope::Wasi;
    if (label == "worker") return Scope::WorkerThreads;
    if (label == "inspector") return Scope::Inspector;
    if (label == "net") return Scope::Net;
    if (label == "addon") return Scope::Addon;
    if (label == "ffi") return Scope::Ffi;
    return Scope::Root;
}

// node Permission::PermissionToString — the `#Name` of the table row, which is
// what lands on `err.permission` ("FileSystemRead", not "fs.read").
constexpr std::string_view scope_name(Scope s) noexcept {
    switch (s) {
        case Scope::FileSystem:      return "FileSystem";
        case Scope::FileSystemRead:  return "FileSystemRead";
        case Scope::FileSystemWrite: return "FileSystemWrite";
        case Scope::ChildProcess:    return "ChildProcess";
        case Scope::Wasi:            return "WASI";
        case Scope::WorkerThreads:   return "WorkerThreads";
        case Scope::Inspector:       return "Inspector";
        case Scope::Net:             return "Net";
        case Scope::Addon:           return "Addon";
        case Scope::Ffi:             return "FFI";
        case Scope::Root:            return "";
    }
    return "";
}

// node GetErrorFlagSuggestion: the 4th column of the PERMISSIONS table. The
// FileSystem row is deliberately empty (there is no --allow-fs).
constexpr std::string_view scope_flag(Scope s) noexcept {
    switch (s) {
        case Scope::FileSystemRead:  return "--allow-fs-read";
        case Scope::FileSystemWrite: return "--allow-fs-write";
        case Scope::ChildProcess:    return "--allow-child-process";
        case Scope::Wasi:            return "--allow-wasi";
        case Scope::WorkerThreads:   return "--allow-worker";
        case Scope::Inspector:       return "--allow-inspector";
        case Scope::Net:             return "--allow-net";
        case Scope::Addon:           return "--allow-addons";
        case Scope::Ffi:             return "--allow-ffi";
        default:                     return "";
    }
}

// node CreateAccessDeniedError's message body.
inline std::string access_denied_message(Scope s) {
    const std::string_view flag{scope_flag(s)};
    std::string msg{"Access to this API has been restricted."};
    if (!flag.empty()) {
        msg += " Use ";
        msg += flag;
        msg += " to manage permissions.";
    }
    return msg;
}

// ── path.resolve (node src/path.cc PathResolve, POSIX arm) ───────────────────
// node lib/path.js normalizeString, verbatim: it is what makes `..` traversal
// collapse BEFORE the radix lookup, which is the whole defence in
// test-permission-fs-traversal-path.
inline std::string normalize_string(std::string_view path, bool allowAboveRoot) {
    std::string res{};
    std::size_t lastSegmentLength{0};
    std::ptrdiff_t lastSlash{-1};
    int dots{0};
    char code{0};
    for (std::size_t i{0}; i <= path.size(); ++i) {
        if (i < path.size()) {
            code = path[i];
        } else if (code == '/') {
            break;
        } else {
            code = '/';
        }

        if (code == '/') {
            if (lastSlash == static_cast<std::ptrdiff_t>(i) - 1 || dots == 1) {
                // NOOP — empty segment or "."
            } else if (dots == 2) {
                if (res.size() < 2 || lastSegmentLength != 2 || res[res.size() - 1] != '.' ||
                    res[res.size() - 2] != '.') {
                    if (res.size() > 2) {
                        const auto lastSlashIndex{res.rfind('/')};
                        if (lastSlashIndex == std::string::npos) {
                            res.clear();
                            lastSegmentLength = 0;
                        } else {
                            res.resize(lastSlashIndex);
                            const auto prev{res.rfind('/')};
                            lastSegmentLength =
                                prev == std::string::npos ? res.size() : res.size() - 1 - prev;
                        }
                        lastSlash = static_cast<std::ptrdiff_t>(i);
                        dots = 0;
                        continue;
                    }
                    if (!res.empty()) {
                        res.clear();
                        lastSegmentLength = 0;
                        lastSlash = static_cast<std::ptrdiff_t>(i);
                        dots = 0;
                        continue;
                    }
                }
                if (allowAboveRoot) {
                    res += res.empty() ? ".." : "/..";
                    lastSegmentLength = 2;
                }
            } else {
                const std::size_t from{static_cast<std::size_t>(lastSlash + 1)};
                const std::string_view segment{path.substr(from, i - from)};
                if (!res.empty()) res += '/';
                res += segment;
                lastSegmentLength = i - static_cast<std::size_t>(lastSlash) - 1;
            }
            lastSlash = static_cast<std::ptrdiff_t>(i);
            dots = 0;
        } else if (code == '.' && dots != -1) {
            ++dots;
        } else {
            dots = -1;
        }
    }
    return res;
}

// node PathResolve(env, {p}) with `cwd` standing in for env->GetCwd().
inline std::string path_resolve(std::string_view cwd, std::string_view p) {
    std::string resolved{};
    bool absolute{false};
    if (!p.empty()) {
        resolved = std::string{p} + "/";
        if (p.front() == '/') absolute = true;
    }
    if (!absolute && !cwd.empty()) {
        resolved = std::string{cwd} + "/" + resolved;
        if (cwd.front() == '/') absolute = true;
    }
    const std::string normalized{normalize_string(resolved, !absolute)};
    if (absolute) return "/" + normalized;
    if (normalized.empty()) return ".";
    return normalized;
}

// ── the fs matcher (node src/permission/fs_permission.h RadixTree) ───────────
// Translated node-for-node, including NextNode's in-loop `idx` advance and
// Lookup's `parent_node_prefix_len - 2` wildcard shortcut: those are what make
// "/home/foo/*" match "/home/foo" but not "/home/fo", and rewriting them
// "cleanly" changes which paths a grant covers.
class RadixTree {
private:
    struct Node {
        std::string prefix{};
        std::map<char, Node*> children{};
        Node* wildcardChild{nullptr};
        bool isLeaf{false};

        explicit Node(std::string pre) : prefix{std::move(pre)} {}
        Node() = default;

        // node: "A node can be an *end* node and have children."
        [[nodiscard]] bool is_end_node() const noexcept {
            if (children.empty()) return true;
            return isLeaf;
        }
    };

    // Nodes live in a deque so references stay stable as it grows; clear()
    // drops the whole arena at once (node deletes recursively — same effect,
    // no ownership bookkeeping to get wrong).
    std::deque<Node> arena_{};
    Node* root_{nullptr};

    Node* make_node_(std::string prefix) {
        arena_.emplace_back(std::move(prefix));
        return &arena_.back();
    }

    Node* create_child_(Node* self, const std::string& pathPrefix) {
        if (pathPrefix.empty()) {
            // node CHECKs here when the node is already a leaf; returning self
            // keeps the shape it would have had rather than aborting the
            // process on a hostile grant list.
            self->isLeaf = true;
            return self;
        }
        const char label{pathPrefix[0]};
        auto it{self->children.find(label)};
        if (it == self->children.end() || it->second == nullptr) {
            Node* fresh{make_node_(pathPrefix)};
            self->children[label] = fresh;
            return fresh;
        }
        Node* child{it->second};

        // swap prefix
        std::size_t i{0};
        const std::size_t prefixLen{pathPrefix.length()};
        for (; i < child->prefix.length(); ++i) {
            const char a{i < prefixLen ? pathPrefix[i] : '\0'};
            if (i > prefixLen || a != child->prefix[i]) {
                const std::string parentPrefix{child->prefix.substr(0, i)};
                const std::string childPrefix{child->prefix.substr(i)};
                child->prefix = childPrefix;
                Node* splitChild{make_node_(parentPrefix)};
                splitChild->children[childPrefix[0]] = child;
                self->children[parentPrefix[0]] = splitChild;
                return create_child_(splitChild, pathPrefix.substr(i));
            }
        }
        child->isLeaf = true;
        return create_child_(child, pathPrefix.substr(i));
    }

    Node* create_wildcard_child_(Node* self) {
        if (self->wildcardChild != nullptr) return self->wildcardChild;
        arena_.emplace_back();
        self->wildcardChild = &arena_.back();
        return self->wildcardChild;
    }

    static Node* next_node_(const Node* self, const std::string& path, std::size_t idx) {
        if (idx >= path.length()) return nullptr;

        // wildcard node takes precedence
        if (self->children.size() > 1) {
            auto w{self->children.find('*')};
            if (w != self->children.end()) return w->second;
        }

        auto it{self->children.find(path[idx])};
        if (it == self->children.end()) return nullptr;
        Node* child{it->second};

        // match prefix
        const std::size_t prefixLen{child->prefix.length()};
        for (std::size_t i{0}; i < path.length(); ++i) {
            if (i >= prefixLen || child->prefix[i] == '*') return child;

            // Handle optional trailing (node's comment): path=/home/subdirectory
            // against child=subdirectory/*.
            if (idx >= path.length() && child->prefix[i] == '/') continue;

            if (path[idx++] != child->prefix[i]) return nullptr;
        }
        return child;
    }

public:
    RadixTree() { root_ = make_node_(""); }
    RadixTree(const RadixTree&) = delete;
    RadixTree& operator=(const RadixTree&) = delete;

    void clear() {
        arena_.clear();
        root_ = make_node_("");
    }

    void insert(const std::string& path) {
        Node* current{root_};
        std::size_t parentPrefixLen{current->prefix.length()};
        const std::size_t pathLen{path.length()};

        for (std::size_t i{1}; i <= pathLen; ++i) {
            const bool isWildcardNode{path[i - 1] == '*'};
            const bool isLastChar{i == pathLen};

            if (isWildcardNode || isLastChar) {
                // node passes `i` as the substr COUNT (not i - parentPrefixLen);
                // substr clamps, so the node's prefix is the remaining tail.
                current = create_child_(current, path.substr(parentPrefixLen, i));
            }
            if (isWildcardNode) {
                current = create_wildcard_child_(current);
                parentPrefixLen = i;
            }
        }
    }

    [[nodiscard]] bool lookup(std::string_view s) const { return lookup(s, false); }

    [[nodiscard]] bool lookup(std::string_view s, bool whenEmptyReturn) const {
        const Node* current{root_};
        if (current->children.empty()) return whenEmptyReturn;
        std::size_t parentPrefixLen{current->prefix.length()};
        const std::string path{s};
        const std::size_t pathLen{path.length()};

        while (true) {
            if (parentPrefixLen == pathLen && current->is_end_node()) return true;

            const Node* node{next_node_(current, path, parentPrefixLen)};
            if (node == nullptr) return false;

            current = node;
            parentPrefixLen += current->prefix.length();
            if (current->wildcardChild != nullptr && pathLen >= (parentPrefixLen - 2)) {
                return true;
            }
        }
    }
};

// ── the model ────────────────────────────────────────────────────────────────
// One instance per process (the runtime owns it). Every method takes already
// UTF-8 paths; resolution against the CURRENT cwd happens here, because node
// resolves at each call (env->GetCwd()) and process.chdir() must therefore move
// what a relative reference means.
class Model {
private:
    bool enabled_{false};
    bool warningOnly_{false};

    // fs (node FSPermission)
    RadixTree grantedIn_{};
    RadixTree grantedOut_{};
    std::vector<std::string> grantedPathsIn_{};
    std::vector<std::string> grantedPathsOut_{};
    bool denyAllIn_{true};
    bool denyAllOut_{true};
    bool allowAllIn_{false};
    bool allowAllOut_{false};

    // single-state scopes. `true` = denied (node's ChildProcessPermission
    // deny_all_); Net is inverted (allow_net_), matching node exactly.
    bool denyChild_{false};
    bool denyWasi_{false};
    bool denyWorker_{false};
    bool denyInspector_{false};
    bool denyAddon_{false};
    bool denyFfi_{false};
    bool allowNet_{false};

    // Injected so tests can drive the model without touching the real cwd or
    // filesystem; the runtime binds these to std::filesystem.
    std::function<std::string()> cwd_{};
    std::function<bool(const std::string&)> isDir_{};

    [[nodiscard]] std::string resolve_(std::string_view p) const {
        return path_resolve(cwd_ ? cwd_() : std::string{}, p);
    }

    // node fs_permission.cc WildcardIfDir — a directory grant covers its subtree.
    [[nodiscard]] std::string wildcard_if_dir_(const std::string& res) const {
        if (isDir_ && isDir_(res)) {
            if (!res.empty() && res.back() == '/') return res + "*";
            return res + "/*";
        }
        return res;
    }

    void grant_access_(Scope perm, const std::string& res) {
        const std::string path{wildcard_if_dir_(res)};
        if (perm == Scope::FileSystemRead && !grantedIn_.lookup(path)) {
            grantedIn_.insert(path);
            grantedPathsIn_.push_back(path);
            denyAllIn_ = false;
        } else if (perm == Scope::FileSystemWrite && !grantedOut_.lookup(path)) {
            grantedOut_.insert(path);
            grantedPathsOut_.push_back(path);
            denyAllOut_ = false;
        }
    }

    void rebuild_tree_(Scope scope) {
        if (scope == Scope::FileSystemRead) {
            grantedIn_.clear();
            if (grantedPathsIn_.empty()) {
                denyAllIn_ = true;
            } else {
                for (const auto& p : grantedPathsIn_) grantedIn_.insert(p);
            }
        } else if (scope == Scope::FileSystemWrite) {
            grantedOut_.clear();
            if (grantedPathsOut_.empty()) {
                denyAllOut_ = true;
            } else {
                for (const auto& p : grantedPathsOut_) grantedOut_.insert(p);
            }
        }
    }

    void revoke_access_(Scope perm, const std::string& res) {
        const std::string path{wildcard_if_dir_(res)};
        if (perm == Scope::FileSystemRead) {
            auto it{std::ranges::find(grantedPathsIn_, path)};
            if (it != grantedPathsIn_.end()) {
                grantedPathsIn_.erase(it);
                rebuild_tree_(Scope::FileSystemRead);
            }
        } else if (perm == Scope::FileSystemWrite) {
            auto it{std::ranges::find(grantedPathsOut_, path)};
            if (it != grantedPathsOut_.end()) {
                grantedPathsOut_.erase(it);
                rebuild_tree_(Scope::FileSystemWrite);
            }
        }
    }

public:
    Model() = default;

    void set_cwd_provider(std::function<std::string()> f) { cwd_ = std::move(f); }
    void set_is_dir_probe(std::function<bool(const std::string&)> f) { isDir_ = std::move(f); }

    void enable() { enabled_ = true; }
    void enable_warning_only() { warningOnly_ = true; }
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    [[nodiscard]] bool warning_only() const noexcept { return warningOnly_; }

    // node Permission::Apply → the per-scope PermissionBase::Apply.
    void apply(Scope scope, std::span<const std::string> allow) {
        switch (scope) {
            case Scope::FileSystemRead:
            case Scope::FileSystemWrite:
                for (const std::string& res : allow) {
                    if (res == "*") {
                        if (scope == Scope::FileSystemRead) {
                            denyAllIn_ = false;
                            allowAllIn_ = true;
                        } else {
                            denyAllOut_ = false;
                            allowAllOut_ = true;
                        }
                        return;
                    }
                    grant_access_(scope, resolve_(res));
                }
                return;
            // Apply == deny for these (env.cc applies them when the flag is absent).
            case Scope::ChildProcess:  denyChild_ = true; return;
            case Scope::Wasi:          denyWasi_ = true; return;
            case Scope::WorkerThreads: denyWorker_ = true; return;
            case Scope::Inspector:     denyInspector_ = true; return;
            case Scope::Addon:         denyAddon_ = true; return;
            case Scope::Ffi:           denyFfi_ = true; return;
            // Net is the exception: Apply == allow.
            case Scope::Net:           allowNet_ = true; return;
            default:                   return;
        }
    }

    // node Permission::is_scope_granted / FSPermission::is_granted.
    // `ref` empty means "the whole scope", which for fs means "was * granted".
    [[nodiscard]] bool is_granted(Scope scope, std::string_view ref = {}) const {
        switch (scope) {
            case Scope::FileSystem:
                return allowAllIn_ && allowAllOut_;
            case Scope::FileSystemRead:
                if (ref.empty()) return allowAllIn_;
                return !denyAllIn_ &&
                       (allowAllIn_ || grantedIn_.lookup(resolve_(ref), true));
            case Scope::FileSystemWrite:
                if (ref.empty()) return allowAllOut_;
                return !denyAllOut_ &&
                       (allowAllOut_ || grantedOut_.lookup(resolve_(ref), true));
            case Scope::ChildProcess:  return !denyChild_;
            case Scope::Wasi:          return !denyWasi_;
            case Scope::WorkerThreads: return !denyWorker_;
            case Scope::Inspector:     return !denyInspector_;
            case Scope::Addon:         return !denyAddon_;
            case Scope::Ffi:           return !denyFfi_;
            case Scope::Net:           return allowNet_;
            case Scope::Root:          return false;
        }
        return false;
    }

    // node Permission::Drop → FSPermission::Drop and friends.
    void drop(Scope scope, std::string_view ref = {}) {
        if (scope == Scope::FileSystem || scope == Scope::FileSystemRead ||
            scope == Scope::FileSystemWrite) {
            if (ref.empty()) {
                if (scope == Scope::FileSystemRead || scope == Scope::FileSystem) {
                    denyAllIn_ = true;
                    allowAllIn_ = false;
                    grantedIn_.clear();
                    grantedPathsIn_.clear();
                }
                if (scope == Scope::FileSystemWrite || scope == Scope::FileSystem) {
                    denyAllOut_ = true;
                    allowAllOut_ = false;
                    grantedOut_.clear();
                    grantedPathsOut_.clear();
                }
                return;
            }
            // "When allowed with *, you can only drop *" — a path drop against a
            // wildcard grant is deliberately a no-op (test-permission-drop-fs-read).
            const std::string resolved{resolve_(ref)};
            if ((scope == Scope::FileSystemRead || scope == Scope::FileSystem) && !allowAllIn_) {
                revoke_access_(Scope::FileSystemRead, resolved);
            }
            if ((scope == Scope::FileSystemWrite || scope == Scope::FileSystem) && !allowAllOut_) {
                revoke_access_(Scope::FileSystemWrite, resolved);
            }
            return;
        }
        switch (scope) {
            case Scope::ChildProcess:  denyChild_ = true; return;
            case Scope::Wasi:          denyWasi_ = true; return;
            case Scope::WorkerThreads: denyWorker_ = true; return;
            case Scope::Inspector:     denyInspector_ = true; return;
            case Scope::Addon:         denyAddon_ = true; return;
            case Scope::Ffi:           denyFfi_ = true; return;
            case Scope::Net:           allowNet_ = false; return;
            default:                   return;
        }
    }
};

}  // namespace mbun::permission
