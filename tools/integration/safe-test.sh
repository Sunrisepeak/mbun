#!/usr/bin/env bash
# safe-test.sh — run a command inside a memory/process/time-bounded systemd scope
# so a fork-deadlock, leak, or fork-bomb can NEVER freeze the whole machine.
#
# WHY: child_process/spawn tests can accumulate many hung heavyweight subprocesses
# (e.g. an OpenSSL+fork deadlock). Left unbounded they exhaust RAM → swap thrash →
# the machine locks up and reboots. A systemd --user scope caps the WHOLE process
# tree's memory (cgroup OOM-kills inside the scope, not the box), its task count,
# and its wall-clock runtime, then reaps the entire group.
#
# Usage:  tools/integration/safe-test.sh <timeout_sec> <cmd> [args...]
#   e.g.  tools/integration/safe-test.sh 30 ./app/cli/target/.../mbun test <file>
# Env overrides: SAFE_MEM (default 6G), SAFE_TASKS (default 128).
#
# COMPILING UNDER THIS WRAPPER NEEDS SAFE_MEM RAISED. `mcpp test -p modules/jsc`
# compiles before it runs, and 6G is not enough: the cgroup OOM-kills it and the
# wrapper reports **exit 124**, which reads exactly like a timeout. Two separate
# lanes lost ~50 minutes each to that misdiagnosis -- one retried at a two-hour
# bound, the other concluded the wrapper "reaps during the compile stage". With
# `SAFE_MEM=34G` it finishes normally. Raise SAFE_MEM for anything that builds;
# the 6G default is sized for RUNNING a single corpus test, not for a compile.
#
# ALWAYS use this (never a bare `mbun test <spawn-heavy-file>`) when a hang is
# possible. Exit code is the command's, or 124 on timeout kill.
set -u
TO="${1:?usage: safe-test.sh <timeout_sec> <cmd...>}"; shift
# The 6G default is sized for RUNNING one corpus test. A command that COMPILES
# needs far more, and getting that wrong is indistinguishable from a timeout: the
# cgroup OOM-kill surfaces as exit 124, the same code RuntimeMaxSec produces. Two
# lanes lost ~50 minutes each to exactly that, one retrying at a two-hour bound.
# So detect a build-shaped command and raise the default instead of documenting a
# footgun. An explicit SAFE_MEM always wins.
_default_mem=6G
case " $* " in
  *" mcpp "*|*" ninja "*|*" make "*|*" cmake "*|*" cc "*|*" c++ "*|*" g++ "*|*" clang++ "*)
    _default_mem=34G ;;
esac
MEM="${SAFE_MEM:-$_default_mem}"
if [ -z "${SAFE_MEM:-}" ] && [ "$_default_mem" != 6G ]; then
  echo "safe-test.sh: build-shaped command detected; MemoryMax=$_default_mem (override with SAFE_MEM)" >&2
fi
# 512, not 128: the corpus has tests that legitimately spawn hundreds of children
# (js/bun/spawn/spawn-many-teardown spawns 350), and a tight TasksMax fails them
# with fork() errors that look like mbun bugs. The freeze protection is MemoryMax
# + MemorySwapMax=0 + RuntimeMaxSec, which are unchanged: hung children now die by
# OOM-kill inside the scope rather than being pre-empted by a task cap.
TASKS="${SAFE_TASKS:-512}"
# TimeoutStopSec: SIGKILL shortly after RuntimeMaxSec's SIGTERM, so children
# that ignore SIGTERM (node child_process suites) still die. setsid: the
# workload gets its own session, so a test that signals its whole process
# group (kill(0, SIGABRT)) cannot kill this shell or the harness above it.
setsid -w systemd-run --user --scope --quiet --collect \
  -p MemoryMax="$MEM" -p MemorySwapMax=0 -p TasksMax="$TASKS" \
  -p RuntimeMaxSec="$TO" -p TimeoutStopSec=3 \
  -- "$@"
rc=$?
# systemd returns 143 (SIGTERM) when RuntimeMaxSec fires; normalize to 124 (timeout).
[ "$rc" = 143 ] && rc=124
exit $rc
