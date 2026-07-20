#!/usr/bin/env python3
"""Smoke-test every runnable example app: boot it, hit it over HTTP, reap it.

`examples/` is the project's public first impression (README quick start), yet
nothing checked that the demos still boot -- breakage was only ever found by
hand. This runner starts each example server through the shared safety layer
(`bounded_run.BoundedServer`), waits for it to accept a request on the example's
port, asserts a 2xx, then kills the whole process tree.

Every server runs SEQUENTIALLY on purpose: the examples all hardcode port 3000
(that is what the README tells a reader to open), so a parallel round would just
race for the listen socket and report EADDRINUSE as an app failure.

Usage:
    python3 tools/integration/smoke_examples.py --bin <mbun> --out target/integration/smoke
    python3 tools/integration/smoke_examples.py --bin <mbun> --app bun/http-server
"""

from __future__ import annotations

import argparse
import json
import socket
import sys
import time
import urllib.error
import urllib.request
from dataclasses import asdict, dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from bounded_run import BoundedRun, BoundedServer, ensure_disk_headroom

# name -> how to boot it. `install` is the npm-dependency bootstrap for the
# framework demos; it is skipped when node_modules is already present so a local
# round stays offline and fast.
DEFAULT_MANIFEST: list[dict] = [
    {"name": "node/http-server", "args": ["examples/node/http-server.js"]},
    {"name": "bun/http-server", "args": ["examples/bun/http-server.ts"]},
    {
        "name": "node/express",
        "args": ["--cwd", "examples/node/express", "server.js"],
        "install": ["--cwd", "examples/node/express", "install"],
        "node_modules": "examples/node/express/node_modules",
    },
    {
        "name": "bun/elysia",
        "args": ["--cwd", "examples/bun/elysia", "server.ts"],
        "install": ["--cwd", "examples/bun/elysia", "install"],
        "node_modules": "examples/bun/elysia/node_modules",
    },
]

DEFAULT_PORT = 3000
DEFAULT_STARTUP_TIMEOUT = 25.0
DEFAULT_INSTALL_TIMEOUT = 300.0


@dataclass(frozen=True)
class Result:
    name: str
    classification: str
    status: int
    duration_ms: int
    detail: str
    log: str


def port_is_free(port: int) -> bool:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            probe.bind(("127.0.0.1", port))
        except OSError:
            return False
    return True

