// mbun.app — application orchestration and command implementations.

export module mbun.app;

import std;
import mbun.exe_platform;
import mbun.cli;
// `mbun run <script>` — package.json scripts, node_modules/.bin PATH stitching.
import mbun.cli.run_command;
import mbun.install.command;
import mbun.install.dependency;
import mbun.install.npm.json;
import mbun.install.package_json_editor;
// `bun run --workspaces` — the root package.json `workspaces` glob expansion.
import mbun.install.workspace_map;
import mbun.jsc.module_loader;  // runtime_jsx_options — the JSX config sink
import mbun.jsc.runtime;
import mbun.jsc.test_runner;
import mbun.js_parser;          // detail::JsxOptions / JsxRuntime
import mbun.semver;             // engines.node / engines.bun range judgment
import mbun.toml;
import mbun.bunfig.types;
import mbun.bunfig.parser;
import mbun.http;
// `mbun build` bundles through the same engine Bun.build binds to.
import mbun.bundler;
import mbun.resolver;

export namespace mbun::app {

constexpr std::string_view USAGE = R"(mbun — an experimental rewrite of bun in MC++

Usage: mbun <command> [...flags]

Commands:
  run        Execute a file or package.json script   (mbun run <script|file> [args...])
  test       Run tests with the bun:test runner      (mbun test [...files])
  install    Install dependencies                    (file: folder slice)
  add        Add a dependency                   (mbun add <package>...)
  build      Bundle for production               (mbun build <entrypoint> [...flags])
  exec       Execute a shell script              (mbun exec <script>)

Flags:
  -v, --version  Print version and exit
  -h, --help     Show this message
)";

// A bare path argument that looks like a runnable script (bun-style `bun x.js`).
// Defined later in this TU; used by run_install above their definitions.
std::optional<std::filesystem::path> find_package_json(const std::filesystem::path& start);
void warn_engines_mismatch(const std::filesystem::path& packageJsonPath);

bool looks_like_script(std::string_view s) {
    for (std::string_view ext : {".mjs", ".cjs", ".js", ".mts", ".cts", ".ts", ".jsx", ".tsx"}) {
        if (s.ends_with(ext)) return true;
    }
    return false;
}

// `bun <file.md>` renders markdown to the terminal instead of executing JS.
bool is_markdown(std::string_view s) {
    return s.ends_with(".md") || s.ends_with(".markdown");
}

// JSON-escape a byte string for embedding as a JS string literal.
std::string js_quote(std::string_view s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                constexpr char hex[] = "0123456789abcdef";
                out += "\\u00";
                out += hex[(c >> 4) & 0xf];
                out += hex[c & 0xf];
            } else {
                out += c;
            }
        }
    }
    out += "\"";
    return out;
}

// Render a markdown file to ANSI on stdout via Bun.markdown.ansi. Colors,
// columns, and hyperlinks follow bun's `bun <file.md>` behavior (FORCE_COLOR /
// NO_COLOR / TTY, COLUMNS env, hyperlinks only on a real TTY).
int run_markdown(std::string_view file) {
    std::string code =
        "(function(){const fs=require('fs');const p=" + js_quote(file) + ";"
        "let src;try{src=fs.readFileSync(p,'utf8');}catch(e){"
        "process.stderr.write('error: '+(e&&e.message||e)+'\\n');process.exit(1);}"
        "const env=process.env||{};"
        "const isTTY=!!(process.stdout&&process.stdout.isTTY);"
        "let colors;if(env.FORCE_COLOR!==undefined)colors=true;"
        "else if(env.NO_COLOR!==undefined)colors=false;else colors=isTTY;"
        "const hyperlinks=colors&&isTTY;"
        "let columns=80;if(env.COLUMNS!==undefined){const n=parseInt(env.COLUMNS,10);if(n>0)columns=n;}"
        "else if(isTTY&&process.stdout.columns)columns=process.stdout.columns;"
        "process.stdout.write(Bun.markdown.ansi(src,{colors:colors,columns:columns,hyperlinks:hyperlinks}));"
        "})();";
    std::vector<std::string> jsArgv{"mbun", std::string{file}};
    mbun::jsc::runtime::set_argv(std::move(jsArgv));
    return mbun::jsc::runtime::run_eval(code);
}

// `--preserve-symlinks-main` (run_command.rs:2580) — set from the flag or from
// NODE_PRESERVE_SYMLINKS_MAIN (bun reads both; run_command.rs:2581-2584).
bool gPreserveSymlinksMain{false};

// Resolves the entry point the way bun boots it (run_command.rs:2578-2602): the
// ONE resolve for the entry runs with `preserve_symlinks =
// --preserve-symlinks-main || NODE_PRESERVE_SYMLINKS_MAIN`, which defaults to
// false (resolver/options.rs:278), so a symlinked entry is booted at its REAL
// path (resolver/resolver.rs:6225 `if !self.opts.preserve_symlinks` →
// set_realpath).
//
// This is what makes `node_modules/.bin/*` work at all: every one of them is a
// symlink INTO the package dir (`.bin/mocha -> ../mocha/bin/mocha.js`), and
// mocha's own `require('../lib/cli/options')` must resolve against `mocha/`,
// not `.bin/`. Booting the link path made mbun look for
// `node_modules/lib/cli/options` and fail.
//
// bun also always reports an ABSOLUTE argv[1]/__filename, even for a plain
// (non-symlink) relative entry — verified on bun 1.3.14:
//   bun ./real.js  → argv1=/abs/.../real.js   (mbun previously: "./real.js")
//   bun ./link.js  → argv1=/abs/.../real.js   (the realpath, not the link)
std::string resolve_entry_path(std::string_view script) {
    std::error_code ec{};
    const std::filesystem::path p{script};
    if (!gPreserveSymlinksMain) {
        // canonical() == absolute + symlink resolution.
        if (std::filesystem::path real{std::filesystem::canonical(p, ec)}; !ec) {
            return real.string();
        }
    }
    // preserve-symlinks-main, or a target canonical() cannot stat (bun still
    // hands the resolver an absolute path in that case).
    if (std::filesystem::path abs{std::filesystem::absolute(p, ec)}; !ec) {
        return abs.lexically_normal().string();
    }
    return std::string{script};
}

