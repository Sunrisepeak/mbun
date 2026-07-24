#!/usr/bin/env python3
"""bounded_run — the shared safety layer for every tool that executes tests,
benchmarks, or debug workloads on this machine.

Any harness that runs untrusted/spawn-heavy workloads (corpus runners, bench
drivers, bisect scripts) MUST go through `bounded_run()` instead of a bare
subprocess call. Each protection below exists because its absence has actually
frozen this machine or killed a measurement run:

1. systemd --user scope with MemoryMax / MemorySwapMax=0 / TasksMax /
   RuntimeMaxSec: a fork-deadlock OOM-kills inside the scope instead of
   swap-thrashing the whole box.
2. TimeoutStopSec after RuntimeMaxSec's SIGTERM: tests whose children ignore
   SIGTERM (node child_process suites) still get SIGKILLed.
3. start_new_session: tests that signal their whole process group
   (kill(0, SIGABRT), process.kill(-pid)) can no longer kill the harness —
   this silently murdered three corpus runs before it was isolated.
4. stdout straight to the log FILE, never a pipe: orphaned grandchildren
   holding a pipe's write end stall subprocess.run() past its timeout and
   wedge the whole worker pool.
5. Private per-run TMPDIR, deleted afterwards: killed tests leak tmpdir
   debris; one corpus round left ~500k entries in /tmp and filled the disk.
6. Disk-space guard: refuse to launch when the disk is nearly full, so a
   runaway workload degrades into a clear error instead of a dead machine.

Where a `systemd --user` scope cannot be created (CI runners have systemd but
no session bus for the job user), protections 2-6 still apply and the workload
degrades to a timeout-bounded subprocess rather than failing to launch at all.

Usage:
    from bounded_run import BoundedRun, ensure_disk_headroom
    ensure_disk_headroom()                       # once, at harness startup
    result = BoundedRun(log_path).run(cmd, timeout=15.0)
    result.exit_code / result.timed_out / result.oom_killed

A workload that never exits on its own (an example server under a smoke test)
uses the same protections through `BoundedServer`:

    with BoundedServer(log_path).start(cmd, max_lifetime=30.0) as server:
        ...                                       # probe it
    # the whole process tree is dead here, scope collected
"""

from __future__ import annotations

import contextlib
import functools
import os
import shutil
import signal
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path


