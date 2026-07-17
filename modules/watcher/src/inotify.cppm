// inotify.cppm — mbun.watcher.inotify: a real Linux inotify(7) filesystem
// watcher exposing node:fs.watch semantics (watch(path, recursive) → an event
// stream of create/modify/delete/rename/move with the changed path).
//
// This is the node fs.watch model, not the bundler watchlist model: each
// FsWatcher owns its own inotify fd and maps every watch descriptor (wd) to the
// directory/file it observes. Recursive watches walk the subtree at subscribe
// time and add a wd per directory, then attach a wd to each new subdirectory as
// it appears (IN_CREATE|IN_ISDIR / IN_MOVED_TO|IN_ISDIR) — exactly how bun's
// path_watcher tracks structure changes after the initial crawl.
//
// Event translation mirrors two bun references so behaviour stays aligned:
//   • rich EventOp bits  ← Watcher.rs::watch_event_from_inotify_event
//   • node ChangeKind    ← path_watcher.rs (rename vs change)
// Per-(path,kind) 1ms duplicate suppression matches path_watcher's ChangeEvent
// coalescing; the ppoll-style coalesce window batches a burst into one drain.
//
// PORT-SOURCE: .mbun/bun-ref/src/runtime/node/path_watcher.rs (Linux inotify path)
//              .mbun/bun-ref/src/watcher/INotifyWatcher.rs   (mask→op, read/coalesce)
module;

#if defined(__linux__)
#include <cerrno>
#include <poll.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

export module mbun.watcher.inotify;

import std;
import mbun.watcher.event;
import mbun.watcher.state;
import mbun.watcher.backend;

namespace mbun::watcher {

// node:fs.watch surfaces exactly two event types.
export enum class ChangeKind : std::uint8_t { Rename, Change };

export constexpr std::string_view change_kind_name(ChangeKind kind) noexcept {
    return kind == ChangeKind::Rename ? std::string_view{"rename"}
                                      : std::string_view{"change"};
}

// One delivered filesystem event. `name` is relative to the watch root
// (empty == the root itself); `op` carries the rich bits, `kind` the node type.
export struct FsEvent {
    EventOp op{EventOp::None};
    ChangeKind kind{ChangeKind::Change};
    std::string name;
    bool is_dir{false};
};

// Whether the live inotify backend is compiled in on this platform.
export inline constexpr bool fs_watcher_available() noexcept {
#if defined(__linux__)
    return true;
#else
    return false;
#endif
}

#if defined(__linux__)

export class FsWatcher {
private:
    struct WatchNode {
        std::string subpath;  // dir/file path relative to root_ ("" == root)
        bool is_dir{true};
    };

    // Per-(path,kind) suppression window, mirroring path_watcher::ChangeEvent.
    struct ChangeEvent {
        HashType hash{};
        ChangeKind kind{ChangeKind::Change};
        std::int64_t timestamp{};
    };

    int fd_{-1};
    std::string root_;
    bool recursive_{false};
    bool root_is_file_{false};
    std::unordered_map<int, WatchNode> nodes_;
    ChangeEvent last_{};

    static constexpr std::uint32_t WATCH_FILE_MASK{
        IN_MODIFY | IN_ATTRIB | IN_MOVE_SELF | IN_DELETE_SELF};
    static constexpr std::uint32_t WATCH_DIR_MASK{
        IN_MODIFY | IN_ATTRIB | IN_CREATE | IN_DELETE | IN_DELETE_SELF
        | IN_MOVED_FROM | IN_MOVED_TO | IN_MOVE_SELF | IN_ONLYDIR};

    static constexpr int COALESCE_MS{25};
    static constexpr int MAX_COALESCE_ROUNDS{64};

public:
    FsWatcher() = default;
    FsWatcher(const FsWatcher&) = delete;
    FsWatcher& operator=(const FsWatcher&) = delete;
    FsWatcher(FsWatcher&& other) noexcept { *this = std::move(other); }
    FsWatcher& operator=(FsWatcher&& other) noexcept {
        if (this != &other) {
            close();
            fd_ = std::exchange(other.fd_, -1);
            root_ = std::move(other.root_);
            recursive_ = other.recursive_;
            root_is_file_ = other.root_is_file_;
            nodes_ = std::move(other.nodes_);
            last_ = other.last_;
        }
        return *this;
    }
    ~FsWatcher() { close(); }

