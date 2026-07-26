# mbun | [Rewrite Bun in MC++](https://github.com/Sunrisepeak/mbun/pull/1) - Just for Fun

[中文文档](README.zh-CN.md)

`mbun` is a modular C++26 JavaScript runtime built with [mcpp](https://github.com/mcpp-community/mcpp) — a rewrite of Bun in MC++ that is compatible with both Node.js and Bun. Bun APIs and `bun:test`, `node:*` built-ins, CommonJS/ES modules, and the npm package ecosystem run in one executable.

**Why rewrite Bun in MC++?**

1. **"Rewrite" performance art, reprised** — [Rewrite Bun in Rust](https://github.com/oven-sh/bun/pull/30412)
2. **[mcpp](https://github.com/mcpp-community/mcpp) build tool + modular C++ engineering validation** — stress-testing the [mcpp](https://github.com/mcpp-community/mcpp) build tool, C++26, and C++ Modules for real-world usability in a 90+-member module workspace.
3. **Validating and exploring AI-agent-driven development & collaboration** — AI agents participate in the whole development and collaboration loop, as an end-to-end experiment: validating ideas, surfacing the problems you actually hit, and finding out how humans and agents work together on a large codebase.

> [!CAUTION]
> mbun is an experimental, source-first project — do not use it in production yet.
> The goal is useful compatibility in real applications, measured against the native
> Bun and Node.js test corpora rather than API checklists.

## Quick start

### 1. Build from source and run a minimal script

Install [xlings](https://github.com/d2learn/xlings), then install mcpp with it (toolchains are downloaded automatically into an isolated sandbox):

```bash
curl -fsSL https://raw.githubusercontent.com/openxlings/xlings/main/tools/other/quick_install.sh | bash
xlings install mcpp -y
```

Build mbun and run the smallest example:

```bash
git clone https://github.com/Sunrisepeak/mbun
cd mbun
git submodule update --init --recursive
mcpp build
mcpp run -- --version   # mbun 2026.07.18.0 + the compatible bun/node versions
mcpp run -- examples/common/hello.ts
```

### 2. Minimal Node.js and Bun examples

The same executable runs both API families natively:

```bash
# Node.js: node:http built-in + CommonJS
mcpp run -- examples/node/http-server.js

# Bun: Bun.serve + TypeScript
mcpp run -- examples/bun/http-server.ts
```

Each starts a server on <http://127.0.0.1:3000/>.

### 3. Well-known framework demos: Express and Elysia

[Express](https://github.com/expressjs/express) is the most widely used Node.js web framework. mbun installs its npm dependencies itself and runs it:

```bash
mcpp run -- --cwd examples/node/express install
mcpp run -- --cwd examples/node/express server.js
```

[Elysia](https://github.com/elysiajs/elysia) is a popular Bun-native web framework, running on the same executable:

```bash
mcpp run -- --cwd examples/bun/elysia install
mcpp run -- --cwd examples/bun/elysia server.ts
```

Open <http://127.0.0.1:3000/> to see each demo page.

## Contributing & collaboration — AI Agent open-source spec (hagent)

mbun's goal #3 is to explore AI-agent-driven development and collaboration. That
model is written up as **hagent** — an AI Agent open-source collaboration spec
(**h**uman + **agent**) that lives in [`hagent/`](hagent/) and is dogfooded here.
Its core thesis: **agents are the execution engine; the builder — the human who
runs them — creates and judges** (architecture, decisions, taste, norms). Every
action is attributed to a builder, and no change is "done" without reproducible
evidence.

### Ways to contribute

Everything goes through your coding agent — start it in the repo root and it
auto-loads `AGENTS.md` and the relevant skills, so it already knows the spec.
Pick the level that fits you, and always review what the agent produced before
opening a PR.

1. **Report & discuss** — hit a bug or have an idea? Have your agent file an
   issue or discussion following the reporting SOP: software versions, error
   output, initial analysis, and references — with local privacy scrubbed
   (usernames, tokens). → [`issue-reporting`](.agents/skills/issue-reporting/SKILL.md)
2. **Verify & review** — reproduce reported bugs and confirm whether they're
   real, add findings; help verify issues and PRs; review others' PRs.
3. **Develop** — pick a task and build it, issue-first (bugfix / refactor / new
   feature — new features need an issue discussion and a design doc under
   `.agents/docs/`). → [`dev-process`](.agents/skills/dev-process/SKILL.md),
   [`tdd-workflow`](.agents/skills/tdd-workflow/SKILL.md)

More: [hagent overview](hagent/README.md) · [charter](hagent/charter.md) ·
[contributing](hagent/contributing.md) · [agents](hagent/agents.md) ·
[labels](hagent/labels.md).

### Project maintainers

Elevated trust is a ladder — humans and agents earn it by the same rules
(charter [§4](hagent/charter.md#4-permission-tiers)). Tiers above committer are
still being defined.

| Role | Permission | How to apply | Holders |
| --- | --- | --- | --- |
| **Triager** | Triage, label, verify issues & PRs, run CI | Self-apply after ≥ 3 merged PRs | — |
| **Committer** | Write (push / merge) | Module background + significant contributions; maintainer approval | — |
| **Module maintainer** | A module's review & decisions | — | — |
| **Project maintainer** | Governance, protected surfaces | — | [@sunrisepeak](https://github.com/sunrisepeak) |

## Compatibility data

Source-snapshot measurements (2026-07-21) against the upstream corpora pinned as submodules under `compat/`, produced by the runners in `tools/integration/`. Unsupported cases are never counted as passes, and these are not release guarantees:

| Target | Result | Rate |
| --- | ---: | ---: |
| Bun native test corpus (`compat/bun/test`) | 885 / 1,902 files fully green *(round-7/8 snapshot — stale, see below)* | 46.5% |
| Bun native tests, test level | 32,190 pass / 17,254 fail of 52,454 run *(same stale snapshot)* | 61.4% |
| Node.js native tests (`compat/node/test/parallel`) | 2,591 / 4,433 files pass (direct execution) | 58.4% |
| Node.js native tests, excluding files that skip themselves | 2,591 / 3,881 files pass | 66.8% |
| Elysia test suite | 1,522 pass / 3 fail | 99.8% |

File-level "green" means every executed test in the file passed and the file reported no error outside a test; it is stricter than an API checklist and lower than test-level pass rates. Files that declare no runnable test, files whose every test is skipped, and files needing a service this environment lacks (MySQL, Redis, the npm registry) are separate buckets and never count as passes. Node.js files run directly through mbun (exit 0 = pass) without Node's own harness services, so that figure is honest file-level coverage, not API completion.

**The two Bun rows are a stale round-7/8 snapshot and are not contemporaneous with the Node.js rows.** They are the last full Bun-corpus measurement taken, kept for that reason and not because they still hold — rounds 9 and 10 landed 20+ tasks over shared code since. There is direct evidence the number has moved in both directions: the deep-equality alignment deliberately reclassified 22 files in Bun's own deep-equal suite as `ahead-of-reference` (mbun is *more* correct than Bun there, and the runner now scores that as its own bucket rather than as a failure), while the request-smuggling fix took Bun's own `request-smuggling.test.ts` from 53 to 61 passing. A full re-measure is owed.

**The Node.js figures were previously overstated and have been corrected downward at the source.** Two measurement defects were found and fixed:

- **Self-skips were counted as passes.** Node's `common.skip()` prints `1..0 # Skipped:` and exits 0, so exit-code-only classification could not tell "ran everything and passed" from "declined to run because this runtime lacks the feature". 1,527 of the 4,433 files can take a skip path. They now land in a `skipped` bucket and never count as passes — which is why the second row excludes 542 files rather than the 351 it used to.
- **`assert.throws` ignored its error argument.** `assert.throws(fn, { code: 'ERR_X' })` passed for *any* throw, and `assert.throws(fn, common.expectsError({…}))` never called the validator. Fixing it removed 126 passes from the figure below; a random 25 of those were checked individually and all 25 pass again the moment the broken matcher is restored, confirming they were verifying nothing.
- **`common.mustCall` was never enforced.** Node registers its verifier inside `process.on('exit')`, which mbun did not fire, so an under-called `mustCall(fn, 2)` still exited 0. At the time, 946 of the then-1,533 passing files used `mustCall*` — their central assertion had never run. `process.on('exit')` now fires and the event loop no longer swallows exceptions thrown inside callbacks.

The previously published 44.5% was a product of both defects and was never real. Measured with the corrected runner on the same machine, the comparable prior figure is **38.2%**, and the current figure is **58.4%** strict / **66.8%** excluding self-skips. Timeouts fell from 583 files to 125 over the same period, so a file that used to hang for 15 seconds now reports a real, diagnosable failure. Expect the strict rate to keep moving in both directions as more verification becomes real. Details, the full estimate-vs-actual record, and how to reproduce: [`compat/README.md`](compat/README.md).

## Related projects

- [mcpp](https://github.com/mcpp-community/mcpp) — the modular C++26 build tool and package manager mbun is built with
- [Bun](https://github.com/oven-sh/bun) — the JavaScript runtime whose APIs, `bun:test` runner, and native test corpus mbun targets ([bun.com](https://bun.com))
- [Node.js](https://github.com/nodejs/node) — the `node:*` built-ins, module systems, and native test corpus mbun targets ([nodejs.org](https://nodejs.org))
- [Express](https://github.com/expressjs/express) — Node.js web framework used in `examples/node/express` ([expressjs.com](https://expressjs.com))
- [Elysia](https://github.com/elysiajs/elysia) — Bun-native web framework used in `examples/bun/elysia` ([elysiajs.com](https://elysiajs.com))
- [WebKit JavaScriptCore](https://github.com/WebKit/WebKit/tree/main/Source/JavaScriptCore) — the JavaScript engine embedded by mbun (as by Bun)

## Limitations

- Linux x86_64 is the primary validated target.
- JavaScriptCore and native dependencies are built through mcpp packages.
- Compatibility varies by API, package, and execution mode; passing the demos does not imply full Bun or Node.js compatibility.
- No prebuilt binary or stable release package is provided yet.

## License

mbun code is licensed under [MIT](LICENSE) — the same license as Node.js and Bun. Third-party components (JavaScriptCore, OpenSSL, …) and the Bun / Node.js test corpora keep their upstream licenses — they are listed together at the end of [LICENSE](LICENSE).