@functools.cache
def have_systemd_scope() -> bool:
    """Whether a `systemd-run --user --scope` actually launches here.

    The binary merely existing is not enough: CI runners ship systemd but give
    the job user no session bus, where every scope launch dies with "Failed to
    connect to bus" -- which would turn every bounded workload into a spurious
    failure instead of degrading to a plain timeout-bounded subprocess. Probe
    once, cache, and fall back when the probe fails.
    """
    if shutil.which("systemd-run") is None:
        return False
    try:
        probe = subprocess.run(
            ["systemd-run", "--user", "--scope", "--quiet", "--collect", "--", "true"],
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            timeout=30, check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        return False
    return probe.returncode == 0


# Defaults shared by every harness; override per-call only with a reason.
DEFAULT_MEMORY_MAX = "4G"
DEFAULT_TASKS_MAX = 512  # spawn-many tests legitimately fork hundreds of children
DEFAULT_KILL_GRACE_SEC = 3
MIN_DISK_HEADROOM_BYTES = 2 * 1024**3


@functools.cache
def suppress_core_dumps() -> bool:
    """Set RLIMIT_CORE to 0 for this process and everything it spawns.

    A corpus round runs thousands of files and a large minority abort or
    segfault. This host pipes core_pattern into apport, so EVERY crash forks a
    crash reporter that reads the whole core through a pipe -- during a run that
    is a continuous tax in CPU, RAM and disk, on top of the cores themselves.
    It also actively obstructs debugging: apport swallows the core, which is
    already recorded in the changelog as the reason a heap use-after-free could
    not be chased.

    Cores are not this project's debugging path anyway -- modules/crash_handler
    installs a chaining handler that prints a symbolised backtrace in-process,
    which survives here because it runs *before* the kernel dumps.

    Set once in the parent so children inherit it; preexec_fn would run per-fork
    and is unsafe from the thread pools the runners use. Escape hatch:
    MBUN_ALLOW_CORE=1 for someone deliberately collecting a core.
    """
    if os.environ.get("MBUN_ALLOW_CORE") == "1":
        return False
    try:
        import resource

        _, hard = resource.getrlimit(resource.RLIMIT_CORE)
        resource.setrlimit(resource.RLIMIT_CORE, (0, hard))
    except (ImportError, OSError, ValueError):
        return False
    return True


def force_rmtree(path: Path) -> None:
    """rmtree that also removes chmod-000 debris left by permission tests.

    Plain rmtree(ignore_errors=True) silently leaves unreadable directories
    behind; ones under target/ later crashed mcpp's recursive source scan and
    broke every subsequent build. Chmod on the way down, then delete.
    """
    if not path.exists():
        return
    for root, dirs, _files in os.walk(path):
        for name in dirs:
            try:
                os.chmod(os.path.join(root, name), 0o700)
            except OSError:
                pass
    shutil.rmtree(path, ignore_errors=True)


def ensure_disk_headroom(path: Path | str = "/", minimum: int = MIN_DISK_HEADROOM_BYTES) -> None:
    """Fail fast when the filesystem is nearly full instead of filling it."""
    usage = shutil.disk_usage(str(path))
    if usage.free < minimum:
        raise SystemExit(
            f"bounded_run: only {usage.free / 1024**2:.0f}MB free on {path} "
            f"(need {minimum / 1024**2:.0f}MB); refusing to run — clean up first"
        )


@dataclass(frozen=True)
class BoundedResult:
    exit_code: int
    timed_out: bool
    oom_killed: bool


class BoundedRun:
    """One bounded workload execution writing its output to `log_path`."""

    def __init__(self, log_path: Path, private_tmp: Path | None = None) -> None:
        self.logPath = log_path
        self.privateTmp = private_tmp

    def run(
        self,
        cmd: list[str],
        timeout: float,
        cwd: Path | str | None = None,
        env: dict[str, str] | None = None,
        memory_max: str = DEFAULT_MEMORY_MAX,
        tasks_max: int = DEFAULT_TASKS_MAX,
    ) -> BoundedResult:
        suppress_core_dumps()
        full_env = dict(env if env is not None else os.environ)
        if self.privateTmp is not None:
            self.privateTmp.mkdir(parents=True, exist_ok=True)
            tmp = str(self.privateTmp)
            full_env.update({"TMPDIR": tmp, "TMP": tmp, "TEMP": tmp})

        if have_systemd_scope():
            cmd = [
                "systemd-run", "--user", "--scope", "--quiet", "--collect",
                "-p", f"MemoryMax={memory_max}", "-p", "MemorySwapMax=0",
                "-p", f"TasksMax={tasks_max}",
                "-p", f"RuntimeMaxSec={int(timeout) + DEFAULT_KILL_GRACE_SEC}",
                "-p", f"TimeoutStopSec={DEFAULT_KILL_GRACE_SEC}", "--",
            ] + cmd

        timed_out = False
        oom_killed = False
        exit_code = 0
        self.logPath.parent.mkdir(parents=True, exist_ok=True)
        with self.logPath.open("w", encoding="utf-8") as log_stream:
            try:
                completed = subprocess.run(
                    cmd,
                    cwd=cwd,
                    env=full_env,
                    stdin=subprocess.DEVNULL,
                    stdout=log_stream,
                    stderr=subprocess.STDOUT,
                    start_new_session=True,
                    timeout=timeout + 8,
                    check=False,
                )
                exit_code = completed.returncode
                if exit_code in (-signal.SIGKILL, 137):
                    oom_killed = True
                elif have_systemd_scope() and exit_code in (-signal.SIGTERM, 143):
                    timed_out = True
                    exit_code = 124
            except subprocess.TimeoutExpired:
                timed_out = True
                exit_code = 124

        if self.privateTmp is not None:
            force_rmtree(self.privateTmp)
        if timed_out:
            self.append_note(f"[bounded-run] timeout after {timeout:.3f}s")
        elif oom_killed:
            self.append_note(f"[bounded-run] killed (SIGKILL) -- MemoryMax={memory_max}/OOM")
        return BoundedResult(exit_code, timed_out, oom_killed)

    def append_note(self, note: str) -> None:
        with self.logPath.open("a", encoding="utf-8") as log_stream:
            log_stream.write(f"\n{note}\n")

    def read_output(self) -> str:
        return self.logPath.read_text(encoding="utf-8", errors="replace")


class BoundedServer:
    """A bounded workload that is *supposed* to keep running while you probe it.

    `BoundedRun.run()` is run-to-completion, which a server smoke test can never
    be: the server only exits when we kill it. The protections are identical and
    all still needed here:

    - the systemd scope caps the tree's memory and reaps EVERY descendant on
      exit -- a server that forks workers and then crashes otherwise leaves
      children holding the listen port, and every later smoke run fails with
      EADDRINUSE against a ghost;
    - `RuntimeMaxSec` is the backstop for the harness itself dying (Ctrl-C, an
      exception between start and stop): the scope still expires on its own;
    - `start_new_session` keeps a server that signals its process group from
      killing this harness;
    - stdout goes straight to the log file, so a probe never blocks on a full
      pipe buffer while the server waits to write.
    """

    def __init__(self, log_path: Path, private_tmp: Path | None = None) -> None:
        self.logPath = log_path
        self.privateTmp = private_tmp

    @contextlib.contextmanager
    def start(
        self,
        cmd: list[str],
        max_lifetime: float,
        cwd: Path | str | None = None,
        env: dict[str, str] | None = None,
        memory_max: str = DEFAULT_MEMORY_MAX,
        tasks_max: int = DEFAULT_TASKS_MAX,
    ):
        """Launch `cmd` in the background; yield the Popen; always reap the tree."""
        full_env = dict(env if env is not None else os.environ)
        if self.privateTmp is not None:
            self.privateTmp.mkdir(parents=True, exist_ok=True)
            tmp = str(self.privateTmp)
            full_env.update({"TMPDIR": tmp, "TMP": tmp, "TEMP": tmp})

        if have_systemd_scope():
            cmd = [
                "systemd-run", "--user", "--scope", "--quiet", "--collect",
                "-p", f"MemoryMax={memory_max}", "-p", "MemorySwapMax=0",
                "-p", f"TasksMax={tasks_max}",
                "-p", f"RuntimeMaxSec={int(max_lifetime) + DEFAULT_KILL_GRACE_SEC}",
                "-p", f"TimeoutStopSec={DEFAULT_KILL_GRACE_SEC}", "--",
            ] + cmd

        self.logPath.parent.mkdir(parents=True, exist_ok=True)
        log_stream = self.logPath.open("w", encoding="utf-8")
        process = subprocess.Popen(
            cmd,
            cwd=cwd,
            env=full_env,
            stdin=subprocess.DEVNULL,
            stdout=log_stream,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        try:
            yield process
        finally:
            self.stop_(process)
            log_stream.close()
            if self.privateTmp is not None:
                force_rmtree(self.privateTmp)

    def stop_(self, process: subprocess.Popen) -> None:
        """SIGTERM the whole session, then SIGKILL what ignored it."""
        if process.poll() is not None:
            return
        for sig in (signal.SIGTERM, signal.SIGKILL):
            with contextlib.suppress(ProcessLookupError, PermissionError):
                os.killpg(os.getpgid(process.pid), sig)
            deadline = time.monotonic() + DEFAULT_KILL_GRACE_SEC
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    return
                time.sleep(0.05)
        with contextlib.suppress(subprocess.TimeoutExpired):
            process.wait(timeout=DEFAULT_KILL_GRACE_SEC)

    def read_output(self) -> str:
        if not self.logPath.exists():
            return ""
        return self.logPath.read_text(encoding="utf-8", errors="replace")