// Run a JS file with process.argv = [runtime, script, ...args] (Node/bun order).
int run_script(std::string_view script, std::span<const std::string_view> scriptArgs) {
    // Best-effort, NON-FATAL bunfig.toml validation (ref bun
    // src/bunfig/arguments.rs load_config): emit a config error to stderr but
    // still run the script (exit unaffected). Parser + "expected string" type
    // check already live in modules/bunfig; only the run-path wiring was missing.
    {
        std::error_code ec{};
        if (std::filesystem::exists("bunfig.toml", ec)) {
            std::ifstream in{"bunfig.toml", std::ios::binary};
            if (in) {
                std::string src{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
                if (auto root = mbun::toml::parse(src)) {
                    auto cfg = mbun::bunfig::Parser{mbun::bunfig::Command::Run}.parse(*root);
                    if (!cfg) {
                        const auto& e = cfg.error();
                        if (e.key.empty())
                            std::println(std::cerr, "error: {} in bunfig.toml", e.message);
                        else
                            std::println(std::cerr, "error: {} for \"{}\" in bunfig.toml", e.message, e.key);
                    } else {
                        if (cfg->disable_default_env_files) {
                            // bunfig env=false / env.file=false disables .env loading, like
                            // --no-env-file. ref: bun bunfig.rs -> dotenv/env_loader.rs.
                            mbun::jsc::runtime::set_disable_env_files(true);
                        }
                        // `preload = [...]`: imported before the entry point
                        // (jsc_hooks.rs `reload_entry_point`).
                        if (!cfg->preloads.empty()) {
                            mbun::jsc::runtime::set_preloads(cfg->preloads);
                        }
                    }
                }
            }
        }
    }
    if (is_markdown(script)) return run_markdown(script);
    const std::string entry{resolve_entry_path(script)};
    std::vector<std::string> jsArgv;
    jsArgv.reserve(scriptArgs.size() + 2);
    jsArgv.emplace_back("mbun");
    jsArgv.emplace_back(entry);
    for (std::string_view a : scriptArgs) jsArgv.emplace_back(a);
    mbun::jsc::runtime::set_argv(std::move(jsArgv));
    return mbun::jsc::runtime::run_file(entry);
}

// `mbun pm version [args...]` — bump the package.json version like `npm version`.
// Behavior mirrors bun's PmVersionCommand (see .mbun/bun-ref/src/runtime/cli/
// pm_version_command.rs): find nearest package.json walking up, compute the new
// version via semver rules, run pre/version/post lifecycle scripts, optionally
// commit + tag with git. Implemented on top of the JSC runtime (fs +
// child_process) so the semver/git/JSON logic is shared with the JS layer.
int run_pm_version(std::span<const std::string_view> pmArgs) {
    // Inject the CLI operands as a JS array literal, then run the port.
    std::string argsLit = "const __ARGS=[";
    for (std::size_t i{0}; i < pmArgs.size(); ++i) {
        if (i) argsLit += ",";
        argsLit += js_quote(pmArgs[i]);
    }
    argsLit += "];\n";

    static constexpr std::string_view BODY = R"JS(
(function(){
  const fs = require('fs');
  const cp = require('child_process');
  const path = require('path');
  function errln(s){ process.stderr.write('error: ' + s + '\n'); }

  const KNOWN = ['patch','minor','major','prepatch','preminor','premajor','prerelease','from-git'];

  // ---- flag parsing ----
  let gitTagVersion = true;
  let allowSame = false;
  let force = false;
  let ignoreScripts = false;
  let preid = '';
  let message = null;
  const positionals = [];
  for (let i=0;i<__ARGS.length;i++){
    let a = __ARGS[i];
    if (a === '--no-git-tag-version') { gitTagVersion = false; continue; }
    if (a === '--git-tag-version') { gitTagVersion = true; continue; }
    if (a === '--git-tag-version=true') { gitTagVersion = true; continue; }
    if (a === '--git-tag-version=false') { gitTagVersion = false; continue; }
    if (a === '--allow-same-version') { allowSame = true; continue; }
    if (a === '--force' || a === '-f') { force = true; continue; }
    if (a === '--ignore-scripts') { ignoreScripts = true; continue; }
    if (a === '--message' || a === '-m') { message = __ARGS[++i]; continue; }
    if (a.startsWith('--message=')) { message = a.slice('--message='.length); continue; }
    if (a === '--preid') { preid = __ARGS[++i] || ''; continue; }
    if (a.startsWith('--preid=')) { preid = a.slice('--preid='.length); continue; }
    if (a.startsWith('-')) { continue; } // ignore unknown flags
    positionals.push(a);
  }

  // ---- semver ----
  function parseSemver(str){
    const m = /^(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.-]+))?(?:\+([0-9A-Za-z.-]+))?$/.exec(str);
    if (!m) return null;
    return { major:+m[1], minor:+m[2], patch:+m[3], pre: m[4]===undefined?'':m[4] };
  }
  const isDigits = (s)=>/^\d+$/.test(s);

  function incrementVersion(cur, type, id){
    const M=cur.major, m=cur.minor, p=cur.patch;
    switch(type){
      case 'patch': return `${M}.${m}.${p+1}`;
      case 'minor': return `${M}.${m+1}.0`;
      case 'major': return `${M+1}.0.0`;
      case 'prepatch': return id ? `${M}.${m}.${p+1}-${id}.0` : `${M}.${m}.${p+1}-0`;
      case 'preminor': return id ? `${M}.${m+1}.0-${id}.0` : `${M}.${m+1}.0-0`;
      case 'premajor': return id ? `${M+1}.0.0-${id}.0` : `${M+1}.0.0-0`;
      case 'prerelease': {
        if (cur.pre){
          const cp2 = cur.pre;
          const identifier = id ? id : cp2;
          const dot = cp2.lastIndexOf('.');
          if (dot >= 0){
            const num = parseInt(cp2.slice(dot+1),10);
            const n = isNaN(num) ? 0 : num;
            return `${M}.${m}.${p}-${identifier}.${n+1}`;
          } else {
            if (isDigits(cp2)){
              const n = parseInt(cp2,10);
              return id ? `${M}.${m}.${p}-${id}.${n+1}` : `${M}.${m}.${p}-${n+1}`;
            } else {
              return `${M}.${m}.${p}-${identifier}.1`;
            }
          }
        } else {
          return id ? `${M}.${m}.${p+1}-${id}.0` : `${M}.${m}.${p+1}-0`;
        }
      }
    }
    return `${M}.${m}.${p}`;
  }

  function calcNewVersion(currentStr, type, specific, dir){
    if (type === 'specific') return specific;
    if (type === 'from-git') return versionFromGit(dir);
    const cur = parseSemver(currentStr);
    if (!cur){
      errln(`Current version "${currentStr}" is not a valid semver`);
      process.exit(1);
    }
    let id;
    if (preid) id = preid;
    else if (!cur.pre) id = '';
    else {
      const cp2 = cur.pre;
      const dot = cp2.indexOf('.');
      if (dot >= 0) id = cp2.slice(0,dot);
      else if (isDigits(cp2)) id = '';
      else id = cp2;
    }
    return incrementVersion(cur, type, id);
  }

  // ---- locate package.json (walk up) ----
  function findPackageDir(start){
    let dir = start;
    while(true){
      if (fs.existsSync(path.join(dir,'package.json'))) return dir;
      const parent = path.dirname(dir);
      if (parent === dir) break;
      dir = parent;
    }
    return start;
  }

  const startDir = process.cwd();
  const pkgDir = findPackageDir(startDir);
  const pkgPath = path.join(pkgDir, 'package.json');

  // ---- git helpers ----
  function gitClean(dir){
    const r = cp.spawnSync('git',['status','--porcelain'],{cwd:dir,encoding:'utf8'});
    return r.status === 0 && (r.stdout || '') === '';
  }
  function versionFromGit(dir){
    const r = cp.spawnSync('git',['describe','--tags','--abbrev=0'],{cwd:dir,encoding:'utf8'});
    if (r.status !== 0){
      const e = (r.stderr||'').trim();
      errln(e ? `Git error: ${e}` : 'No git tags found');
      process.exit(1);
    }
    let v = (r.stdout||'').trim();
    if (v.startsWith('v')) v = v.slice(1);
    return v;
  }
  function gitCommitAndTag(version, msg, dir){
    let add = cp.spawnSync('git',['add','package.json'],{cwd:dir,encoding:'utf8'});
    if (add.status !== 0){ errln(`Git add failed with exit code ${add.status}`); process.exit(1); }
    const commitMsg = msg ? msg.split('%s').join(version) : `v${version}`;
    let c = cp.spawnSync('git',['commit','-m',commitMsg],{cwd:dir,encoding:'utf8'});
    if (c.status !== 0){ errln('Git commit failed'); process.exit(1); }
    const tag = `v${version}`;
    let t = cp.spawnSync('git',['tag','-a',tag,'-m',tag],{cwd:dir,encoding:'utf8'});
    if (t.status !== 0){ errln('Git tag failed'); process.exit(1); }
  }

  // ---- help / previews ----
  function readCurrentVersion(){
    try {
      const txt = fs.readFileSync(pkgPath,'utf8');
      const j = JSON.parse(txt);
      if (j && typeof j === 'object' && !Array.isArray(j) && typeof j.version === 'string') return j.version;
    } catch(e){}
    return null;
  }
  function showHelp(){
    const cur = readCurrentVersion();
    const base = cur || '1.0.0';
    process.stdout.write(`bun pm version v0.1.0\n`);
    if (cur) process.stdout.write(`Current package version: v${cur}\n`);
    process.stdout.write(`\nIncrement:\n`);
    const line = (label, nv) => process.stdout.write(`  ${label.padEnd(10)} ${base} → ${nv}\n`);
    line('patch', calcNewVersion(base,'patch',null,pkgDir));
    line('minor', calcNewVersion(base,'minor',null,pkgDir));
    line('major', calcNewVersion(base,'major',null,pkgDir));
    line('prerelease', calcNewVersion(base,'prerelease',null,pkgDir));
    if (base.indexOf('-') >= 0 || preid){
      line('prepatch', calcNewVersion(base,'prepatch',null,pkgDir));
      line('preminor', calcNewVersion(base,'preminor',null,pkgDir));
      line('premajor', calcNewVersion(base,'premajor',null,pkgDir));
    }
    process.stdout.write(`  from-git   Use version from latest git tag\n`);
    process.stdout.write(`  1.2.3      Set specific version\n`);
    process.exit(0);
  }

  if (positionals.length === 0){ showHelp(); return; }

  // ---- parse version argument ----
  const arg = positionals[0];
  let versionType, specific = null;
  if (KNOWN.indexOf(arg) >= 0){ versionType = arg; }
  else if (parseSemver(arg)){ versionType = 'specific'; specific = arg; }
  else { errln(`Invalid version argument: "${arg}"`); process.exit(1); }

  // ---- git preflight ----
  if (gitTagVersion){
    if (!fs.existsSync(path.join(pkgDir,'.git'))){
      gitTagVersion = false;
    } else if (!force && !gitClean(pkgDir)){
      errln('Git working directory not clean.');
      process.exit(1);
    }
  }

  // ---- read + parse package.json ----
  let contents;
  try { contents = fs.readFileSync(pkgPath,'utf8'); }
  catch(e){ errln(`Failed to read package.json: ${e && e.message ? e.message : e}`); process.exit(1); }

  let json;
  try { json = JSON.parse(contents); }
  catch(e){ errln('Failed to parse package.json'); process.exit(1); }
  if (!json || typeof json !== 'object' || Array.isArray(json)){
    errln('Failed to parse package.json'); process.exit(1);
  }

  const scripts = (!ignoreScripts && json.scripts && typeof json.scripts === 'object') ? json.scripts : null;
  function runScript(name){
    if (!scripts) return;
    const cmd = scripts[name];
    if (typeof cmd !== 'string' || cmd.length === 0) return;
    const env = Object.assign({}, process.env, { npm_lifecycle_event: name, npm_lifecycle_script: cmd });
    const r = cp.spawnSync('sh',['-c',cmd],{cwd:pkgDir, env, stdio:'inherit'});
    if (r.status !== 0){
      errln(`script "${name}" exited with code ${r.status == null ? 1 : r.status}`);
      process.exit(1);
    }
  }

  runScript('preversion');

  const currentVersion = (typeof json.version === 'string') ? json.version : null;
  const newVersion = calcNewVersion(currentVersion || '0.0.0', versionType, specific, pkgDir);

  if (currentVersion !== null && !allowSame && currentVersion === newVersion){
    errln('Version not changed'); process.exit(1);
  }

  // ---- write package.json (preserve formatting for in-place version edit) ----
  let out;
  const reVersion = /("version"\s*:\s*)"[^"]*"/;
  if (reVersion.test(contents)){
    out = contents.replace(reVersion, `$1"${newVersion}"`);
  } else {
    json.version = newVersion;
    out = JSON.stringify(json, null, 2);
    if (contents.endsWith('\n')) out += '\n';
  }
  try { fs.writeFileSync(pkgPath, out); }
  catch(e){ errln(`Failed to write package.json: ${e && e.message ? e.message : e}`); process.exit(1); }

  runScript('version');

  if (gitTagVersion){
    gitCommitAndTag(newVersion, message, pkgDir);
  }

  runScript('postversion');

  process.stdout.write(`v${newVersion}\n`);
  process.exit(0);
})();
)JS";

    std::string code = argsLit + std::string{BODY};
    std::vector<std::string> jsArgv{"mbun", "pm", "version"};
    for (std::string_view a : pmArgs) jsArgv.emplace_back(a);
    mbun::jsc::runtime::set_argv(std::move(jsArgv));
    return mbun::jsc::runtime::run_eval(code);
}

// Transcribed from bun's exec help (ref: bun-ref/src/cli/mod.rs:2139-2154).
constexpr std::string_view EXEC_USAGE = R"(Usage: mbun exec <script>

Execute a shell script directly from mbun.

Note: If executing this from a shell, make sure to escape the string!

Examples:
  mbun exec "echo hi"
  mbun exec "echo \"hey friends\"!"
)";

// `mbun exec <script>`: run a shell script through mbun's shell interpreter,
// inheriting this process's stdio; the script's exit code becomes ours
// (ref: bun-ref/src/cli/exec_command.rs:78-102). With no positional, bun prints
// exec's help and treats that as success (mod.rs:1566-1568 -> tag_print_help).
int run_exec(const std::string& script) {
    if (script.empty()) {
        std::print("{}", EXEC_USAGE);
        return 0;
    }
    return mbun::jsc::runtime::run_shell_source(script);
}

// ─── `mbun test` file discovery ─────────────────────────────────────────────
// Port of bun's Scanner (ref: bun-ref/src/cli/test/Scanner.rs) plus the
// path-mode/filter-mode switch that drives it (test_command.rs:2272-2296).
//
// The positional list is classified as a whole: if ANY positional is absolute or
// starts with "./" / "../", ALL are scanned as filepaths; otherwise ALL are
// filters matched (by substring) against a scan of the top level dir. In path
// mode a positional is opened as a directory first; ENOTDIR means it is a file
// and is taken without a .test/_test/.spec/_spec suffix requirement, but it must
// still have a JS-like extension (`is_test_file` → `could_be_test_file<false>`,
// Scanner.rs:152-156, 274-280) — so a named `./favicon.ico` is skipped, never
// run. Walking a directory instead requires the suffix
// (`could_be_test_file<true>`, Scanner.rs:425).

