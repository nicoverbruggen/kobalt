# Working on Kobalt public source

Read [README.md](README.md) before changing code. Follow [Building Kobalt](docs/development.md) for build commands, tests and source boundaries. [Shared reader controls](docs/development.md#shared-reader-controls) is the canonical component guidance. Keep reader behavior documentation current in [Reader behavior](docs/reader.md).

## README edits

Only the user edits this repository's root `README.md`. Agents may edit README files in subdirectories as ordinary project documentation. As soon as you identify a necessary edit to the root `README.md`, tell the user why it is needed and propose the exact wording. Notify the user immediately, even when other work remains. Continue other pending work that can proceed independently. Wait for the user's root README edits only when no other pending work can proceed.

## Worktrees and branches

`develop` is this repository's integration branch. Start new implementation work in a separate worktree and task branch based on `develop`. Continue existing work in its existing worktree and branch unless the user requests otherwise. Target completed implementation changes at `develop`. Use `main` only for explicitly requested release work.

When the user explicitly asks you to implement a change and merge it into `develop`, you may create the task branch and worktree, stage and commit the task's changes, and integrate them into local `develop` after the required checks pass. This authorization covers only repositories named in the request. Follow the signing requirements and preserve unrelated work. Do not ask again for operations already covered by that authorization.

A request to merge a task means to integrate its commits. Fast-forward `develop` when possible. Otherwise, cherry-pick the task's commits in order onto current `develop`. Create a merge commit only when the user explicitly requests one. Preserve independent changes when resolving conflicts. If the resulting source differs from the tested source, run the affected checks again before finishing.

A request to fix or implement something without an explicit merge instruction authorizes local edits and verification only. A request naming only a specific Git operation authorizes only that operation. Rebasing and destructive cleanup require separate explicit authorization. Completed task branches and worktrees follow the cleanup rule below.

Never push branches, tags or any other refs unless the user explicitly requests that push. Permission to commit or merge does not authorize a push. Force-pushing requires an explicit force-push instruction. Global agent instructions still govern permissions, Git authorization and signing.

## Signing handoff

When committing is authorized, finish implementation and required verification before the signing handoff. Present the proposed atomic commits and their validation results, then warn the user before triggering signing prompts. Use imperative one-line commit messages and no agent coauthor attribution.

Always sign project commits, including cherry-picked commits and explicitly requested merge commits. Run commits that use 1Password signing outside the agent sandbox. If signing fails, retry once automatically. Before retrying, confirm that the intended commit was not already created. If the second signing attempt fails, stop and ask permission for one further attempt so the user can intervene. Do not make further attempts without permission, and never fall back to unsigned commits.

After signing succeeds, continue any local integration already authorized without asking again.

## Key constraints

- Design for slow ARM cores, limited memory and e-paper displays. Follow [Performance and resource use](docs/development.md#performance-and-resource-use) and support performance claims with measurements.
- Keep shared reader behavior compatible with Kobo Qt5, Kobo Qt6, Android and the macOS development app. Firmware 4 uses Qt5; firmware 5 and 6 use Qt6. For Kobo development builds, interactive previews and integration checks, use firmware 4 and Qt5 unless the user requests another target. Build and check the same target and installation mode. Host tests use their documented host Qt SDK.
- Before adding a control or interaction, look for an existing shared reader component. Follow its ownership, layout and interaction rules.
- Preserve existing functionality when adding features or refactoring. Check affected callers and platform implementations before changing shared contracts.
- Public builds and tests must work from this checkout alone. Do not add dependencies on private checkouts, private fixtures or local services.

## UI proposals

Requested UI changes and bug fixes within the requested scope do not require a separate proposal review. Choose routine layout and implementation details using the project's existing components and conventions.

Before changing established UI behavior beyond the requested scope, explain the proposed behavior and wait for approval. If the task leaves a material choice between different user-visible behaviors unresolved, ask before implementing that choice. UI proposals should normally include a design or mockup.

## Build speed and caches

Use incremental development builds while implementing a change. Reserve fresh packaging builds for final package validation or investigation of build reproducibility.

Cache each compiled dependency separately and share verified dependency artifacts across worktrees. Cache keys must cover source checksums, patches, compiler and toolchain identity, relevant build options and upstream dependency identities. Changing one dependency must invalidate only that dependency and those that depend on it. Follow the build documentation for the configurable cache location.

Keep application objects and writable build directories inside each worktree's ignored build output. Do not share configured CMake build directories between worktrees. Verify cached artifacts before reuse. Rebuild invalid entries rather than bypassing verification.

Report build and test durations separately from investigation time. Do not impose a maximum duration without representative measurements.

## Verification

During implementation, use focused tests and incremental builds for quick feedback. Add regression tests for bugs you fix. Before merging, run all checks required by [Building Kobalt](docs/development.md) for the affected code and platforms against the completed change, including the formatting and UI architecture checks when they apply. Run the complete required verification once against the completed change before integrating it into `develop`. Do not require intermediate atomic commits to build or pass tests. Do not run tests or create temporary checkouts solely to verify each intermediate commit. Focused checks during development remain appropriate. Repeat affected checks only when their relevant source, dependency, toolchain or configuration inputs change. Reuse recorded results when those inputs match. Creating or cherry-picking commits does not invalidate verification merely because commit hashes change.

A filtered test run does not count as a full-suite pass. Report skipped or unavailable checks explicitly. Do not weaken assertions merely to obtain a pass. If a required check fails or cannot run, resolve it before merging. If it cannot be resolved within the authorized task, report the blocker and stop before merging unless the user explicitly approves an exception.

For UI changes, complete the required screenshot captures and inspect affected screenshots in light and dark mode at the documented densities. Preserve the affected capture set before a UI refactor, as described in [Native screenshot tour](docs/development.md#native-screenshot-tour). Successful capture alone does not establish visual correctness.

Report source inspection, host tests, platform integration checks and physical-device checks separately. Do not claim one establishes the results of another. Host tests do not establish Nickel ABI compatibility or physical-device display behavior.

## Evidence and cleanup

Record the source commits, hashes of relevant uncommitted inputs, build configuration and commands alongside verification results. Keep required logs, screenshots and reports in ignored build output. Do not create tracked summary documents or publish artifacts without explicit approval.

Arrange cleanup before starting temporary test devices. Copy required evidence out, then stop and delete each device immediately after its last use, including after failed tests. Verify that the device's temporary storage was removed. Remove only devices and scratch files created by the current task. Preserve explicitly persistent devices and shared caches.

After successfully integrating a task into `develop`, remove the completed task's feature worktree and delete its feature branch. This cleanup is included in the merge authorization. Before removal, verify that all intended changes are present in `develop`, the worktree has no uncommitted changes, and no other active task is using it. Preserve required verification evidence outside the worktree first.

Do not discard uncommitted work or delete unrelated branches or worktrees. If safe cleanup cannot be established, leave them intact and report why. This does not authorize deleting remote branches or performing any push.

## Changelog version selection

Before adding changelog entries, check the newest versioned heading and whether its exact Git tag exists. Ignore a generic `Unreleased` heading when identifying that version.

If the tag does not exist, that version is still open. Put new features and fixes under that heading. If the tag exists, that version is closed to new changes. Open the next version's section and keep `VERSION` consistent with it.

Choose the next version automatically only when an established project rule makes it unambiguous. Otherwise, ask before selecting it. During a beta cycle, ask whether to continue with the next beta or move to the final release.

Verify tag availability against the authoritative release remote. If verification fails, do not assume the tag is absent. Selecting an upcoming version does not authorize creating or pushing a Git tag.

## Note on Nickel 6.x

Firmware 6 Nickel can abort with heap corruption while its process exits, after `ServiceLocator::~ServiceLocator`, with `malloc_consolidate(): unaligned fastbin chunk detected` or `corrupted double-linked list`. This happens with any NickelHook plugin loaded, including one with no hooks and no Kobalt code. The cause is not known. It happens only at process exit, after Kobalt has saved its state. Since Nickel 6.x isn't considered stable yet, treat it as a known issue, not as a regression or a release blocker, unless a change makes it happen before exit or more often.

## Platforms

- Intended release platforms are Kobo with Qt5 or Qt6, and Android. This does not mean every target is ready for release; see the README for current availability.
- macOS is a development platform for the shared reader, not a general release target.
- Tolino has partial support through Qt6 but is not a supported release target.
- iOS/iPadOS, PocketBook and jailbroken Kindle are under consideration and are not supported.

## Release validation

Before a release is tagged, do the following:

- Make sure the changelog contains all new user-facing changes (not technical decisions, minor changes).
- When preparing a release, update the root `VERSION` file to the release tag, such as `v1.0-beta.2`, in the same change as its `CHANGELOG.md` entry. Check that both match before tagging. Untagged builds use `VERSION`; tagged CI builds use the triggering tag.
- Assemble the screenshot pass for the previous tag and compare it to the new release. Did all the differences match expected changes in the codebase, or did something unexpected change?

## Acknowledgements

Update [ACKNOWLEDGEMENTS.md](ACKNOWLEDGEMENTS.md) when adding or replacing a dependency or asset. Include upstream links and the original authors. Credit Nico Verbruggen for his projects and adaptations, while keeping the original upstream authors credited. Keep maintenance instructions in this file, not in the acknowledgements document.