    // Begin watching `path`. A directory root with `recursive` also crawls and
    // attaches a wd to every existing subdirectory. Returns the errno-tagged
    // failure of inotify_init1 / the root inotify_add_watch on error.
    [[nodiscard]] std::expected<void, BackendError> watch(std::string_view path,
                                                          bool recursive) {
        if (fd_ < 0) {
            fd_ = ::inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
            if (fd_ < 0) {
                return std::unexpected(errno_("inotify_init1"));
            }
        }
        root_ = normalize_(path);
        recursive_ = recursive;
        root_is_file_ = !is_directory_(root_);

        auto added = add_one_(root_, "", !root_is_file_);
        if (!added) {
            return std::unexpected(added.error());
        }
        if (recursive_ && !root_is_file_) {
            walk_add_(root_, "", nullptr);
        }
        return {};
    }

    // Drain events, waiting up to `timeout_ms` for the first, then batching a
    // short coalesce window. Bounded: never blocks past timeout + coalesce.
    [[nodiscard]] std::vector<FsEvent> poll(int timeout_ms) {
        std::vector<FsEvent> out;
        if (fd_ < 0) {
            return out;
        }
        if (!wait_readable_(timeout_ms)) {
            return out;
        }
        drain_(out);
        for (int round{0}; round < MAX_COALESCE_ROUNDS && wait_readable_(COALESCE_MS);
             ++round) {
            drain_(out);
        }
        return out;
    }

    void close() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        nodes_.clear();
    }

    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    [[nodiscard]] std::size_t watch_count() const noexcept { return nodes_.size(); }
    [[nodiscard]] const std::string& root() const noexcept { return root_; }

private:
    static BackendError errno_(std::string_view syscall) {
        const int code{errno};
        return BackendError{code, std::format("{}: {}", syscall, std::strerror(code))};
    }

    static std::string normalize_(std::string_view path) {
        std::string p{path};
        while (p.size() > 1 && p.back() == '/') {
            p.pop_back();
        }
        return p;
    }

    static bool is_directory_(const std::string& path) {
        struct ::stat st{};
        if (::stat(path.c_str(), &st) != 0) {
            return false;
        }
        return S_ISDIR(st.st_mode);
    }

    static std::string join_(std::string_view a, std::string_view b) {
        if (a.empty()) {
            return std::string{b};
        }
        if (b.empty()) {
            return std::string{a};
        }
        std::string out{a};
        out.push_back('/');
        out.append(b);
        return out;
    }

    static std::string_view basename_(std::string_view path) {
        const auto slash = path.find_last_of('/');
        return slash == std::string_view::npos ? path : path.substr(slash + 1);
    }

