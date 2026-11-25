#!/usr/bin/env bash
# Opens an issue about a failed daily rebase, or comments on the one that is already open, so that a failure that
# persists for several days does not open an issue per day.
#
# Usage: fork-report-rebase-failure.sh <title> <body>
# Requires GH_TOKEN with issues: write, and GITHUB_REPOSITORY.

set -euo pipefail

title="$1"
body="$2"
label="rebase-failure"

gh label create "${label}" --repo "${GITHUB_REPOSITORY}" --color B60205 \
    --description "The daily rebase onto upstream failed" 2> /dev/null || true

existing=$(gh issue list --repo "${GITHUB_REPOSITORY}" --state open --label "${label}" \
    --json number --jq '.[0].number // empty')

if [[ -n "${existing}" ]]; then
    gh issue comment "${existing}" --repo "${GITHUB_REPOSITORY}" --body "**${title}**"$'\n\n'"${body}"
else
    gh issue create --repo "${GITHUB_REPOSITORY}" --label "${label}" --title "${title}" --body "${body}"
fi