// Extensions whose loader `is_javascript_like()` (ref: Scanner.rs:275-277).
constexpr std::array JS_LIKE_EXTS { std::string_view { ".js" },  std::string_view { ".jsx" },
                                    std::string_view { ".ts" },  std::string_view { ".tsx" },
                                    std::string_view { ".mjs" }, std::string_view { ".cjs" },
                                    std::string_view { ".mts" }, std::string_view { ".cts" } };

// ref: Scanner.rs:460 `TEST_NAME_SUFFIXES`.
constexpr std::array TEST_NAME_SUFFIXES { std::string_view { ".test" }, std::string_view { "_test" },
                                          std::string_view { ".spec" }, std::string_view { "_spec" } };

// `could_be_test_file<NEEDS_TEST_SUFFIX>` (ref: Scanner.rs:274-289). The suffix is
// matched on the basename with its extension removed, so "a.test.ts" → "a.test".
bool could_be_test_file(std::string_view name, bool needsTestSuffix) {
    const std::size_t dot { name.rfind('.') };
    if (dot == std::string_view::npos) return false;
    const std::string_view ext { name.substr(dot) };
    if (!std::ranges::contains(JS_LIKE_EXTS, ext)) return false;
    if (!needsTestSuffix) return true;
    const std::string_view stem { name.substr(0, dot) };
    return std::ranges::any_of(TEST_NAME_SUFFIXES,
                               [stem](std::string_view s) { return stem.ends_with(s); });
}

// Walk `dir` collecting test-suffixed files. Directories whose name starts with
// "." or equals "node_modules" are pruned (ref: Scanner.rs:367-370).
void scan_dir_for_tests(const std::filesystem::path& dir, std::vector<std::filesystem::path>& out) {
    std::error_code ec {};
    std::filesystem::recursive_directory_iterator it {
        dir, std::filesystem::directory_options::skip_permission_denied, ec
    };
    if (ec) return;
    for (auto iter { it }; iter != std::filesystem::recursive_directory_iterator {};
         iter.increment(ec)) {
        if (ec) break;
        const std::filesystem::path& p { iter->path() };
        const std::string name { p.filename().string() };
        std::error_code ec2 {};
        if (iter->is_directory(ec2)) {
            if (name == "node_modules" || name.starts_with(".")) iter.disable_recursion_pending();
            continue;
        }
        if (!iter->is_regular_file(ec2)) continue;
        if (could_be_test_file(name, /*needsTestSuffix=*/true)) out.push_back(p);
    }
}

// Resolve `mbun test`'s positionals to the list of files to run. bun's Scanner
// visits directories breadth-first and sorts sibling entries by lowercased base
// name (Scanner.rs:396-412) purely so discovery order is deterministic; sorting
// the collected absolute paths gets the same guarantee, which is what the
// `--randomize` tests (an unrandomized run must equal the next unrandomized run)
// and `--bail` ordering depend on.
std::vector<std::filesystem::path> discover_test_files(std::span<const std::string> filters) {
    std::vector<std::filesystem::path> out {};
    std::error_code ec {};

    // No positional: scan cwd (bun scans the top level dir).
    if (filters.empty()) {
        scan_dir_for_tests(std::filesystem::current_path(ec), out);
        std::ranges::sort(out);
        return out;
    }

    // bun decides path-mode vs filter-mode ONCE for the whole positional list, not
    // per positional (ref: test_command.rs:2272-2296 `has_relative_path`): if ANY
    // positional is absolute or starts with "./" / "../", every positional is
    // scanned as a filepath; otherwise every positional is a filter matched against
    // a scan of the top level dir. A bare `bun test plain.ts` is therefore a FILTER,
    // which is why bun prints `note: To treat the "plain.ts" filter as a path, run
    // "bun test ./plain.ts"` (test_command.rs:2736).
    const bool hasRelativePath { std::ranges::any_of(filters, [](const std::string& f) {
        return std::filesystem::path { f }.is_absolute() || f.starts_with("./") || f.starts_with("../");
    }) };

    if (hasRelativePath) {
        for (const std::string& f : filters) {
            const std::filesystem::path p { f };
            if (std::filesystem::is_directory(p, ec)) {
                std::vector<std::filesystem::path> found {};
                scan_dir_for_tests(p, found);
                std::ranges::sort(found);
                out.insert(out.end(), found.begin(), found.end());
            } else if (std::filesystem::is_regular_file(p, ec)
                       && could_be_test_file(p.filename().string(), /*needsTestSuffix=*/false)) {
                // Named file: no .test/.spec suffix needed, but a JS-like extension
                // still is — `Scanner::scan`'s ENOTDIR branch gates on `is_test_file`
                // → `could_be_test_file::<false>` (Scanner.rs:152-156, 274-280). So an
                // explicitly named `./tsconfig.json` is silently skipped, not run.
                out.push_back(p);
            }
        }
        return out;
    }

    // Filter mode: scan the top level dir and keep the test files whose path
    // contains any positional as a substring (`does_path_match_filter` is
    // index_of, not starts_with — Scanner.rs:306-317).
    std::vector<std::filesystem::path> scanned {};
    scan_dir_for_tests(std::filesystem::current_path(ec), scanned);
    std::ranges::sort(scanned);
    for (const auto& p : scanned) {
        const std::string s { p.string() };
        if (std::ranges::any_of(filters,
                                [&s](const std::string& f) { return s.find(f) != std::string::npos; })) {
            out.push_back(p);
        }
    }
    return out;
}

// --only-failures: keep only the failing tests' report lines. bun implements this
// in the reporter — a test's status line, and everything printed with it, is
// skipped unless the result is a Fail (ref: test_command.rs:1283-1287).
//
// A test's detail can sit on either side of its status line in this runner: an
// assertion/throw prints flush-left as it happens and the "(fail)" line follows
// (test_runner.cppm runTest), while the --todo branch pushes its reason after.
// So non-status lines are buffered until the status line says who owns them, and
// lines trailing a kept "(fail)" are kept too.
std::string only_failure_lines(std::string_view body) {
    constexpr std::array STATUS_PREFIXES { std::string_view { "(pass)" }, std::string_view { "(fail)" },
                                           std::string_view { "(skip)" }, std::string_view { "(todo)" } };
    std::string out {};
    std::string pending {};  // detail seen before we know whose it is
    bool keeping { false };  // last status line was a (fail): its trailing detail stays

    const auto emit { [&out](std::string_view s) {
        if (s.empty()) return;
        if (!out.empty()) out.push_back('\n');
        out.append(s);
    } };

    for (const auto lineRange : std::views::split(body, '\n')) {
        const std::string_view line { lineRange.data(), lineRange.size() };
        const bool isStatus { std::ranges::any_of(
            STATUS_PREFIXES, [line](std::string_view p) { return line.starts_with(p); }) };

        if (!isStatus) {
            if (keeping) emit(line);  // trailing detail of the (fail) above
            else {                    // may belong to a (fail) still to come
                if (!pending.empty()) pending.push_back('\n');
                pending.append(line);
            }
            continue;
        }

        keeping = line.starts_with("(fail)");
        if (keeping) {
            emit(pending);
            emit(line);
        }
        pending.clear();
    }
    return out;
}

// Fold a bunfig `[jsx]`-family block into the runtime's process-wide JSX config.
//
// ref bunfig/bunfig.rs:949-1014 — bunfig writes factory/fragment/import_source
// only when non-empty, but ALWAYS writes runtime+development (so a bunfig with
// only `jsxImportSource` still lands on the Automatic/dev default). modules/
// bunfig already parses every one of these keys into BunfigConfig; until now
// nothing read them back out, so `jsx_import_source` was a field that could be
// set and could not do anything.
void apply_bunfig_jsx(const mbun::bunfig::BunfigConfig& cfg,
                      mbun::js_parser::detail::JsxOptions& out) {
    if (!cfg.jsx_factory.empty()) out.factory = cfg.jsx_factory;
    if (!cfg.jsx_fragment.empty()) out.fragment = cfg.jsx_fragment;
    if (!cfg.jsx_import_source.empty()) out.import_source = cfg.jsx_import_source;
    // `Solid` has no lowering here; bun treats it as its own runtime and mbun
    // has not ported one, so it falls back to the classic factory rather than
    // pretending. (bunfig still ACCEPTS the value — rejecting it would be
    // inventing an error bun does not have.)
    out.runtime = cfg.jsx_runtime == mbun::bunfig::JsxRuntime::Automatic
                      ? mbun::js_parser::detail::JsxRuntime::Automatic
                      : mbun::js_parser::detail::JsxRuntime::Classic;
    out.development = cfg.jsx_development;
}

