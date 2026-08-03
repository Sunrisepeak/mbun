# mbun | [Rewrite Bun in MC++](https://github.com/Sunrisepeak/mbun/pull/1)

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

Measured 2026-08-03 by the runners in `tools/integration/`, both corpora executed
in full on **one frozen binary** (`f65f960b7ac7d212…`). Nothing unsupported is
counted as a pass.

| Target | Result | Rate |
| --- | ---: | ---: |
| Node.js native tests (`compat/node/test/parallel`) | 3,267 / 4,433 files pass | 73.7% |
| Node.js native tests, excluding files that skip themselves | 3,267 / 3,919 files pass | 83.4% |
| Bun native full corpus (`compat/bun/test`) | 1,076 / 1,902 files fully green | 56.6% |
| Bun corpus, excluding files with no runnable tests here | 1,076 / 1,806 files fully green | 59.6% |
| Both corpora combined | 4,343 / 6,335 files | 68.6% |
| Both corpora combined, runnable only | 4,343 / 5,725 files | 75.9% |
| Elysia test suite (measured 2026-07-30, not re-run since) | 1,522 pass / 3 fail | 99.8% |

The "excluding" rows are not a softer denominator: a file counts as excluded only
when it reports no runnable tests on this platform — it skipped itself, it has no
tests upstream, or it needs a service this machine does not provide. Those
denominators **grew** this round (Node 3,898 → 3,919) because 21 files that used
to skip themselves now run.

**What stands between here and 100%, counted rather than estimated.** 514 Node
files decline to run, and the reasons are not interchangeable:

| Self-skip reason | Files | Nature |
| --- | ---: | --- |
| QUIC not enabled | 238 | no `node:quic` subsystem; node's own build disables it by default too |
| V8 inspector disabled | 180 | needs a CDP backend; JSC speaks the WebInspector protocol instead |
| missing ESLint | 29 | node's own lint tests; ESLint is not vendored here |
| no OpenSSL CLI | 10 | crypto build configuration |
| requires Amaro | 6 | TypeScript loader |
| Windows-specific | 6 | unreachable on Linux by construction |
| other (Temporal, debug-only builds, cipher availability) | 45 | assorted |

The two large blocks are priced. QUIC needs no protocol work — node builds it on
vendored ngtcp2, and the linked OpenSSL 3.5.1 already exports everything that
backend calls — but it does need node's ~19k-line binding re-expressed against
JSC. The inspector block has **no per-file gate**: all of it hangs on one global
capability flag, so no partial slice can score, and flipping it early converts
skips into failures.

## Related projects

- [mcpp](https://github.com/mcpp-community/mcpp) — the modular C++26 build tool and package manager mbun is built with
- [Bun](https://github.com/oven-sh/bun) — the JavaScript runtime whose APIs, `bun:test` runner, and native test corpus mbun targets ([bun.com](https://bun.com))
- [Node.js](https://github.com/nodejs/node) — the `node:*` built-ins, module systems, and native test corpus mbun targets ([nodejs.org](https://nodejs.org))
- [Express](https://github.com/expressjs/express) — Node.js web framework used in `examples/node/express` ([expressjs.com](https://expressjs.com))
- [Elysia](https://github.com/elysiajs/elysia) — Bun-native web framework used in `examples/bun/elysia` ([elysiajs.com](https://elysiajs.com))
- [WebKit JavaScriptCore](https://github.com/WebKit/WebKit/tree/main/Source/JavaScriptCore) — the JavaScript engine embedded by mbun (as by Bun)

## Limitations

- Linux x86_64 is the primary validated target: it is the only one the corpus
  numbers above were measured on, and the only one whose CI runs the workspace
  unit tests and the example-app smoke.
- macOS arm64 builds and runs. CI compiles the whole workspace and executes the
  binary on `macos-latest`; the wider test steps there are still being observed
  and are not yet gates. The numbers above have not been re-measured on it, so
  treat them as Linux figures until they are.
- Windows is not built or tested. The source carries `_WIN32` branches, but
  nothing has ever been compiled for it here, so that is intent rather than
  support.
- JavaScriptCore and native dependencies are built through mcpp packages.
- Compatibility varies by API, package, and execution mode; passing the demos does not imply full Bun or Node.js compatibility.
- No prebuilt binary or stable release package is provided yet.

## License

mbun code is licensed under [MIT](LICENSE) — the same license as Node.js and Bun. Third-party components (JavaScriptCore, OpenSSL, …) and the Bun / Node.js test corpora keep their upstream licenses — they are listed together at the end of [LICENSE](LICENSE).