    static std::int64_t now_ms_() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    }

    // inotify_add_watch + record ownership. During a recursive crawl a racing
    // ENOENT/ENOTDIR is benign (the entry vanished) and skipped.
    [[nodiscard]] std::expected<void, BackendError> add_one_(const std::string& absPath,
                                                             std::string_view subpath,
                                                             bool isDir) {
        const std::uint32_t mask{(root_is_file_ && subpath.empty()) ? WATCH_FILE_MASK
                                                                    : WATCH_DIR_MASK};
        const int wd{::inotify_add_watch(fd_, absPath.c_str(), mask)};
        if (wd < 0) {
            if (!subpath.empty()) {
                return {};  // raced during walk; skip
            }
            return std::unexpected(errno_("inotify_add_watch"));
        }
        // inotify returns the same wd for the same inode; overwrite keeps the
        // subpath current after a moved/renamed directory re-attaches.
        nodes_[wd] = WatchNode{std::string{subpath}, isDir};
        return {};
    }

    // Best-effort subtree crawl: attach a wd to every subdirectory. When
    // `emitInto` is set, synthesize a rename for each discovered entry — a newly
    // created directory's pre-existing children never fire their own IN_CREATE.
    void walk_add_(const std::string& absDir, const std::string& relDir,
                   std::vector<FsEvent>* emitInto) {
        std::error_code ec;
        std::filesystem::directory_iterator it{absDir, ec};
        if (ec) {
            return;
        }
        for (const auto& entry : it) {
            std::error_code isDirEc;
            const bool isDir{entry.is_directory(isDirEc)};
            const std::string name{entry.path().filename().string()};
            const std::string rel{join_(relDir, name)};
            if (emitInto != nullptr) {
                emit_(*emitInto, EventOp::Create, ChangeKind::Rename, rel, isDir);
            }
            if (isDir && !isDirEc) {
                (void)add_one_(entry.path().string(), rel, true);
                walk_add_(entry.path().string(), rel, emitInto);
            }
        }
    }

    bool wait_readable_(int timeout_ms) {
        struct ::pollfd pfd{};
        pfd.fd = fd_;
        pfd.events = POLLIN;
        for (;;) {
            const int rc{::poll(&pfd, 1, timeout_ms)};
            if (rc < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }
            return rc > 0 && (pfd.revents & POLLIN) != 0;
        }
    }

    void drain_(std::vector<FsEvent>& out) {
        alignas(alignof(struct ::inotify_event)) std::array<char, 64 * 1024> buf{};
        for (;;) {
            const ::ssize_t n{::read(fd_, buf.data(), buf.size())};
            if (n <= 0) {
                if (n < 0 && errno == EINTR) {
                    continue;
                }
                return;  // EAGAIN (queue drained) or error/EOF
            }
            std::size_t i{0};
            while (i + sizeof(struct ::inotify_event) <= static_cast<std::size_t>(n)) {
                const auto* ev = reinterpret_cast<const struct ::inotify_event*>(buf.data() + i);
                i += sizeof(struct ::inotify_event) + ev->len;
                handle_event_(out, *ev);
            }
        }
    }

    void handle_event_(std::vector<FsEvent>& out, const struct ::inotify_event& ev) {
        if ((ev.mask & IN_Q_OVERFLOW) != 0) {
            out.push_back(FsEvent{EventOp::None, ChangeKind::Change, std::string{}, false});
            return;
        }
        if ((ev.mask & IN_IGNORED) != 0) {
            nodes_.erase(ev.wd);  // wd retired by the kernel
            return;
        }
        const auto node_it = nodes_.find(ev.wd);
        if (node_it == nodes_.end()) {
            return;
        }
        const std::string subpath{node_it->second.subpath};

        std::string name;
        if (ev.len > 0) {
            name = std::string{ev.name};  // kernel NUL-pads within len
        }
        const bool isDirChild{(ev.mask & IN_ISDIR) != 0};

        // Path relative to the watch root.
        std::string rel;
        if (root_is_file_) {
            rel = std::string{basename_(root_)};
        } else if (subpath.empty()) {
            rel = (name.empty() && !recursive_) ? std::string{basename_(root_)} : name;
        } else {
            rel = name.empty() ? subpath : join_(subpath, name);
        }

        emit_(out, op_from_mask_(ev.mask), kind_from_mask_(ev.mask), rel, isDirChild);

        // Recursive: a directory appeared under the tree — start watching it and
        // synthesize renames for anything already inside it.
        if (recursive_ && isDirChild && ((ev.mask & (IN_CREATE | IN_MOVED_TO)) != 0)
            && !name.empty()) {
            const std::string childAbs{join_(join_(root_, subpath), name)};
            (void)add_one_(childAbs, rel, true);
            walk_add_(childAbs, rel, &out);
        }
    }

    // Rich op bits — Watcher.rs::watch_event_from_inotify_event, plus Metadata
    // for IN_ATTRIB (the dir/file masks watch it).
    static EventOp op_from_mask_(std::uint32_t mask) noexcept {
        EventOp op{EventOp::None};
        if ((mask & (IN_DELETE_SELF | IN_DELETE)) != 0) op |= EventOp::Delete;
        if ((mask & IN_MOVE_SELF) != 0) op |= EventOp::Rename;
        if ((mask & IN_MOVED_TO) != 0) op |= EventOp::MoveTo;
        if ((mask & IN_MOVED_FROM) != 0) op |= EventOp::Rename;
        if ((mask & IN_MODIFY) != 0) op |= EventOp::Write;
        if ((mask & IN_CREATE) != 0) op |= EventOp::Create;
        if ((mask & IN_ATTRIB) != 0) op |= EventOp::Metadata;
        return op;
    }

    // node event type — path_watcher.rs: structural changes are "rename",
    // content/metadata changes are "change".
    static ChangeKind kind_from_mask_(std::uint32_t mask) noexcept {
        constexpr std::uint32_t rename_bits{IN_CREATE | IN_DELETE | IN_DELETE_SELF
                                            | IN_MOVE_SELF | IN_MOVED_FROM | IN_MOVED_TO};
        return (mask & rename_bits) != 0 ? ChangeKind::Rename : ChangeKind::Change;
    }

    // 1ms per-(path,kind) suppression, matching path_watcher::ChangeEvent.
    void emit_(std::vector<FsEvent>& out, EventOp op, ChangeKind kind,
               const std::string& name, bool isDir) {
        const HashType h{hash_path(name)};
        const std::int64_t ts{now_ms_()};
        const bool fresh{last_.timestamp == 0 || (ts - last_.timestamp) > 1
                         || last_.kind != kind || last_.hash != h};
        if (!fresh) {
            return;
        }
        last_ = ChangeEvent{h, kind, ts};
        out.push_back(FsEvent{op, kind, name, isDir});
    }
};

#endif  // __linux__

}  // namespace mbun::watcher