// `mbun test [file|dir|filter]...`: discover the test files, run each through
// mbun.jsc.test_runner, and print a bun-style per-file report + aggregate
// summary. Returns the process exit code (0 all pass, 1 any fail / load error).
int run_test(std::span<const std::string_view> args) {
    mbun::cli::TestFlags flags { mbun::cli::parse_test(args) };
    if (!flags.parseError.empty()) {
        std::println(std::cerr, "error: {}", flags.parseError);
        return 1;
    }

    // bunfig.toml's [test] block supplies defaults the command line overrides
    // (ref: bun-ref/src/bunfig/bunfig.rs — load_config runs before Arguments'
    // flag parse and each flag only turns the option ON). modules/bunfig already
    // parses `onlyFailures`/`randomize`/`seed`; this is the consumer.
    {
        std::error_code ec {};
        if (std::filesystem::exists("bunfig.toml", ec)) {
            std::ifstream in { "bunfig.toml", std::ios::binary };
            if (in) {
                const std::string src { std::istreambuf_iterator<char> { in },
                                        std::istreambuf_iterator<char> {} };
                if (auto root = mbun::toml::parse(src)) {
                    if (auto cfg = mbun::bunfig::Parser { mbun::bunfig::Command::Test }.parse(*root)) {
                        if (cfg->test.only_failures) flags.onlyFailures = true;
                        if (cfg->test.randomize) flags.randomize = true;
                        if (cfg->test.seed && !flags.seed) {
                            flags.seed = *cfg->test.seed;
                            flags.randomize = true;  // a seed implies randomizing
                        }
                        apply_bunfig_jsx(*cfg, mbun::jsc::module_loader::runtime_jsx_options());
                    } else {
                        const auto& e = cfg.error();
                        if (e.key.empty())
                            std::println(std::cerr, "error: {} in bunfig.toml", e.message);
                        else
                            std::println(std::cerr, "error: {} for \"{}\" in bunfig.toml", e.message,
                                         e.key);
                    }
                }
            }
        }
    }

    // AI-agent detection: bun defaults --only-failures ON for agents so
    // pass/skip/todo lines don't flood the agent's context window
    // (ref bun-ref test_command.rs:2163-2166 + output.rs is_ai_agent).
    if (!flags.onlyFailures) {
        bool agent {};
        if (const char* a { std::getenv("AGENT") }) {
            agent = std::string_view { a } == "1";
        } else {
            const auto truthy { [](const char* name) {
                const char* v { std::getenv(name) };
                if (!v || !*v) return false;
                char* end {};
                const long n { std::strtol(v, &end, 10) };
                return end != v && *end == '\0' && n != 0;
            } };
            agent = truthy("CLAUDECODE") || truthy("REPL_ID");
        }
        if (agent) flags.onlyFailures = true;
    }

    // The CLI flags land AFTER bunfig so they win: bun loads bunfig during
    // Arguments' parse and then applies `--jsx-import-source`/`--jsx-runtime`
    // over the top (ref Arguments.rs:1385-1386).
    {
        auto& jsx { mbun::jsc::module_loader::runtime_jsx_options() };
        if (flags.jsxImportSource) jsx.import_source = *flags.jsxImportSource;
        if (flags.jsxRuntime) {
            // ref options_types/jsx.rs:44-53 RUNTIME_MAP. cli.cppm already
            // rejected anything outside this set.
            jsx.runtime = (*flags.jsxRuntime == "classic" || *flags.jsxRuntime == "react")
                              ? mbun::js_parser::detail::JsxRuntime::Classic
                              : mbun::js_parser::detail::JsxRuntime::Automatic;
        }
    }

    // bun routes the per-file lines and the aggregate summary to stderr (tests that
    // spawn `bun test` scrape proc.stderr for "N pass"/"Ran N tests"), but the
    // banner goes to STDOUT: test_command.rs exec() writes it via Output::writer()
    // followed by a single b"\n". Snapshot tests assert on stdout, so the banner
    // must be exactly "bun test <version_with_sha>" + one newline — no blank line.
    std::println(std::cout, "bun test {}", mbun::cli::VERSION_WITH_SHA);

    std::vector<std::filesystem::path> files { discover_test_files(flags.filters) };
    if (files.empty()) {
        // ref: test_command.rs:2693.
        std::println(std::cerr,
                     "No tests found!\n\nTests need \".test\", \"_test_\", \".spec\" or \"_spec_\" "
                     "in the filename (ex: \"MyApp.test.ts\")");
        return flags.passWithNoTests ? 0 : 1;
    }

    // --seed is only *drawn* when randomizing; bun keeps it at 0 otherwise and
    // gates the printed "--seed=N" on the PRNG existing (test_command.rs:2014-2031,
    // :2805-2808). `--seed N` implies --randomize (Arguments.rs:1832).
    std::optional<std::uint32_t> seed {};
    if (flags.randomize) {
        seed = flags.seed ? *flags.seed
                          : static_cast<std::uint32_t>(std::random_device {}());
    }

    // Randomize the order of test files (ref: test_command.rs:2572-2584). bun runs a
    // reverse Fisher-Yates over its top-level PRNG here; mbun draws the file order
    // from a PRNG seeded by the run seed alone, so `--seed=N` reproduces it.
    if (seed) {
        std::mt19937_64 fileRng { *seed };
        std::ranges::shuffle(files, fileRng);
    }

    const auto started { std::chrono::steady_clock::now() };
    int pass { 0 }, fail { 0 }, skip { 0 }, todo { 0 }, errors { 0 }, expectCalls { 0 };
    int skippedLabel { 0 };  // tests dropped by -t/--test-name-pattern (jest.rs:282)

    // The label filter (-t/--test-name-pattern/--grep), forwarded to each file.
    const std::optional<std::string_view> namePattern {
        flags.testNamePattern ? std::optional<std::string_view> { *flags.testNamePattern }
                              : std::nullopt };

    for (const auto& f : files) {
        const std::string path { f.string() };
        // The file header is titled with the path RELATIVE to the top level dir,
        // not the absolute path (ref: test_command.rs:3096 — `let file_title =
        // resolve_path::relative(FileSystem::instance().top_level_dir, file_path)`).
        std::error_code rec {};
        const std::filesystem::path rel { std::filesystem::relative(f, std::filesystem::current_path(rec), rec) };
        const std::string title { (rec || rel.empty()) ? path : rel.string() };

        mbun::jsc::test_runner::RunResult r { mbun::jsc::test_runner::run_file(path, seed, namePattern) };

        if (!r.ok) {  // a file that fails to load/run counts as one failed test (bun)
            std::println(std::cerr, "{}:\n  error: {}\n", title, r.error);
            fail += 1;
            errors += 1;
            continue;
        }

        // --only-failures hides everything but the failures (ref: Arguments.rs:601,
        // test_command.rs:1284/1340 — the reporter drops non-failure lines).
        const std::string body { flags.onlyFailures ? only_failure_lines(r.body) : r.body };
        // With --only-failures a file contributing no failures prints nothing at
        // all — not even its path header (test_command.rs:1340 gates the header
        // on the reporter, and only-failures suppresses it).
        if (!flags.onlyFailures || !body.empty()) {
            std::println(std::cerr, "{}:", title);
            if (!body.empty()) std::println(std::cerr, "{}", body);
        }
        if (r.errors > 0 && !r.error_text.empty()) {
            std::println(std::cerr,
                         "\n# Unhandled error between tests\n"
                         "-------------------------------\n{}\n"
                         "-------------------------------",
                         r.error_text);
        }

        pass += r.pass;
        fail += r.fail;
        skip += r.skip;
        todo += r.todo;
        errors += r.errors;
        expectCalls += r.expect_calls;
        skippedLabel += r.skipped_label;
    }

    const auto elapsed { std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - started)
                             .count() };

    // Summary block, in bun's order (ref: test_command.rs:2805-2896): the seed
    // line, then pass / skip / todo / fail / errors / expect() calls, then
    // print_summary()'s "Ran N tests across M files." — where N counts
    // pass+fail+skip+todo (test_command.rs:1395), NOT the between-test errors.
    std::println(std::cerr, "");
    if (seed) std::println(std::cerr, " --seed={}", *seed);
    std::println(std::cerr, " {} pass", pass);
    if (skip > 0) std::println(std::cerr, " {} skip", skip);
    if (todo > 0) std::println(std::cerr, " {} todo", todo);
    std::println(std::cerr, " {} fail", fail);
    if (errors > 0) std::println(std::cerr, " {} error{}", errors, errors == 1 ? "" : "s");
    if (expectCalls > 0) std::println(std::cerr, " {} expect() calls", expectCalls);
    std::println(std::cerr, "Ran {} test{} across {} file{}. [{:.2f}ms]", pass + fail + skip + todo,
                 (pass + fail + skip + todo) == 1 ? "" : "s", files.size(),
                 files.size() == 1 ? "" : "s", elapsed);
    // Exit 1 on any failure, or when the label filter (-t/--test-name-pattern)
    // matched nothing so no test ran — unless --pass-with-no-tests. Port of
    // test_command.rs:2930 `should_fail_on_no_tests = !pass_with_no_tests &&
    // (failed_to_find_any_tests || did_label_filter_out_all_tests())`, where
    // did_label_filter_out_all_tests() (jest.rs:282) is `skipped_because_label > 0
    // && (pass+skip+todo+fail+expectations) == 0`.
    const bool labelFilteredAll {
        skippedLabel > 0 && (pass + skip + todo + fail + expectCalls) == 0 };
    return (fail > 0 || (!flags.passWithNoTests && labelFilteredAll)) ? 1 : 0;
}

int run_install(std::span<const std::string_view> args) {
    mbun::install::command::InstallOptions options{};
    for (std::size_t i{0}; i < args.size(); ++i) {
        std::string_view arg{args[i]};
        if (arg == "--frozen-lockfile") {
            options.frozenLockfile = true;
        } else if (arg == "--ignore-scripts") {
            options.ignoreScripts = true;
        } else if (arg == "--no-progress") {
            options.noProgress = true;
        } else if (arg == "--lockfile-only") {
            options.lockfileOnly = true;
        } else if (arg == "--save-text-lockfile") {
            options.saveTextLockfile = true;
        } else if (arg == "--force" || arg == "-f") {
            options.force = true;
        } else if (arg == "--registry" && i + 1 < args.size()) {
            options.registry.assign(args[++i]);
        } else if (arg.starts_with("--registry=")) {
            options.registry.assign(arg.substr(std::string_view{"--registry="}.size()));
        } else {
            std::println(std::cerr, "error: unsupported install argument '{}'", arg);
            return 2;
        }
    }

    // Installing a project: judge its package.json engines with the compat
    // versions (warn-only, npm non-strict).
    if (auto packageJson{find_package_json(std::filesystem::current_path())}) {
        warn_engines_mismatch(*packageJson);
    }
    auto result{mbun::install::command::install_project(std::filesystem::current_path(), options)};
    if (!result) {
        std::println(std::cerr, "error: {}", result.error().message);
        return 1;
    }
    // bun prints "Saved lockfile" on stderr when it writes bun.lock
    // (PackageManagerDirectories.rs save_lockfile).
    if (result->savedLockfile) {
        std::println(std::cerr, "Saved lockfile");
    }
    std::println("mbun install v{}\n", mbun::cli::VERSION);
    if (options.lockfileOnly) {
        // ref: install_with_manager.rs:1767-1782 save_lockfile_only summary.
        std::println("Saved bun.lock ({} package{})", result->lockPackageCount,
                     result->lockPackageCount == 1 ? "" : "s");
        return 0;
    }
    std::println("{} package{} installed", result->installed,
                 result->installed == 1 ? "" : "s");
    return 0;
}

// ─── `mbun add` ─────────────────────────────────────────────────────────────

// `bun add --help`. Wording mirrors bun's add help block
// (ref: bun-ref/src/install/PackageManager/CommandLineArguments.rs:692-753),
// reduced to the flags this slice honours so the help never promises behaviour
// `add` does not have.
constexpr std::string_view ADD_USAGE = R"(Usage:
  Add a new dependency to package.json and install it.
  mbun add [flags] <package>...

Flags:
  -d, --dev                    Add dependency to "devDependencies"
      --optional               Add dependency to "optionalDependencies"
      --peer                   Add dependency to "peerDependencies"
  -E, --exact                  Add the exact version instead of the ^range
  -h, --help                   Print this help menu

Examples:
  mbun add left-pad
  mbun add left-pad@1.3.0
  mbun add -d typescript
)";

