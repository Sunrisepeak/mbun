#!/usr/bin/env bash
# Self-test for smoke_examples.py: a fake "mbun" stands in for the real binary so
# every classification (ok / bad-status / start-error / no-response) is exercised
# without building mbun or touching the real example apps.
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/../../.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

port=$(python3 - <<'PY'
import socket
with socket.socket() as s:
    s.bind(("127.0.0.1", 0))
    print(s.getsockname()[1])
PY
)

cat >"$tmp/fake-mbun" <<'EOF'
#!/usr/bin/env bash
# $1 = app kind, $2 = port
case "$1" in
  serve-ok)   exec python3 -m http.server "$2" --bind 127.0.0.1 --directory "$(dirname "$0")" ;;
  serve-500)  exec python3 -c '
import http.server, sys
class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_error(500)
    def log_message(self, *a): pass
http.server.HTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
' "$2" ;;
  crash)      echo "boom"; exit 3 ;;
  silent)     exec sleep 300 ;;
esac
EOF
chmod +x "$tmp/fake-mbun"

cat >"$tmp/manifest.json" <<EOF
[
  {"name": "ok-app",     "args": ["serve-ok", "$port"]},
  {"name": "bad-app",    "args": ["serve-500", "$port"]},
  {"name": "crash-app",  "args": ["crash", "$port"]},
  {"name": "silent-app", "args": ["silent", "$port"]},
  {"name": "needs-deps", "args": ["serve-ok", "$port"],
   "install": ["install"], "node_modules": "no-such-node_modules"}
]
EOF

set +e
python3 "$repo_root/tools/integration/smoke_examples.py" \
  --bin "$tmp/fake-mbun" --root "$tmp" --manifest "$tmp/manifest.json" \
  --port "$port" --startup-timeout 5 --out "$tmp/out" >"$tmp/stdout.txt" 2>&1
rc=$?
set -e
# Three of the four apps fail on purpose, so a non-zero exit is the expectation.
[ "$rc" = 1 ] || { echo "expected exit 1 from a partially failing round, got $rc"; cat "$tmp/stdout.txt"; exit 1; }

python3 - "$tmp/out" <<'PY'
import csv, json, pathlib, sys
root = pathlib.Path(sys.argv[1])
rows = list(csv.DictReader((root / "results.tsv").open(), delimiter="\t"))
assert [row["name"] for row in rows] == [
    "ok-app", "bad-app", "crash-app", "silent-app", "needs-deps",
], rows
assert [row["classification"] for row in rows] == [
    "ok", "bad-status", "start-error", "no-response", "skipped-no-deps",
], [row["classification"] for row in rows]
assert rows[0]["status"] == "200" and rows[1]["status"] == "500"
summary = json.loads((root / "summary.json").read_text())
assert summary == {
    "apps": 5, "ok": 1, "skipped": 1,
    "categories": {"bad-status": 1, "no-response": 1, "ok": 1, "skipped-no-deps": 1,
                   "start-error": 1},
}, summary
for row in rows:
    # A skipped app never launched, so it has no server log.
    if row["classification"] != "skipped-no-deps":
        assert (root / row["log"]).exists(), row
print("test_smoke_examples: ok")
PY

# No fake server may survive the round: BoundedServer must reap the whole tree.
if pgrep -f "$tmp/fake-mbun" >/dev/null 2>&1; then
  echo "leaked server process after the round"; pgrep -af "$tmp/fake-mbun"; exit 1
fi
