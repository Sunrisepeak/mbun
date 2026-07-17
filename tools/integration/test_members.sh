#!/usr/bin/env bash
set -uo pipefail

fail() {
    printf '%s\n' "test_members.sh: $1" >&2
    exit 1
}

repo=${1:-$(git rev-parse --show-toplevel)}
result=${2:-$repo/target/integration/member-results.tsv}
manifest="$repo/mcpp.toml"
[[ -r "$manifest" ]] || fail 'root mcpp.toml is missing or unreadable'

if ! result_parent=$(dirname "$result"); then
    fail 'could not determine result directory'
fi
log_dir="$result_parent/member-logs"
if ! mkdir -p "$result_parent" "$log_dir"; then
    fail 'could not create result or log directory'
fi
if ! printf 'member\tcommand\tstatus\tseconds\tlog_path\n' >"$result"; then
    fail 'could not write result header'
fi

if ! member_list=$(sed -n '/^members = \[/,/^\]/p' "$manifest" \
    | sed -n 's/^[[:space:]]*"\([^"]*\)",\{0,1\}$/\1/p'); then
    fail 'could not parse workspace members'
fi
[[ -n "$member_list" ]] || fail 'workspace has no parsed members'
mapfile -t members <<<"$member_list"

failed=0
for member in "${members[@]}"; do
    name=${member//\//_}
    log="$log_dir/$name.log"
    start=$(date +%s)
    if ! : >"$log"; then
        status=fail
        failed=1
    elif (cd "$repo/$member" && mcpp test) >"$log" 2>&1; then
        status=pass
    else
        status=fail
        failed=1
    fi
    end=$(date +%s)
    if ! printf '%s\tmcpp test\t%s\t%s\t%s\n' "$member" "$status" "$((end-start))" "$log" >>"$result"; then
        fail 'could not write result row'
    fi
done
exit "$failed"
