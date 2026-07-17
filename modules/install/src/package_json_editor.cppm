// package_json_editor.cppm — the write-side of `mbun add`'s package.json edit.
//
// Blueprint: bun-ref/src/install/PackageManager/PackageJSONEditor.rs (`edit`),
// driven from bun-ref/src/install/PackageManager/updatePackageJSONAndInstall.rs
// :304-318 (Subcommand::Add → `PackageJSONEditor::edit(.., EditOptions {
// exact_versions, before_install: true, .. })`).
//
// bun edits the package.json *twice* per `add`: once before install with the
// requested literal, then again after resolution with the concrete version
// (updatePackageJSONAndInstall.rs:569 / PackageJSONEditor.rs:1204-1226 write
// `^` + the resolved version unless `--exact`). mbun resolves first and edits
// once — the file only ever lands on disk in the post-resolution state, so the
// observable result matches while skipping the intermediate write.
//
// The layout rules replicated here are load-bearing (regression/issue/00631
// asserts the exact bytes), and all of them fall out of *which nodes bun
// rebuilds*:
//   - A rebuilt node is `E::Object { properties, ..Default::default() }`, and
//     `Default` means `is_single_line: false` → it re-prints multi-line even if
//     it was parsed from a single-line source (PackageJSONEditor.rs:1024-1030,
//     :1058-1064 for the root; :896-900 for a freshly created dependency list).
//   - A *reused* node keeps the `is_single_line` the parser gave it, so the
//     root only flips to multi-line when a dependency list is actually added
//     (the `needs_new_dependency_list` guards at :996 / :1031).
export module mbun.install.package_json_editor;

import std;
import mbun.install.npm.json;

namespace mbun::install::package_json_editor {

namespace json = mbun::install::npm::json;

// One dependency to write into the list. `version` is the final literal
// ("^1.3.0" / "1.3.0" / "latest"), already resolved by the caller.
export struct NewDependency {
    std::string name;
    std::string version;
};

// The dependency list a request targets.
//   ref: bun-ref/src/install/PackageManager/CommandLineArguments.rs:199-208
//        (-d/--dev, --optional, --peer) and the `dependency_list` argument
//        threaded into `PackageJSONEditor::edit`.
export enum class List : std::uint8_t { Dependencies, DevDependencies, OptionalDependencies, PeerDependencies };

export constexpr std::string_view list_name(List list) {
    switch (list) {
        case List::DevDependencies: return "devDependencies";
        case List::OptionalDependencies: return "optionalDependencies";
        case List::PeerDependencies: return "peerDependencies";
        case List::Dependencies: break;
    }
    return "dependencies";
}

namespace detail {

// Park a synthesized string in the Document's owned pool and hand back a view.
// `ownedStrings` is a std::deque, so existing views stay valid across pushes.
std::string_view intern(json::Document& doc, std::string_view s) {
    return doc.ownedStrings.emplace_back(s);
}

json::Value* find_member(json::Value& object, std::string_view key) {
    for (auto& m : object.members) {
        if (m.key == key) {
            return m.value.get();
        }
    }
    return nullptr;
}

}  // namespace detail

// Insert (or update) `deps` into `list` of the package.json `doc`.
//
// ref: PackageJSONEditor.rs:887-1066. Mirrors bun's structure exactly:
//   1. Reuse the existing dependency list when it is an object, else build a
//      fresh one (:887-903, `needs_new_dependency_list`).
//   2. Fill in the requests, then alphabetize when the list has >1 entry
//      (:906-911 → `alphabetize_properties`, a stable bytewise sort on the key:
//      ast/e.rs:1622-1635 + :1724-1741 `object_sorter_is_less_than` → `a.cmp(&b)`).
//   3. Attach a *new* list to the root, rebuilding the root so it re-prints
//      multi-line (:1024-1030). A root that is not an object, or is an empty
//      object, is replaced outright by one holding just the list (:1063-1092).
export void edit(json::Document& doc, List list, std::span<const NewDependency> deps) {
    if (deps.empty()) {
        return;
    }
    const std::string_view listName{list_name(list)};

    if (!doc.root) {
        doc.root = std::make_unique<json::Value>();
    }
    json::Value& root{*doc.root};

    // ref: :1063-1069 — a non-object or empty-object root is replaced wholesale.
    const bool rebuildRoot{root.kind != json::Kind::Object || root.members.empty()};
    if (rebuildRoot) {
        root.kind = json::Kind::Object;
        root.members.clear();
        root.items.clear();
        root.singleLine = false;  // E::Object { .. ..Default::default() }
    }

    // ── 1. the dependency list ──────────────────────────────────────────────
    json::Value* listObject{detail::find_member(root, listName)};
    bool needsNewList{listObject == nullptr || listObject->kind != json::Kind::Object};
    std::unique_ptr<json::Value> freshList;
    if (needsNewList) {
        // ref: :896-900 — `E::Object { properties: vec![], ..Default::default() }`,
        // i.e. singleLine=false → the new list prints multi-line.
        freshList = std::make_unique<json::Value>();
        freshList->kind = json::Kind::Object;
        listObject = freshList.get();
    }

    // ── 2. fill in the requests, then alphabetize ───────────────────────────
    for (const auto& dep : deps) {
        const std::string_view version{detail::intern(doc, dep.version)};
        if (json::Value* existing{detail::find_member(*listObject, dep.name)}) {
            existing->kind = json::Kind::String;
            existing->str = version;
            continue;
        }
        auto value{std::make_unique<json::Value>()};
        value->kind = json::Kind::String;
        value->str = version;
        listObject->members.push_back(json::Member{detail::intern(doc, dep.name), std::move(value)});
    }
    if (listObject->members.size() > 1) {
        std::ranges::stable_sort(listObject->members, std::ranges::less{}, &json::Member::key);
    }

    // ── 3. attach a new list to the root ────────────────────────────────────
    if (needsNewList) {
        root.members.push_back(json::Member{detail::intern(doc, listName), std::move(freshList)});
        // ref: :1024-1030 / :1058-1064 — bun re-allocates the root as
        // `E::Object { properties, ..Default::default() }` when it appends a
        // dependency list, which resets is_single_line to false. Only the flag
        // is observable, so flip it in place rather than rebuilding the node.
        root.singleLine = false;
    }
}

}  // namespace mbun::install::package_json_editor
