#!/usr/bin/env bash
set -euo pipefail

repo=$(git rev-parse --show-toplevel)
cd "$repo"
common_git_dir=$(git rev-parse --path-format=absolute --git-common-dir)
repository_root=$(dirname "$common_git_dir")
manifest="$repo/mcpp.toml"
if [[ ! -r $manifest ]]; then
    printf '%s\n' 'branch_inventory.sh: root mcpp.toml is missing or unreadable' >&2
    exit 1
fi
if ! grep -Eq '^\[workspace\]' "$manifest"; then
    printf '%s\n' 'branch_inventory.sh: root mcpp.toml has no [workspace] section' >&2
    exit 1
fi
printf '%s\n' 'branch|tip|main_ancestor|patch_equivalent|modules|worktree|remote'

declare -A worktrees=()
current_path=''
while IFS= read -r line; do
    case "$line" in
        'worktree '*) current_path=${line#worktree } ;;
        'branch refs/heads/'*) worktrees[${line#branch refs/heads/}]=$current_path ;;
    esac
done < <(git worktree list --porcelain)

while IFS= read -r branch; do
    tip=$(git rev-parse --short=8 "$branch")
    if git merge-base --is-ancestor "$branch" main; then
        ancestor=yes
    else
        ancestor=no
    fi

    modules=$(git diff --name-only "$(git merge-base main "$branch")" "$branch" -- modules \
        | awk -F/ 'NF > 1 { print $2 }' | sort -u | paste -sd, -)

    patch_equivalent=no
    if [[ $ancestor == yes ]]; then
        patch_equivalent=yes
    elif [[ -n $(git rev-list --min-parents=2 "main..$branch") ]]; then
        patch_equivalent=no
    elif [[ -z $(git cherry main "$branch" | awk '$1 == "+" { print }') ]]; then
        patch_equivalent=yes
    fi

    if git show-ref --verify --quiet "refs/remotes/origin/$branch"; then
        remote=yes
    else
        remote=no
    fi

    worktree=${worktrees[$branch]:-}
    case "$worktree" in
        "$repository_root") worktree=. ;;
        "$repository_root"/*) worktree=${worktree#"$repository_root"/} ;;
        '') ;;
        *) worktree='<external>' ;;
    esac

    printf '%s|%s|%s|%s|%s|%s|%s\n' \
        "$branch" "$tip" "$ancestor" "$patch_equivalent" "$modules" \
        "$worktree" "$remote"
done < <(git for-each-ref --format='%(refname:short)' 'refs/heads/agent/t*' | sort -V)