// One `add` request, after spec parsing.
//   ref: bun-ref/src/install/PackageManager/UpdateRequest.rs:143-289 `parse_with_error`
//        (alias/value split) — mbun reuses the already-ported
//        dependency::split_name_and_maybe_version + dependency::infer_tag.
struct AddRequest {
    std::string name;     // "left-pad", "@types/bun"
    std::string literal;  // what to write into package.json before install
    // True when the request resolved to a dist-tag (a bare name means the
    // "latest" tag: dependency.rs:794-797 "empty string means `latest`" and
    // :1352-1355). Only these get rewritten to `^<resolved>` after install —
    // an explicit range/version is written through verbatim
    //   (ref: PackageJSONEditor.rs:1204-1226 vs the :1250 fallthrough,
    //    `arena_dup(request.version.literal)`).
    bool isDistTag{false};
};

// Walk up from `start` looking for package.json (same search `install_project`
// does; `add` needs the path *before* installing in order to edit it).
std::optional<std::filesystem::path> find_package_json(const std::filesystem::path& start) {
    std::error_code ec;
    std::filesystem::path current{std::filesystem::weakly_canonical(start, ec)};
    if (ec) current = start;
    for (;; current = current.parent_path()) {
        if (std::filesystem::is_regular_file(current / "package.json", ec)) {
            return current / "package.json";
        }
        if (!current.has_relative_path()) break;
    }
    return std::nullopt;
}

// package.json "engines" gate for running/installing a project, judged with
// the COMPAT versions mbun claims (mbun::cli::NODE_COMPAT_VERSION for
// engines.node, mbun::cli::VERSION for engines.bun — the corpus-pinned
// contract): a node project and a bun project each get a truthful answer.
// npm non-strict semantics: a mismatch warns on stderr and continues.
void warn_engines_mismatch(const std::filesystem::path& packageJsonPath) {
    std::ifstream in{packageJsonPath, std::ios::binary};
    if (!in) return;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string text{buffer.str()};
    auto doc{mbun::install::npm::json::parse(text)};
    if (!doc || !doc->root) return;
    const auto* engines{doc->root->get("engines")};
    if (engines == nullptr || !engines->is_object()) return;
    const auto check{[&](std::string_view key, std::string_view compatVersion) {
        const auto* range{engines->get(key)};
        if (range == nullptr || range->str.empty()) return;
        if (mbun::semver::satisfies(compatVersion, range->str)) return;
        std::println(std::cerr,
                     "warn: package.json engines.{} \"{}\" is not satisfied by the {} "
                     "compatibility version {} (mbun {})",
                     key, range->str, key, compatVersion, mbun::cli::MBUN_VERSION);
    }};
    check("node", mbun::cli::NODE_COMPAT_VERSION);
    check("bun", mbun::cli::VERSION);
}

// Print the edited tree back over package.json, preserving a trailing newline
// when the original had one.
//   ref: updatePackageJSONAndInstall.rs:175-176 (`preserve_trailing_newline_at_eof
//   = contents.last() == Some(&b'\n')`) and :371 (`buffer_writer.append_newline
//   = preserve_trailing_newline_at_eof_for_package_json`) — oven-sh/bun#1375.
bool write_package_json(const std::filesystem::path& path,
                        const mbun::install::npm::json::Value& root,
                        const mbun::install::npm::json::Indentation& indent,
                        bool trailingNewline) {
    std::string out{mbun::install::npm::json::stringify(root, indent)};
    if (trailingNewline) out.push_back('\n');
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    if (!file) return false;
    file.write(out.data(), static_cast<std::streamsize>(out.size()));
    return static_cast<bool>(file);
}

int run_add(std::span<const std::string_view> args) {
    namespace json = mbun::install::npm::json;
    namespace editor = mbun::install::package_json_editor;

    auto flags{mbun::cli::parse_add(args)};
    if (flags.help) {
        std::print("{}", ADD_USAGE);
        return 0;
    }
    if (!flags.parseError.empty()) {
        std::println(std::cerr, "error: {}", flags.parseError);
        return 1;
    }
    if (!flags.unsupported.empty()) {
        std::println(std::cerr, "error: unsupported add argument '{}'", flags.unsupported.front());
        return 1;
    }
    if (flags.packages.empty()) {
        // ref: updatePackageJSONAndInstall.rs:33-41 — err + help, then exit 0.
        std::println(std::cerr, "error: no package specified to add");
        std::print("{}", ADD_USAGE);
        return 0;
    }

    // ── which dependency list (ref: CommandLineArguments.rs:1308-1313) ──────
    editor::List list{editor::List::Dependencies};
    if (flags.dev) {
        list = editor::List::DevDependencies;
    } else if (flags.optional) {
        list = editor::List::OptionalDependencies;
    } else if (flags.peer) {
        list = editor::List::PeerDependencies;
    }

    // ── parse the update requests ──────────────────────────────────────────
    std::vector<AddRequest> requests;
    for (const auto& spec : flags.packages) {
        auto [name, version] = mbun::install::dependency::split_name_and_maybe_version(spec);
        if (name.empty()) {
            // ref: UpdateRequest.rs:216-232 `unrecognised dependency format: {}`.
            std::println(std::cerr, "error: unrecognised dependency format: {}", spec);
            return 1;
        }
        const std::string_view value{version.value_or("")};
        const auto tag{mbun::install::dependency::infer_tag(value)};
        AddRequest request{std::string{name}, std::string{value},
                           tag == mbun::install::dependency::Tag::DistTag};
        if (request.isDistTag && request.literal.empty()) {
            // An empty spec is the "latest" dist-tag. bun writes "" here and
            // lets its resolver infer the tag (dependency.rs:794-797 /
            // :1352-1355); mbun's installer resolves the tag from the
            // package.json literal, so the inferred tag name goes in verbatim.
            // Either way it is a placeholder — the post-install edit below
            // replaces it with `^<resolved>`.
            request.literal = "latest";
        }
        requests.push_back(std::move(request));
    }

    const auto packageJsonPath{find_package_json(std::filesystem::current_path())};
    if (!packageJsonPath) {
        std::println(std::cerr, "error: could not find package.json");
        return 1;
    }

    std::string source;
    {
        std::ifstream file{*packageJsonPath, std::ios::binary};
        if (!file) {
            std::println(std::cerr, "error: failed to read package.json \"{}\"",
                         packageJsonPath->string());
            return 1;
        }
        source.assign(std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{});
    }
    // ref: updatePackageJSONAndInstall.rs:130-136 — the package.json is parsed
    // with `guess_indentation: true`, and :379 feeds that indent to print_json.
    const json::Indentation indent{json::guess_indentation(source)};
    const bool trailingNewline{source.ends_with('\n')};

    auto document{json::parse(source)};
    if (!document) {
        std::println(std::cerr, "error: failed to parse package.json \"{}\"",
                     packageJsonPath->string());
        return 1;
    }

    // ── phase 1: write the requested literals, then install ────────────────
    // bun edits twice on purpose rather than resolving up front — see the
    // rationale at updatePackageJSONAndInstall.rs:391-399 ("pretend a human
    // did: bun add react@latest, open lockfile, find what react resolved to,
    // replace \"react\": \"latest\" with \"react\": \"^16.2.0\"").
    std::vector<editor::NewDependency> pending;
    pending.reserve(requests.size());
    for (const auto& request : requests) {
        pending.push_back(editor::NewDependency{request.name, request.literal});
    }
    editor::edit(*document, list, pending);
    if (!write_package_json(*packageJsonPath, *document->root, indent, trailingNewline)) {
        std::println(std::cerr, "error: failed to write package.json \"{}\"",
                     packageJsonPath->string());
        return 1;
    }

    auto result{mbun::install::command::install_project(packageJsonPath->parent_path(), {})};
    if (!result) {
        std::println(std::cerr, "error: {}", result.error().message);
        return 1;
    }

    // ── phase 2: rewrite dist-tag requests with the resolved version ───────
    // ref: PackageJSONEditor.rs:1204-1226 — `^` + the resolved version, or the
    // bare version under `--exact` (EditOptions::exact_versions, fed from
    // `manager.options.enable.exact_versions()` at :563/:575).
    std::vector<editor::NewDependency> resolved;
    for (const auto& request : requests) {
        if (!request.isDistTag) continue;
        const auto match{std::ranges::find_if(result->packages, [&](const std::string& entry) {
            // registry_install reports "name@version"; the version follows the
            // last '@' so scoped names ("@types/bun@1.0.0") split correctly.
            const std::size_t at{entry.rfind('@')};
            return at != std::string::npos && at != 0 && entry.substr(0, at) == request.name;
        })};
        if (match == result->packages.end()) continue;
        const std::string version{match->substr(match->rfind('@') + 1)};
        resolved.push_back(editor::NewDependency{
            request.name, flags.exact ? version : std::format("^{}", version)});
    }
    if (!resolved.empty()) {
        editor::edit(*document, list, resolved);
        if (!write_package_json(*packageJsonPath, *document->root, indent, trailingNewline)) {
            std::println(std::cerr, "error: failed to write package.json \"{}\"",
                         packageJsonPath->string());
            return 1;
        }
    }

    std::println("mbun add v{}\n", mbun::cli::VERSION);
    for (const auto& dep : resolved) {
        std::println("installed {}@{}", dep.name, dep.version);
    }
    std::println("\n{} package{} installed", result->installed,
                 result->installed == 1 ? "" : "s");
    return 0;
}

// ─── `mbun build` ───────────────────────────────────────────────────────────

// `bun build --help`. Shape (Usage / Flags / Examples blocks and their wording)
// mirrors bun's BuildCommand help (src/cli/mod.rs :2005-2034); the flag list is
// bun's BUILD_ONLY_PARAMS (src/cli/Arguments.rs :401-550) reduced to the flags
// this build slice actually accepts, with the unimplemented ones marked so the
// help never promises behaviour the bundler does not have.
constexpr std::string_view BUILD_USAGE = R"(Usage:
  Transpile and bundle one or more files.
  mbun build [flags] <entrypoint>

Flags:
  --target <STR>                   The intended execution environment for the bundle. "browser", "bun" or "node"
  --outdir <STR>                   Default to "dist" if multiple files
  --outfile <STR>                  Write to a file
  --sourcemap <STR>?               Build with sourcemaps - 'linked', 'inline', 'external', or 'none'
  --format <STR>                   Specifies the module format to build to. Only "esm" is implemented.
  --root <STR>                     Root directory used for multiple entry points
  --public-path <STR>              A prefix to be appended to any import paths in bundled code
  --banner <STR>                   Add a banner to the bundled output such as "use client"; for a bundle being used with RSCs
  --footer <STR>                   Add a footer to the bundled output such as // built with bun!
  -e, --external <STR>...          Exclude module from transpilation (can use * wildcards). ex: -e react
  --conditions <STR>...            Pass custom conditions to resolve
  -h, --help                       Display this menu and exit

Not implemented yet in mbun (passing these is an error, never a silent no-op):
  --compile, --bytecode, --minify*, --splitting, --no-bundle, --production,
  --watch, --metafile*, --packages, --entry-naming/--chunk-naming/--asset-naming,
  --env, --react-*, --css-chunking, --app, --server-components

