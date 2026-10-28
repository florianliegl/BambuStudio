# Updating the feature branch

Keep `origin` pointed at the user's fork and `upstream` at
`https://github.com/bambulab/BambuStudio.git`. Start from a clean feature branch
with work committed. Do not merge a new master or beta simply because its version
number is larger.

```powershell
git remote -v
git status --short
git switch codex/extended-ams-lite
git branch codex/extended-ams-lite-before-update
$release = Invoke-RestMethod https://api.github.com/repos/bambulab/BambuStudio/releases/latest
$release | Select-Object tag_name, published_at, prerelease, draft, html_url
```

Verify the release page and metadata describe a stable public release. Record
the tag, commit and publication date in the project README. Use a fresh backup
branch name for each update; the example above intentionally fails if it exists.

```powershell
git fetch upstream --tags
git merge --no-ff --no-commit $release.tag_name
```

The initial checkout is shallow. Before a merge across histories, fetch the
missing ancestry, using `git fetch --unshallow upstream` if
`git rev-parse --is-shallow-repository` returns `true`. Resolve conflicts by
reviewing the new release's actual behavior, not by taking all of one side.
If the compatibility audit fails, use `git merge --abort` while the merge is
still pending and keep the previous stable base.

Review the exact integration contract:

- Does `MoveVertex::extruder_id` still store virtual filament identity?
- Does the processor record every relevant virtual selection before purge, and
  retain true layer order for startup and multiple same-layer selections?
- Are move ordering, G-code line ID updates, result copy/reset and serialization
  still compatible with the snapshot fields?
- Have mixed-filament handling, nozzle grouping, mapping payloads, normal pause,
  tool-change macros, or firmware minimum versions changed?
- Does the Preview launcher still reject stale slices and bind to the current
  plate? Are all changed source files registered with the build?
- Are previously unverified hardware capabilities still gated?

Build/run the standalone tests, real processor test, application regressions,
and manual Preview checks listed in `ARCHITECTURE.md`. Repeat the same known
four-color and eight-color slicing fixtures and review schedule/G-code diffs.
Compile with the dependency/toolchain versions required by the new release.

Commit the merge only after these checks pass and the documentation records the
new result. Push only the feature branch, using a normal push; preserve the
backup until regression review is complete. Never replace the fork's default
branch as part of this procedure. Full application build success is not physical
printer validation; revalidate any execution backend against the actual A1
firmware separately if one is added in future.