def wait_for_port_free(port: int, timeout: float) -> bool:
    """A just-killed server's socket can linger; give it a moment before the next app."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if port_is_free(port):
            return True
        time.sleep(0.2)
    return port_is_free(port)


def probe(url: str, timeout: float) -> tuple[int, str]:
    try:
        with urllib.request.urlopen(url, timeout=timeout) as response:
            return response.status, ""
    except urllib.error.HTTPError as error:  # served, but not 2xx
        return error.code, f"HTTP {error.code}"
    except (urllib.error.URLError, OSError, TimeoutError) as error:
        return 0, str(error)


def dependency_state(app: dict, binary: Path, root: Path, output_dir: Path,
                     install: bool, install_timeout: float) -> tuple[str, str]:
    """Return (state, detail): "ready", "skip" (deps absent, install not asked
    for) or "error"."""
    modules = app.get("node_modules")
    if not app.get("install") or (modules and (root / modules).is_dir()):
        return "ready", ""
    if not install:
        # `mbun install` currently does not finish for these fixtures (it ran
        # past a 880s bound on examples/bun/elysia), so bootstrapping is opt-in:
        # a missing node_modules is reported and skipped rather than hanging a
        # smoke round. Pass --install once install is fixed.
        return "skip", "node_modules missing and --install not given"
    runner = BoundedRun(output_dir / "logs" / f"{app['name'].replace('/', '_')}-install.log")
    bounded = runner.run([str(binary), *app["install"]], timeout=install_timeout, cwd=root)
    if bounded.exit_code != 0:
        return "error", f"install exited {bounded.exit_code}"
    return "ready", ""


def smoke_one(
    app: dict, binary: Path, root: Path, output_dir: Path, port: int, startup_timeout: float,
    install: bool = False, install_timeout: float = DEFAULT_INSTALL_TIMEOUT,
) -> Result:
    started = time.monotonic()
    name = app["name"]
    relative_log = Path("logs") / f"{name.replace('/', '_')}.log"
    log_path = output_dir / relative_log

    def done(classification: str, status: int, detail: str) -> Result:
        return Result(
            name, classification, status, round((time.monotonic() - started) * 1000),
            detail, str(relative_log),
        )

    state, detail = dependency_state(app, binary, root, output_dir, install, install_timeout)
    if state == "error":
        return done("install-error", 0, detail)
    if state == "skip":
        return done("skipped-no-deps", 0, detail)

    if not wait_for_port_free(port, timeout=10.0):
        return done("port-busy", 0, f"port {port} still in use before start")

    server = BoundedServer(log_path, private_tmp=output_dir / "tmp" / name.replace("/", "_"))
    url = f"http://127.0.0.1:{port}/"
    # RuntimeMaxSec is a backstop only: the app is killed as soon as it answers.
    with server.start([str(binary), *app["args"]], max_lifetime=startup_timeout + 30.0,
                      cwd=root) as process:
        deadline = time.monotonic() + startup_timeout
        status, detail = 0, "server never accepted a connection"
        while time.monotonic() < deadline:
            if process.poll() is not None:
                return done("start-error", 0, f"exited {process.returncode} before serving")
            status, detail = probe(url, timeout=2.0)
            if status:
                break
            time.sleep(0.2)

    if not status:
        return done("no-response", 0, detail)
    if not 200 <= status < 300:
        return done("bad-status", status, detail)
    return done("ok", status, "")


def write_outputs(output_dir: Path, results: list[Result]) -> dict:
    output_dir.mkdir(parents=True, exist_ok=True)
    columns = ["name", "classification", "status", "duration_ms", "detail", "log"]
    with (output_dir / "results.tsv").open("w", encoding="utf-8") as stream:
        stream.write("\t".join(columns) + "\n")
        for result in results:
            values = asdict(result)
            stream.write("\t".join(str(values[column]) for column in columns) + "\n")

    categories: dict[str, int] = {}
    for result in results:
        categories[result.classification] = categories.get(result.classification, 0) + 1
    summary = {
        "apps": len(results),
        "ok": categories.get("ok", 0),
        "skipped": categories.get("skipped-no-deps", 0),
        "categories": dict(sorted(categories.items())),
    }
    (output_dir / "summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return summary


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", required=True, type=Path, help="mbun executable")
    parser.add_argument("--root", default=Path.cwd(), type=Path, help="repository root")
    parser.add_argument("--out", default=Path("target/integration/smoke"), type=Path)
    parser.add_argument("--app", action="append", help="only these apps (repeatable)")
    parser.add_argument("--manifest", type=Path, help="JSON app list (self-tests)")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--startup-timeout", type=float, default=DEFAULT_STARTUP_TIMEOUT)
    parser.add_argument(
        "--install", action="store_true",
        help="bootstrap an app's npm dependencies with `mbun install` when "
             "node_modules is missing (off by default: install does not "
             "currently finish for the framework demos)",
    )
    parser.add_argument("--install-timeout", type=float, default=DEFAULT_INSTALL_TIMEOUT)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    ensure_disk_headroom()
    root = args.root.resolve()
    binary = args.bin.resolve()
    output_dir = args.out.resolve()

    apps = json.loads(args.manifest.read_text(encoding="utf-8")) if args.manifest else DEFAULT_MANIFEST
    if args.app:
        wanted = set(args.app)
        unknown = wanted - {app["name"] for app in apps}
        if unknown:
            raise SystemExit(f"unknown app(s): {', '.join(sorted(unknown))}")
        apps = [app for app in apps if app["name"] in wanted]
    if not apps:
        raise SystemExit("no example apps selected")

    output_dir.mkdir(parents=True, exist_ok=True)
    results: list[Result] = []
    for app in apps:
        result = smoke_one(app, binary, root, output_dir, args.port, args.startup_timeout,
                           args.install, args.install_timeout)
        results.append(result)
        marker = {"ok": "ok", "skipped-no-deps": "skip"}.get(
            result.classification, f"FAIL[{result.classification}]")
        print(f"{marker:>16}  {result.name}  {result.status or '-'}  "
              f"{result.duration_ms}ms  {result.detail}".rstrip())

    summary = write_outputs(output_dir, results)
    print(json.dumps(summary, sort_keys=True))
    # A skipped app is not a pass, but it is not a failure of the app either;
    # it is reported and does not redden the round.
    return 0 if summary["ok"] + summary["skipped"] == summary["apps"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