Examples:
  Bundle to a single file:
  mbun build --outfile=bundle.js ./src/index.ts

  Bundle into a directory:
  mbun build --outdir=out ./index.ts

  Bundle code to be run in Bun (reduces server startup time):
  mbun build --target=bun --outfile=server.js ./server.ts
)";

// bun's byte-size formatter, `bun_fmt::size` with default options
// (space_between_number_and_unit = true). Ported 1:1 from the SizeFormatter
// Display impl (src/bun_core/fmt.rs :2614-2660) so the `bun build` file listing
// prints identical sizes: 0 -> "0 KB", <512 -> "<n> bytes", then SI magnitudes
// keyed off integer log2/9 with 1 or 2 fraction digits.
std::string format_size(std::size_t value) {
    if (value == 0) return "0 KB";
    if (value < 512) return std::format("{} bytes", value);

    constexpr std::string_view MAGS_SI{" KMGTPEZY"};
    const auto log2{static_cast<std::size_t>(std::bit_width(value) - 1)};
    const std::size_t magnitude{std::min(log2 / 9, MAGS_SI.size() - 1)};
    const double newValue{static_cast<double>(value) / std::pow(1000.0, static_cast<double>(magnitude))};
    const char suffix{MAGS_SI[magnitude]};

    if (suffix == ' ') return std::format("{:.2f} KB", newValue / 1000.0);
    const int precision{std::abs(newValue - std::trunc(newValue)) <= 0.100 ? 1 : 2};
    return std::format("{:.{}f} {}B", newValue, precision, suffix);
}

// A resolver filesystem backed by the real OS, so on-disk entrypoints and their
// transitive imports resolve. Mirrors the Bun.build bridge's disk fallback
// (modules/jsc/src/runtime/bun_build.inc :164-168).
mbun::resolver::FileSystem build_os_fs() {
    return {
        .file_exists = [](std::string_view p) {
            std::error_code ec{};
            return std::filesystem::is_regular_file(std::filesystem::path{p}, ec);
        },
        .dir_exists = [](std::string_view p) {
            std::error_code ec{};
            return std::filesystem::is_directory(std::filesystem::path{p}, ec);
        },
        .read_file = [](std::string_view p) -> std::optional<std::string> {
            std::ifstream file{std::filesystem::path{p}, std::ios::binary};
            if (!file) return std::nullopt;
            std::ostringstream buf{};
            buf << file.rdbuf();
            if (!file && !file.eof()) return std::nullopt;
            return buf.str();
        },
    };
}

std::string build_absolute_entry(std::string_view entry) {
    std::filesystem::path path{entry};
    if (path.is_absolute()) return path.string();
    std::error_code ec{};
    const std::filesystem::path cwd{std::filesystem::current_path(ec)};
    if (ec) return path.string();
    return (cwd / path).string();
}

int run_build(std::span<const std::string_view> buildArgs) {
    const auto start{std::chrono::steady_clock::now()};
    auto flags{mbun::cli::parse_build(buildArgs)};

    if (flags.help) {
        std::print("{}", BUILD_USAGE);
        return 0;
    }
    if (!flags.parseError.empty()) {
        std::println(std::cerr, "error: {}", flags.parseError);
        return 1;
    }

    // ref: bun src/cli/Arguments.rs :1472-1488 — no entrypoints prints the banner,
    // the error and a short usage, then exits 1.
    if (flags.entryPoints.empty()) {
        std::println("mbun build {}", mbun::cli::VERSION_WITH_SHA);
        std::print(std::cerr, "error: Missing entrypoints. What would you like to bundle?\n\n");
        std::print("Usage:\n  $ mbun build <entrypoint> [...<entrypoints>] [...flags]  \n");
        std::print("\nTo see full documentation:\n  $ mbun build --help\n");
        return 1;
    }

    // Flags bun implements that this bundler slice does not. Failing loudly beats
    // emitting a bundle that silently ignored what the user asked for.
    if (!flags.unsupported.empty()) {
        std::println(std::cerr, "error: {} is not implemented yet in mbun's bundler",
                     flags.unsupported.front());
        return 1;
    }
    if (!flags.external.empty()) {
        std::println(std::cerr, "error: --external is not implemented yet in mbun's bundler");
        return 1;
    }
    if (flags.format != "esm") {
        std::println(std::cerr, "error: --format={} is not implemented yet in mbun's bundler",
                     flags.format);
        return 1;
    }
    if (!flags.publicPath.empty() || !flags.root.empty() || !flags.conditions.empty()) {
        std::println(std::cerr,
                     "error: --public-path/--root/--conditions are not "
                     "implemented yet in mbun's bundler");
        return 1;
    }

    // ref: bun src/cli/build_command.rs :161-167 — an external source map needs --outdir.
    if (flags.sourcemap == "external" && flags.outdir.empty()) {
        std::println(std::cerr, "error: cannot use an external source map without --outdir");
        return 1;
    }
    // ref: bun src/cli/build_command.rs :361-368.
    if (flags.entryPoints.size() > 1 && flags.outdir.empty()) {
        std::println(std::cerr, "error: Must use --outdir when specifying more than one entry point.");
        return 1;
    }
    // mbun's slice emits ONE combined chunk for all entries, so it cannot name a
    // per-entry output file the way bun does (one output per entry point). Reject
    // rather than write a single file that pretends to be the whole build.
    if (flags.entryPoints.size() > 1) {
        std::println(std::cerr,
                     "error: multiple entry points are not implemented yet in mbun's bundler");
        return 1;
    }

    std::vector<std::string> entryPoints{};
    for (const std::string& entry : flags.entryPoints) entryPoints.push_back(build_absolute_entry(entry));

    const mbun::resolver::FileSystem fs{build_os_fs()};
    const bool wantSourcemap{flags.sourcemap != "none"};
    auto built{mbun::bundler::build_bundle(entryPoints, mbun::bundler::Files{},
                                           {.sourcemap = wantSourcemap, .fs = &fs})};
    if (!built) {
        std::println(std::cerr, "error: {}", built.error().message);
        return 1;
    }

    // ref: bun src/bundler/bundle_v2.zig — a --banner is prepended and a --footer is
    // appended to each output chunk (after the hashbang, if any). expectBundled's CLI
    // backend passes the value quoted (`--banner="<v>"`) with no shell to strip it, so
    // `flags.banner`/`flags.footer` already carry the literal text bun writes verbatim.
    if (!flags.banner.empty()) {
        std::string_view code{built->code};
        std::string prefixed{};
        // Keep a leading hashbang (`#!...`) on line 1; the banner follows it.
        if (code.starts_with("#!")) {
            const std::size_t eol{code.find('\n')};
            const std::size_t cut{eol == std::string_view::npos ? code.size() : eol + 1};
            prefixed.append(code.substr(0, cut));
            if (eol == std::string_view::npos) prefixed.push_back('\n');
            prefixed.append(flags.banner);
            prefixed.push_back('\n');
            prefixed.append(code.substr(cut));
        } else {
            prefixed.append(flags.banner);
            prefixed.push_back('\n');
            prefixed.append(code);
        }
        built->code = std::move(prefixed);
    }
    if (!flags.footer.empty()) {
        if (!built->code.empty() && built->code.back() != '\n') built->code.push_back('\n');
        built->code.append(flags.footer);
        built->code.push_back('\n');
    }

    // ref: bun src/cli/build_command.rs :769-780 — with neither --outfile nor
    // --outdir and a single output file, the bundle goes to stdout and nothing else
    // is printed.
    if (flags.outfile.empty() && flags.outdir.empty()) {
        std::print("{}", built->code);
        std::cout.flush();
        return 0;
    }

    // ref: bun src/cli/build_command.rs :750-768 — --outfile without --outdir puts
    // the output in dirname(outfile) under basename(outfile).
    std::filesystem::path outPath{};
    if (!flags.outfile.empty()) {
        outPath = std::filesystem::path{flags.outfile};
        if (!flags.outdir.empty()) outPath = std::filesystem::path{flags.outdir} / outPath.filename();
    } else {
        // bun's default entry naming is "[dir]/[name].[ext]" (src/bundler/options.rs);
        // with one entry "[dir]" is empty, so the chunk lands at <outdir>/<name>.js.
        outPath = std::filesystem::path{flags.outdir} / std::filesystem::path{entryPoints.front()}.filename();
        outPath.replace_extension(".js");
    }

    std::error_code ec{};
    if (const std::filesystem::path parent{outPath.parent_path()}; !parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            std::println(std::cerr, "error: could not open output directory \"{}\"", parent.string());
            return 1;
        }
    }

    struct Emitted {
        std::string path;
        std::string contents;
        std::string_view kind;
    };
    std::vector<Emitted> emitted{};
    emitted.push_back({outPath.string(), built->code, "entry point"});
    if (wantSourcemap && !built->sourcemap.empty()) {
        std::filesystem::path mapPath{outPath};
        mapPath += ".map";
        emitted.push_back({mapPath.string(), built->sourcemap, "source map"});
    }

    bool hadErr{false};
    for (const Emitted& file : emitted) {
        std::ofstream out{file.path, std::ios::binary | std::ios::trunc};
        out.write(file.contents.data(), static_cast<std::streamsize>(file.contents.size()));
        if (!out) {
            std::println(std::cerr, "error: failed to write file \"{}\"", file.path);
            hadErr = true;
        }
    }
    if (hadErr) return 1;

    // ref: bun src/cli/build_command.rs :1019-1035 then :1045-1129 — the "Bundled N
    // modules in Xms" line, a blank line, then the two-space-indented listing of
    // "<path><pad><size>  <pad>(<kind>)" and a trailing blank line.
    const auto elapsedMs{std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count()};
    std::println("Bundled {} module{} in {}ms", built->moduleCount,
                 built->moduleCount == 1 ? "" : "s", elapsedMs);
    std::println("");

    std::size_t maxPathLen{0};
    std::size_t sizePadding{0};
    for (const Emitted& file : emitted) {
        maxPathLen = std::max(maxPathLen, file.path.size());
        sizePadding = std::max(sizePadding, format_size(file.contents.size()).size());
    }
    for (const Emitted& file : emitted) {
        const std::string size{format_size(file.contents.size())};
        std::println("  {}{}{}  {}({})", file.path,
                     std::string(std::max<std::size_t>(2, maxPathLen + 2 - file.path.size()), ' '), size,
                     std::string(sizePadding - size.size(), ' '), file.kind);
    }
    std::println("");
    return 0;
}

