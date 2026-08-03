// test_watcher_inotify.cpp — drives the live Linux inotify FsWatcher against a
// real temporary directory: create / modify / delete files, mkdir + recursive
// subdir tracking, moves, and a file-target watch. Every wait is bounded by a
// poll timeout with an overall deadline (no bare sleeps, no unbounded blocking)
// so the suite fails fast rather than hanging when an event never arrives.

import std;
import mbun.watcher;

namespace {

int failed{0};

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failed;
    }
}

using namespace mbun::watcher;

// Poll the watcher (bounded slices) until `pred` matches a delivered event or
// the overall deadline elapses. Returns the matching event, or nullopt.
std::optional<FsEvent> wait_for(FsWatcher& w,
                                const std::function<bool(const FsEvent&)>& pred,
                                int deadline_ms = 4000) {
    const auto start = std::chrono::steady_clock::now();
    while (true) {
        for (const auto& ev : w.poll(200)) {
            if (pred(ev)) {
                return ev;
            }
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - start)
                                 .count();
        if (elapsed >= deadline_ms) {
            return std::nullopt;
        }
    }
}

std::string make_tempdir() {
    static std::mt19937_64 rng{std::random_device{}()};
    for (int attempt{0}; attempt < 32; ++attempt) {
        const auto name = std::format("mbun-watch-{:016x}", rng());
        const auto path = std::filesystem::temp_directory_path() / name;
        std::error_code ec;
        if (std::filesystem::create_directory(path, ec) && !ec) {
            return path.string();
        }
    }
    std::cerr << "FAIL: could not create a temp dir\n";
    ++failed;
    return {};
}

void write_file(const std::filesystem::path& p, std::string_view contents) {
    std::ofstream out{p, std::ios::binary | std::ios::trunc};
    out << contents;
    out.flush();
    out.close();
}

}  // namespace

int main() {
    expect(fs_watcher_available(), "inotify backend compiled on this platform");
    if (!fs_watcher_available()) {
        return failed == 0 ? 0 : 1;
    }

    // ── directory watch (non-recursive): create / modify / delete a file ─────
    {
        const std::string dir = make_tempdir();
        FsWatcher w;
        auto started = w.watch(dir, /*recursive=*/false);
        expect(started.has_value(), "watch(dir, non-recursive) starts");
        expect(w.valid(), "watcher fd valid after watch");

        write_file(std::filesystem::path{dir} / "a.txt", "hello");
        auto create = wait_for(w, [](const FsEvent& e) {
            return e.name == "a.txt" && has_op(e.op, EventOp::Create);
        });
        expect(create.has_value(), "create a.txt delivers a Create event");
        if (create) {
            expect(create->kind == ChangeKind::Rename, "create maps to node 'rename'");
        }

        write_file(std::filesystem::path{dir} / "a.txt", "hello world again");
        auto modify = wait_for(w, [](const FsEvent& e) {
            return e.name == "a.txt" && has_op(e.op, EventOp::Write);
        });
        expect(modify.has_value(), "modify a.txt delivers a Write event");
        if (modify) {
            expect(modify->kind == ChangeKind::Change, "modify maps to node 'change'");
        }

        std::filesystem::remove(std::filesystem::path{dir} / "a.txt");
        auto del = wait_for(w, [](const FsEvent& e) {
            return e.name == "a.txt" && has_op(e.op, EventOp::Delete);
        });
        expect(del.has_value(), "delete a.txt delivers a Delete event");
        if (del) {
            expect(del->kind == ChangeKind::Rename, "delete maps to node 'rename'");
        }

        w.close();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    // ── rename within a watched directory → two rename events ────────────────
    {
        const std::string dir = make_tempdir();
        FsWatcher w;
        expect(w.watch(dir, false).has_value(), "watch(dir) for rename test");
        write_file(std::filesystem::path{dir} / "old.txt", "x");
        (void)wait_for(w, [](const FsEvent& e) { return e.name == "old.txt"; });

        std::error_code ec;
        std::filesystem::rename(std::filesystem::path{dir} / "old.txt",
                                std::filesystem::path{dir} / "new.txt", ec);
        expect(!ec, "rename old.txt → new.txt on disk");
        auto moved_to = wait_for(w, [](const FsEvent& e) {
            return e.name == "new.txt" && has_op(e.op, EventOp::MoveTo);
        });
        expect(moved_to.has_value(), "rename target delivers a MoveTo event");
        if (moved_to) {
            expect(moved_to->kind == ChangeKind::Rename, "moved-to maps to 'rename'");
        }

        w.close();
        std::filesystem::remove_all(dir, ec);
    }

    // ── recursive watch: new subdir is tracked, nested file surfaces w/ rel path
    {
        const std::string dir = make_tempdir();
        FsWatcher w;
        expect(w.watch(dir, /*recursive=*/true).has_value(),
               "watch(dir, recursive) starts");
        const std::size_t roots = w.watch_count();
        expect(roots >= 1, "recursive watch attaches at least the root wd");

        std::error_code ec;
        std::filesystem::create_directory(std::filesystem::path{dir} / "sub", ec);
        auto subdir = wait_for(w, [](const FsEvent& e) {
            return e.name == "sub" && e.is_dir && has_op(e.op, EventOp::Create);
        });
        expect(subdir.has_value(), "mkdir sub delivers a Create(dir) event");
        // processing that event attaches a wd to 'sub'.
        expect(w.watch_count() > roots, "new subdir is now watched");

        write_file(std::filesystem::path{dir} / "sub" / "nested.txt", "deep");
        auto nested = wait_for(w, [](const FsEvent& e) {
            return e.name == "sub/nested.txt" && has_op(e.op, EventOp::Create);
        });
        expect(nested.has_value(), "file in new subdir surfaces with rel path sub/nested.txt");

        w.close();
        std::filesystem::remove_all(dir, ec);
    }

    // ── file-target watch: modifying the watched file emits a Write/change ────
    {
        const std::string dir = make_tempdir();
        const auto file = std::filesystem::path{dir} / "watched.txt";
        write_file(file, "initial");

        FsWatcher w;
        expect(w.watch(file.string(), false).has_value(), "watch(file) starts");

        write_file(file, "changed contents");
        auto ev = wait_for(w, [](const FsEvent& e) {
            return has_op(e.op, EventOp::Write) && e.kind == ChangeKind::Change;
        });
        expect(ev.has_value(), "modifying watched file emits a Write/change event");
        if (ev) {
            expect(ev->name == "watched.txt", "file event reports basename");
        }

        w.close();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    // ── change_kind_name mapping is stable (node event-type strings) ──────────
    expect(change_kind_name(ChangeKind::Rename) == "rename", "kind name: rename");
    expect(change_kind_name(ChangeKind::Change) == "change", "kind name: change");

    if (failed != 0) {
        std::cerr << failed << " check(s) failed\n";
        return 1;
    }
    std::cout << "test_watcher_inotify ... ok\n";
    return 0;
}
