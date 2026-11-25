# Fork patch stack

This repository is a privacy-focused fork of [Ladybird](https://github.com/LadybirdBrowser/ladybird). It is kept as a
small stack of patches on top of upstream `master`, so that each feature can be rebased frequently and taken upstream
on its own. The plan is tracked in issue #1.

## Branches

- `upstream`: a mirror of upstream `master`, updated by the daily rebase job. Nothing is committed to it.
- `master`: upstream plus the patch stack below, rebased onto `upstream`. It is never merged into.
- `auto-rebase`: written by the daily rebase job. When CI passes on it, `master` is moved to it.
- Feature work happens on branches that are reviewed as pull requests on this fork. Pull requests are never opened
  against `LadybirdBrowser/ladybird`.

## Rules for the stack

- Each feature is a contiguous series of atomic commits in upstream's style (`Documentation/CodePolicy.md`), and each
  commit builds and passes the tests.
- Feature commits keep upstream's default behavior. The fork's own defaults live in one final series,
  `Meta: Apply privacy fork defaults`, so a feature can be cherry-picked without them.
- New code goes in new files where upstream would put it. Hooks into existing upstream files are kept to a few lines,
  because those lines are where rebases conflict.
- Each feature has a design note in `Documentation/Fork/`, written and reviewed before its code.

## CI

- `.github/workflows/fork-ci.yml` lints the fork's commits and code, then builds and runs the full ctest suite (which
  includes `test-web`, the LibTest suites and `cargo test` for the Rust workspace) on pushes to `master` and
  `auto-rebase` and on pull requests. It reuses upstream's `build-and-test.yml`.
- `.github/workflows/fork-rebase.yml` runs daily. It mirrors upstream to `upstream`, rebases `master` onto it, and
  pushes the result to `auto-rebase`. A conflict, or a CI failure on `auto-rebase`, opens or updates an issue labeled
  `rebase-failure`. When CI passes on `auto-rebase`, `master` is moved to it, unless `master` changed after the
  rebase started. It needs a `FORK_REBASE_TOKEN` secret; see the comment at the top of the workflow.

## Series

The commit ranges below are as of the last rebase. Find the current ones with
`git log --oneline upstream..master`.

| Series | Commits | Upstream files touched | Tests | Design note |
|--------|---------|------------------------|-------|-------------|
| Fork infrastructure | `CI: Add a build and test workflow for the fork` to `Documentation: Add a design note for proxy settings` | none | rebase script exercised by hand on a conflicting branch | this file |
| 1. Proxy settings and fail-closed proxying (#3) | not started | see note | see note | [ProxySettings.md](Documentation/Fork/ProxySettings.md) |
| 2. Per-site JavaScript control (#4) | not started | | | |
| 3. Cookie and storage policy (#5) | not started | | | |
| 4. Container tabs (#6) | not started | | | |
| 5. Tor integration (#7) | not started | | | |
| 6. Fingerprinting protections (#8) | not started | | | |
| 7. WireGuard (#9) | not started | | | |
| Fork defaults | not started | | | |