// bun accepts global run flags before the entrypoint (e.g. `bun --no-install
// script.js`). mbun has no installer/watcher, so these are recognised and
// skipped; the first non-flag token is then the command/script. Flags that carry
// their own value in a separate token (--install <v>) skip two.
// `--max-http-header-size <INT>` / `=<INT>`: sets the process-wide header limit
// every server reads per request. Returns the number of tokens consumed (0 when
// `args[i]` is a different flag). ref: bun src/runtime/cli/Arguments.rs:1049.
std::size_t take_max_http_header_size_flag(std::span<const std::string_view> args, std::size_t i) {
    static constexpr std::string_view kFlag{"--max-http-header-size"};
    const std::string_view a{args[i]};
    std::string_view value{};
    std::size_t consumed{0};
    if (a.starts_with(kFlag) && a.size() > kFlag.size() && a[kFlag.size()] == '=') {
        value = a.substr(kFlag.size() + 1);
        consumed = 1;
    } else if (a == kFlag && i + 1 < args.size()) {
        value = args[i + 1];
        consumed = 2;
    } else if (a == kFlag) {
        value = {};  // trailing `--max-http-header-size` with no value: bun errors
        consumed = 1;
    } else {
        return 0;
    }

    // bun: strings::parse_int::<usize>(size_str, 10) — a whole-string unsigned
    // decimal parse; anything else (empty, "NaN", "-1", "12x") is fatal.
    std::size_t size{0};
    const char* first{value.data()};
    const char* last{value.data() + value.size()};
    const auto parsed{std::from_chars(first, last, size, 10)};
    if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != last) {
        std::println(std::cerr,
                     "error: Invalid value for --max-http-header-size: \"{}\". Must be a positive "
                     "integer",
                     value);
        std::exit(1);
    }
    // bun: `if size == 0 { 1024 * 1024 * 1024 }` — 0 means "effectively unbounded".
    mbun::http::set_max_http_header_size(size == 0 ? 1024uz * 1024uz * 1024uz : size);
    return consumed;
}

// Generic `--flag <VALUE>` / `--flag=<VALUE>` reader: hands the value to `sink`
// and returns how many tokens it consumed (0 when args[i] is a different flag).
// bun's clap accepts both spellings for every value-taking option
// (Arguments.rs `args.option(b"--x")`), so both must be recognised here or the
// value token is mistaken for the run target ("Script not found \"--env-file\"").
std::size_t take_valued_flag(std::span<const std::string_view> args, std::size_t i,
                             std::string_view flag, void (*sink)(std::string)) {
    const std::string_view a{args[i]};
    if (a.starts_with(flag) && a.size() > flag.size() && a[flag.size()] == '=') {
        sink(std::string{a.substr(flag.size() + 1)});
        return 1;
    }
    if (a == flag && i + 1 < args.size()) {
        sink(std::string{args[i + 1]});
        return 2;
    }
    // A trailing valueless occurrence is still consumed (bun's clap reports a
    // missing-value error rather than treating it as a positional).
    if (a == flag) return 1;
    return 0;
}

// ─── `mbun run <script>` — package.json scripts ─────────────────────────────
// Port of bun's RunCommand::exec_with_cfg (ref: bun-ref/src/cli/run_command.rs
// :2357). The target-priority rules below are bun's, verbatim:
//
//   1. target starts with '.' OR is absolute  → try_fast_run + SKIP script check
//      (a path always means the file; `bun run ./build` never hits a "build" script)
//   2. else if allow_fast_run_for_extensions && loader can be run → try_fast_run
//      but the script check still runs if the file does not exist.
//      NOTE cli/mod.rs:1473 sets `allow_fast_run_for_extensions: tag ==
//      AutoCommand`, so it is FALSE for `bun run` and TRUE for bare `bun x.ts`.
//      => `bun run foo.ts` checks a *script literally named* "foo.ts" FIRST,
//         then falls through to module resolution; `bun foo.ts` runs the file.
//   3. empty target → print help;  "-" → stdin
//   4. package.json script (unless skipped)   → pre<name>, <name>, post<name>
//   5. module resolution (run the file)
//   6. node_modules/.bin + $PATH binary       (bin_dirs_only for AutoCommand)
//   7. --if-present → exit 0, else the Module/File/Script-not-found error + exit 1
struct RunFlags {
    bool silent{false};
    bool ifPresent{false};
    // DIVERGENCE: bun defaults to its OWN shell (run_command.rs:306) and
    // `--shell=bun|system` picks between them. mbun's shell interpreter
    // (mbun.shell) does not implement $VAR expansion or the `exit` builtin yet,
    // so BOTH settings currently run bun's `--shell=system` code path
    // (run_command.rs:355-370). The flag is parsed and tracked so the `bun`
    // branch can be wired to mbun.shell once it grows those features; until
    // then this field is deliberately not read.
    bool useSystemShell{true};
    // `-b` / `--bun`: "Force a script or package to use Bun's runtime instead of
    // Node.js (via symlinking node)" (`bun run --help`, bun 1.3.14). Sets
    // ctx.debug.run_in_bun → force_using_bun (run_command.rs:2435), whose ONLY
    // effect is planting the <BUN_NODE_DIR>/node shim at the front of PATH
    // (run_command.rs:1989 → install/lib.rs:565).
    bool forceUsingBun{false};
    // `--workspaces`: "Run a script in all workspace packages" (Arguments.rs:336
    // → ctx.workspaces at Arguments.rs:809).
    bool workspaces{false};
};

// `--cwd <STR>`: "Absolute path to resolve files & entry points from. This just
// changes the process' cwd." (Arguments.rs:120). bun joins it onto the current
// cwd and chdir()s, exiting 1 on failure (Arguments.rs:770-788).
void apply_cwd_flag(std::string_view dir) {
    std::error_code ec{};
    std::filesystem::path target{std::filesystem::path{std::filesystem::current_path(ec)} / dir};
    std::filesystem::current_path(target, ec);
    if (ec) {
        std::println(std::cerr, "error: Could not change directory to \"{}\"", dir);
        std::exit(1);
    }
}

// bun's `default_loader_for(target).can_be_run_by_bun()` (run_command.rs:774).
bool loader_can_be_run(std::string_view target) {
    return looks_like_script(target) || is_markdown(target);
}

// ref: run_command.rs:2745-2775 — the not-found wording is target-shaped.
int report_run_target_not_found(std::string_view target) {
    // An existing file with a loader that cannot be executed (e.g. .css) is
    // "Cannot run", not "File not found". ref bun run_command.rs:2745.
    std::error_code ec2{};
    std::filesystem::path tp{target};
    if (!target.empty() && !target.ends_with(".json") &&
        std::filesystem::exists(tp, ec2) && !std::filesystem::is_directory(tp, ec2) &&
        !loader_can_be_run(target)) {
        std::string ext{tp.extension().string()};
        if (!ext.empty() && ext.front() == '.') ext.erase(0, 1);
        std::println(std::cerr, "error: Cannot run \"{}\"", target);
        std::println(std::cerr, "note: Bun cannot run {} files directly", ext);
        return 1;
    }
    bool pathLike{!target.empty() &&
                  (target[0] == '.' || target[0] == '/' || std::filesystem::path{target}.is_absolute())};
    if (looks_like_script(target) || target.ends_with(".json") || pathLike) {
        std::println(std::cerr, "error: Module not found \"{}\"", target);
    } else if (!std::filesystem::path{target}.extension().empty()) {
        std::println(std::cerr, "error: File not found \"{}\"", target);
    } else {
        std::println(std::cerr, "error: Script not found \"{}\"", target);
    }
    return 1;
}

int exec_run_target(std::string_view target, std::span<const std::string_view> passthrough,
                    const RunFlags& flags, bool allowFastRunForExtensions, bool binDirsOnly) {
    namespace run = mbun::cli::run;
    std::error_code ec{};
    std::filesystem::path cwd{std::filesystem::current_path(ec)};
    // Running inside a project: judge its package.json engines with the compat
    // versions (warn-only, npm non-strict).
    if (auto packageJson{find_package_json(cwd)}) warn_engines_mismatch(*packageJson);

    // ── 1/2. target shape → fast-run + script-check policy ──────────────────
    bool tryFastRun{false};
    bool skipScriptCheck{false};
    if (!target.empty() && (target[0] == '.' || std::filesystem::path{target}.is_absolute())) {
        tryFastRun = true;
        skipScriptCheck = true;
    } else if (allowFastRunForExtensions && loader_can_be_run(target)) {
        tryFastRun = true;
    }

    if (tryFastRun && !target.empty()) {
        std::filesystem::path p{target};
        if (std::filesystem::exists(p, ec) && !std::filesystem::is_directory(p, ec)) {
            return run_script(target, passthrough);
        }
    }

    run::PackageScripts pkg{run::load_nearest_package_scripts(cwd)};

    // ── 3. empty target → help (run_command.rs:2456-2466) ───────────────────
    if (target.empty()) {
        run::print_help(pkg);
        if (!pkg.found) std::println("\nNo package.json found.\n");
        return 0;
    }

    // ── PATH stitching, unconditional (run_command.rs:2437) ─────────────────
    const char* pathEnv{std::getenv("PATH")};
    std::string originalPath{pathEnv != nullptr ? pathEnv : ""};
    // bun passes package_json_dir only when the enclosing package.json is not
    // the cwd's own (run_command.rs:1903-1909).
    std::string packageJsonDir{};
    if (pkg.found && pkg.packageJsonDir != cwd) packageJsonDir = pkg.packageJsonDir.string();

    // `--bun`, plus bun's implicit force when `node` is nowhere on PATH
    // (run_command.rs:1955-1966 + env_loader.rs:520). Resolving `node` is the
    // `found_node` probe; skip it when `--bun` already forces the shim.
    run::NodeShim shim{.forceUsingBun = flags.forceUsingBun};
    shim.nodeFound = flags.forceUsingBun ||
                     run::find_binary_in_path(originalPath, cwd.string(), "node").has_value();
    std::string newPath{run::build_path_for_run(cwd.string(), packageJsonDir, originalPath, &shim)};

    std::vector<std::pair<std::string, std::string>> env{
        {"PATH", newPath},
        {"npm_command", "run-script"},  // run_command.rs:2443
    };
    if (shim.planted) {
        // With `--bun`, load_node_js_config is called with the shim as
        // `override_node` and puts these two (env_loader.rs:509-531). Without it
        // (node simply absent) run_command.rs:2008-2023 puts the same two plus
        // `npm_execpath` = this binary. Both are the shim DIR, not <dir>/node —
        // see bun_node_file(); real bun exports NODE=/tmp/bun-node-<sha>.
        env.emplace_back("NODE", std::string{run::bun_node_file()});
        env.emplace_back("npm_node_execpath", std::string{run::bun_node_file()});
        if (!flags.forceUsingBun && !shim.bunSelfPath.empty()) {
            env.emplace_back("npm_execpath", shim.bunSelfPath);
        }
    } else if (flags.forceUsingBun) {
        // `--bun` was asked for and the shim could NOT be planted. Never fall
        // through to the system node pretending it worked — that silent
        // downgrade is exactly the bug this flag was added to fix.
        std::println(std::cerr,
                     "error: --bun: could not create the node shim at \"{}\"", run::BUN_NODE_DIR);
        return 1;
    }
    if (pkg.found) {
        // run_command.rs:732-755 — npm_package_* / npm_package_json.
        if (!pkg.packageName.empty()) env.emplace_back("npm_package_name", pkg.packageName);
        if (!pkg.packageVersion.empty()) env.emplace_back("npm_package_version", pkg.packageVersion);
        env.emplace_back("npm_package_json", pkg.packageJsonPath.string());
    }

    // ── 4. package.json script (run_command.rs:2470-2560) ───────────────────
    if (!skipScriptCheck && pkg.found) {
        if (const auto* entry = pkg.find(target)) {
            run::ScriptRunOptions opts{.silent = flags.silent};
            const std::filesystem::path& scriptCwd{pkg.packageJsonDir};

            // pre<name> — no passthrough args (run_command.rs:2517-2528)
            std::string preName{"pre"};
            preName.append(target);
            if (const auto* pre = pkg.find(preName)) {
                if (auto rc = run::run_package_script(pre->content, preName, scriptCwd, {}, env,
                                                      newPath, opts)) {
                    return *rc;
                }
            }

            // <name> — with passthrough (run_command.rs:2530-2540)
            if (auto rc = run::run_package_script(entry->content, target, scriptCwd, passthrough,
                                                  env, newPath, opts)) {
                return *rc;
            }

            // post<name> — no passthrough args (run_command.rs:2544-2555)
            std::string postName{"post"};
            postName.append(target);
            if (const auto* post = pkg.find(postName)) {
                if (auto rc = run::run_package_script(post->content, postName, scriptCwd, {}, env,
                                                      newPath, opts)) {
                    return *rc;
                }
            }
            return 0;
        }
    }

    // ── 5. module resolution → run the file (run_command.rs:2564-2620) ──────
    // Simplified against bun's full resolver: probe the target (and "./target")
    // as a path under cwd. Covers `bun run foo.ts` / `bun run src/index.ts`.
    if (!skipScriptCheck && !target.empty()) {
        std::filesystem::path candidate{cwd / target};
        if (std::filesystem::exists(candidate, ec) && !std::filesystem::is_directory(candidate, ec) &&
            loader_can_be_run(target)) {
            return run_script(target, passthrough);
        }
    }

    // ── 6. node_modules/.bin + $PATH binary (run_command.rs:2687-2723) ──────
    {
        std::string pathForWhich{newPath};
        if (binDirsOnly) {
            // Search only the prepended .bin dirs (PATH minus ORIGINAL_PATH).
            pathForWhich = originalPath.size() < newPath.size()
                               ? newPath.substr(0, newPath.size() - (originalPath.size() + 1))
                               : std::string{};
        }
        if (!pathForWhich.empty()) {
            if (auto exe = run::find_binary_in_path(pathForWhich, cwd.string(), target)) {
                int rc{run::run_binary(*exe, passthrough, env)};
                if (rc >= 0) return rc;
            }
        }
    }

    // ── 7. failure (run_command.rs:2726-2790) ──────────────────────────────
    if (flags.ifPresent) return 0;
    return report_run_target_not_found(target);
}

