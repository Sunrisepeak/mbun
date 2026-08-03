#!/usr/bin/env bash
# Self-test for failure_signature.py.
#
# The property under test is CONFIDENCE DISCIPLINE, not extraction coverage. A
# cause extractor that labels everything it matched as a mechanism is worse than
# no extractor, because the next round budgets against it. This project has made
# that mistake five times by hand, and the tool made it twice on its own first
# run: `expect(x).toBe(expected)` was read as the literal "expected" and 223
# files were reported as one CLASS, and on the node side a bare "AssertionError"
# inflated the attributed share from 65% to 84%.
#
# So: a signature that names a CONTRACT must be CAUSE/CLASS, and one that only
# says an assertion failed must be MANIFESTATION.
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/../../.." && pwd)
tool="$repo_root/tools/integration/failure_signature.py"

pass() { printf 'ok   - %s\n' "$1"; }
fail() { printf 'FAIL - %s\n' "$1"; exit 1; }

PYTHONPATH="$repo_root/tools/integration" python3 - "$tool" <<'PY' || exit 1
import importlib.util, sys, pathlib
spec = importlib.util.spec_from_file_location("fs_mod", sys.argv[1])
mod = importlib.util.module_from_spec(spec)
# @dataclass resolves annotations through sys.modules[cls.__module__]; a module
# loaded by spec alone is not registered there and the decorator dies on None.
sys.modules["fs_mod"] = mod
spec.loader.exec_module(mod)

def bun(text):
    return mod.bun_signature(text)


def node(text):
    return mod.node_signature(text)


# --- bun: a variable-name argument carries nothing --------------------------
s = bun('expect(received).toBe(expected)\n  Expected: 3\n  Received: 4\n')
assert s.confidence == "MANIFESTATION", s
assert "<bare value>" in s.key, s
print("ok   - expect(x).toBe(expected) is a manifestation, not a mechanism")

# --- bun: a literal argument names what was expected ------------------------
s = bun('expect(received).toContain("util.ts:5:")\n  Received: "boom@/root/app.ts:6"\n')
assert s.confidence == "CLASS", s
assert "util.ts" in s.key, s
print("ok   - a literal matcher argument is a class")

# --- bun: the most explanatory form wins over a generic one -----------------
s = bun("error: TypeError: Cannot destructure property 'minifyTest' from null\n")
assert s.confidence == "CAUSE" and "minifyTest" in s.key, s
print("ok   - a missing internal-for-testing member is a named cause")

s = bun("error: Redis commands over a live socket are not yet implemented in mbun (DEFERRED)\n")
assert s.confidence == "CAUSE", s
print("ok   - mbun's own 'not implemented' is a named cause")

s = bun("error: Failed to build service mysql_plain: failed to solve: mysql:8.0: failed to resolve\n")
assert s.confidence == "CAUSE" and "registry" in s.key, s
print("ok   - a registry-unreachable failure is named, not blamed on the runtime")

# --- node: a bare assertion names no contract -------------------------------
for text in ("AssertionError: The expression evaluated to a falsy value\n",
             "AssertionError: Missing expected exception.\n",
             "error: AssertionError [ERR_ASSERTION]\n"):
    s = node(text)
    assert s.confidence == "MANIFESTATION", (text, s)
print("ok   - bare node assertions are manifestations")

# --- node: an assertion that names a contract IS a class --------------------
s = node("AssertionError: Comparison of the 'code' property failed: expected 'ERR_INVALID_ARG_TYPE', got undefined\n")
assert s.confidence == "CLASS" and "ERR_INVALID_ARG_TYPE" in s.key, s
print("ok   - a property/code comparison is a class")

s = node("AssertionError: Missing expected exception (TypeError).\n")
assert s.confidence == "CLASS" and "TypeError" in s.key, s
print("ok   - a typed missing-exception is a class")

s = node("Mismatched <anonymous> function calls. Expected exactly 1, actual 0.\n")
assert s.confidence == "CLASS" and "mustCall" in s.key, s
print("ok   - mustCall is a class with its counts kept")

# --- neither corpus may silently swallow an unknown form --------------------
s = bun("something entirely unfamiliar\n")
assert s.confidence == "UNSPLIT", s
s = node("something entirely unfamiliar\n")
assert s.confidence == "UNSPLIT", s
print("ok   - an unrecognised form is UNSPLIT, never quietly attributed")
PY

pass "confidence discipline holds across both corpora"
echo "test_failure_signature: ok"
