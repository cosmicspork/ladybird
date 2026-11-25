#!/usr/bin/env bash
# Rebases the fork's patch stack onto upstream master.
#
# Usage: fork-rebase-onto-upstream.sh <stack-branch> <upstream-ref>
#
# On success, HEAD is the rebased stack. On a conflict, the rebase is aborted, the conflicting commit and files are
# written to the file named by $CONFLICT_REPORT (if set), and the script exits with status 1.

set -euo pipefail

stack_branch="$1"
upstream_ref="$2"

# The stack is every commit on the stack branch that upstream does not have. The merge base, not upstream's tip, is
# the point it was last rebased onto.
base=$(git merge-base "${stack_branch}" "${upstream_ref}")
echo "Rebasing $(git rev-list --count "${base}..${stack_branch}") commits from ${stack_branch} onto ${upstream_ref}"

git checkout --quiet --detach "${stack_branch}"
if git rebase --onto "${upstream_ref}" "${base}"; then
    exit 0
fi

stopped_at=$(git rev-parse --short REBASE_HEAD 2> /dev/null || echo unknown)
conflicts=$(git diff --name-only --diff-filter=U)
git rebase --abort

if [[ -n "${CONFLICT_REPORT:-}" ]]; then
    {
        echo "Rebasing \`${stack_branch}\` onto upstream \`$(git rev-parse --short "${upstream_ref}")\` stopped at:"
        echo
        echo "    $(git log -1 --format='%h %s' "${stopped_at}" 2> /dev/null || echo "${stopped_at}")"
        echo
        echo "Conflicting files:"
        echo
        while read -r file; do
            [[ -n "${file}" ]] && echo "- \`${file}\`"
        done <<< "${conflicts}"
    } > "${CONFLICT_REPORT}"
fi

exit 1