// `bun run --workspaces <script>` — run one package.json script in every
// workspace package of the enclosing project, excluding the root itself.
// PORT-SOURCE: bun cli/multi_run.rs:839-1001 (`ctx.workspaces` turns the filter
// engine into "every workspace member") + runtime/cli/filter_run.rs:852-866
// (empty match set → silent 0 under --if-present, else
// `No workspace packages have script "<name>"` on stderr + exit 1).
//
// DIVERGENCE: bun runs the members concurrently and prefixes each line with the
// package name; mbun runs them sequentially in workspace order. The scripts'
// own stdout is what the corpus asserts on, and sequential execution keeps the
// exit-code rule (first failure wins) deterministic.
int exec_run_workspaces(std::string_view target, std::span<const std::string_view> passthrough,
                        const RunFlags& flags) {
    namespace run = mbun::cli::run;
    namespace json = mbun::install::npm::json;
    std::error_code ec{};
    const std::filesystem::path cwd{std::filesystem::current_path(ec)};

    // The workspace root is the nearest enclosing package.json (bun resolves
    // `--workspaces` against the same root the run command resolved).
    const auto rootJsonPath{find_package_json(cwd)};
    std::vector<mbun::install::workspace_map::Entry> members;
    std::filesystem::path root{cwd};
    if (rootJsonPath) {
        root = rootJsonPath->parent_path();
        std::ifstream in{*rootJsonPath, std::ios::binary};
        std::string source{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
        auto doc{json::parse(source)};
        if (doc && doc->root) {
            auto collected{
                mbun::install::workspace_map::collect(root, doc->root->get("workspaces"))};
            if (!collected) {
                std::println(std::cerr, "error: {}", collected.error().message);
                return 1;
            }
            members = std::move(*collected);
        }
    }

    // filter_run.rs builds the script list first, then decides; a member without
    // the script is simply not in the list (it is never an error on its own).
    int exitCode{0};
    bool ran{false};
    for (const auto& member : members) {
        const std::filesystem::path dir{root / member.relPath};
        // multi_run.rs:885 — the root package is excluded under --workspaces.
        if (std::filesystem::equivalent(dir, root, ec)) continue;
        run::PackageScripts pkg{run::load_nearest_package_scripts(dir)};
        if (!pkg.found || pkg.packageJsonDir != dir || pkg.find(target) == nullptr) continue;
        std::filesystem::current_path(dir, ec);
        if (ec) continue;
        ran = true;
        // --if-present is a property of the *set* here, not of each member: the
        // member is known to have the script, so a failure must still surface.
        RunFlags memberFlags{flags};
        memberFlags.ifPresent = false;
        const int rc{exec_run_target(target, passthrough, memberFlags,
                                     /*allowFastRunForExtensions=*/false, /*binDirsOnly=*/false)};
        std::filesystem::current_path(cwd, ec);
        if (rc != 0 && exitCode == 0) exitCode = rc;
    }

    if (!ran) {
        if (flags.ifPresent) return 0;
        std::println(std::cerr, "error: No workspace packages have script \"{}\"", target);
        return 1;
    }
    return exitCode;
}

bool is_skippable_run_flag(std::string_view a) {
    // NOTE `--bun` / `-b` are NOT here: they used to be silently swallowed,
    // which made `bun run --bun <node-cli>` quietly execute on the SYSTEM node
    // while looking like it ran on mbun. They are parsed into
    // RunFlags::forceUsingBun by both flag loops below.
    static constexpr std::string_view kFlags[]{
        "--no-install",     "--prefer-offline", "--prefer-latest", "--silent",
        "--smol",           "--watch",          "--hot",           "--no-clear-screen",
        "--no-addons",      "--no-deprecation", "--no-warnings",   "--dns-result-order",
        // Boolean CA-source flags (bun Arguments.rs:286,1297): accept + run.
        "--use-system-ca",  "--use-openssl-ca", "--use-bundled-ca",
        // Consumed here; the value is picked up in main() before flag parsing
        // (it must reach resolve_entry_path, which run_script calls).
        "--preserve-symlinks-main"};
    if (a.starts_with("--install=") || a.starts_with("--conditions=") ||
        a.starts_with("--cwd=") || a.starts_with("--config=")) {
        return true;
    }
    for (std::string_view f : kFlags) {
        if (a == f) return true;
    }
    return false;
}

// ref: cli/mod.rs:854-863 `is_node` — a plain suffix test on the WHOLE argv[0]
// (NOT the basename), ported verbatim including that looseness.
bool is_node_argv0(std::string_view argv0) { return argv0.ends_with("node"); }

// Port of run_command.rs:2981-3040 `exec_as_if_node` (cli/mod.rs:952-958 routes
// here when argv[0] is `node`). This is how EVERY `#!/usr/bin/env node` shebang
// enters this binary once `--bun` has put <BUN_NODE_DIR>/node at the front of
// PATH — i.e. it is what makes `--bun` actually mean anything.
//
// Node-mode never consults package.json scripts and never treats the target as
// anything but a file: positionals[0] is resolved against cwd when relative
// (run_command.rs:3007-3028) and booted regardless of extension or shebang.
// Unknown flags must not warn (cli/mod.rs:953-955 clears
// WARN_ON_UNRECOGNIZED_FLAG) — node-mode must not reject node's own flags.
int exec_as_if_node(std::span<const std::string_view> args) {
    mbun::cli::run::set_pretend_to_be_node(true);

    std::size_t i{0};
    for (; i < args.size(); ++i) {
        const std::string_view a{args[i]};
        // `node -e <code>` / `-p <code>` (run_command.rs:2988-3000).
        if (a == "-e" || a == "--eval" || a == "-p" || a == "--print") {
            if (i + 1 >= args.size()) {
                std::println(std::cerr, "error: Missing code to evaluate");
                return 1;
            }
            std::vector<std::string> jsArgv{"node"};
            for (std::string_view rest : args.subspan(i + 2)) jsArgv.emplace_back(rest);
            mbun::jsc::runtime::set_argv(std::move(jsArgv));
            std::string code{args[i + 1]};
            if (a == "-p" || a == "--print") code = "console.log((() => (" + code + "))())";
            return mbun::jsc::runtime::run_eval(code);
        }
        // `node --version` prints the node compatibility claim, exactly like
        // node (single line, exit 0). The corpus asserts the exact string
        // (js/node/process/process.test.js expects "v26.3.0").
        if (a == "--version" || a == "-v") {
            std::println("v{}", mbun::cli::NODE_COMPAT_VERSION);
            return 0;
        }
        if (!a.starts_with("-") || a == "-") break;  // first positional == the script
        // Any other leading `-…` is a node flag mbun does not model; drop it
        // rather than mistake it for the script name.
        // GAP: a two-token node flag (`--require x`) eats its value here and the
        // value would be taken as the script. node accepts `--require=x` too, and
        // nothing currently exercised hits the two-token form.
    }

    if (i >= args.size()) {
        // run_command.rs:3046-3049 — wording is verbatim from the reference.
        std::println(std::cerr,
                     "error: Missing script to execute. Bun's provided 'node' cli wrapper does "
                     "not support a repl.");
        return 1;
    }

    // run_command.rs:3007-3028 — relative paths are joined onto cwd so argv[1]
    // and __filename are absolute, as in the reference.
    std::string target{args[i]};
    if (!target.empty() && target[0] != '/') {
        std::error_code ec{};
        std::filesystem::path abs{std::filesystem::current_path(ec) / target};
        if (!ec) target = abs.lexically_normal().string();
    }
    return run_script(target, args.subspan(i + 1));
}

} // namespace mbun::app
