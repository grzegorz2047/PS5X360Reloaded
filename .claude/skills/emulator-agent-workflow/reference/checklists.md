# Checklists and templates

## Before you start a fix

- [ ] Got the fix number (and, later, the candidate label) from the version line's owner, as an explicit value.
- [ ] Own worktree, branch `fix/<slug>` cut from the **current** tip of the branch it will merge into (`<line-branch>` or `<integration-branch>`; ask the owner).
- [ ] Read the shared notes for this area. Nobody else is on the same files.
- [ ] Have the failing log, with its label confirmed.

## Fix ready to push

- [ ] `tools/source-1.0.2/fixes/NN-slug.patch` applies on a freshly assembled tree. CRLF files kept CRLF.
- [ ] General by code shape or flags, with no per-game address tables (a traced constant only as a fallback).
- [ ] New UI text is `Tr("English")`, translated in `english/extra.txt`, and `rekey.py` was run. `check-english-source.py` passes.
- [ ] Host check `tools/check-<slug>.*` fails without the fix and passes with it.
- [ ] Wired in `run-fix-checks.sh` (`checks=(...)`), `.github/workflows/source-fix-checks.yml` (`paths:`) and `fixes/README.md` (host-check table).
- [ ] Fix row added in `fixes/README.md` (patch table) and `docs/SOURCE-1.0.2.md`.
- [ ] zig `-fsyntax-only` on every touched `.cc`/`.cpp` **and every file including a touched header**. CI-only files named.
- [ ] zig compile and run of the new or changed checks. `check-patch-syntax.py` passes.
- [ ] `BUILD_VERSION` = `1.0.x-work-fixNN[letter]`.
- [ ] Commit message says the fix number and what it changes, in English.

## After the push

- [ ] Host checks run (`source-fix-checks.yml`) green.
- [ ] `gh workflow run build-source-1.0.2.yml --ref fix/<slug>`, with the run id reported.
- [ ] Green? Then hand it to the coordinator for a console test. Red? Fix it. No label.
- [ ] Report: branch, commit, label, run id, checks covering it, what to test and how, what is CI-only or untested.

## Console test run

- [ ] The label is in `eboot.elf`. The stage size is close to the base size. The image is built.
- [ ] The user says the app is closed or idle, and is not testing or quitting a game. No other upload is running.
- [ ] `<side-dir>` listed: no partial files, superseded images pruned (keep release, official, active and confirmed ones). On a USB stick, room for one more (about 17 fit).
- [ ] Uploaded as `<side-dir>/PPSA83600.ffpkg.v<label>`, SHA-256 verified by download. Local stage and image deleted.
- [ ] Swap only when `/mnt/shadowmnt/` is empty and no other drive holds a `PPSA83600.ffpkg`. The old active image is kept as `.v<its-label>`.
- [ ] Extended storage: `/data/shadowmount/manual.lst` lists `<image-drive>/PPSA83600.ffpkg` (adding it needs the user's yes).
- [ ] After the start: `debug.log`'s `mount_img.lnk created: … -> <path>` names this image.
- [ ] The user gets: the label they should see on the home screen, the game, the scene, the settings, how long to play, and what to look at.
- [ ] Afterwards: fetch the logs (all parts), check the label in the log, convert the screenshots and look at them yourself.
- [ ] Note: label · branch@commit · run id · SHA · result.

## A/B measurement

- [ ] One variable changed. The same scene, save, settings and resolution, and a similar play time.
- [ ] At least three 30 s summaries per run, skipping the loading one.
- [ ] Results as a table (metric, A, B, difference), with the conclusion in one sentence.

## Diagnostic switch on the console (bisect without rebuilding)

- [ ] The diagnostic reads a small text file in `<data-root>/` when it changes and logs what it loaded.
- [ ] Its detection is proven to work: a "fired"/"armed" counter is non-zero on a case made to trigger it. A silent counter is not evidence until then.
- [ ] The default reproduces the bug. Each line flips one key, address or feature.
- [ ] Halve the candidate set per run and keep a table of what each run had and the result.
- [ ] When found: write the real fix by code shape, and delete the file from the console.

## Candidate (owner only)

- [ ] Working branch build green **and** console confirmed.
- [ ] `BUILD_VERSION` = `1.0.x_candidate_N` (the next free N in your range, after checking what was already given out).
- [ ] Merged into `<line-branch>`. Fix rows in number order. Candidate build green.
- [ ] Image uploaded and labelled. Status updated.

## Release

- [ ] `release/source-X.Y.Z[.W]` cut from `<release-branch>` (the previous release), with `<line-branch>` merged in.
- [ ] `package.py` `SHIPPED_CONTENT_VERSIONS` has the previous version. `docs/release-notes/vX.Y.Z.md` written in English (player-facing; workarounds with UI labels).
- [ ] The user said yes to the push. `release-source.yml` green, and the **draft** exists.
- [ ] Draft checked: target commit, assets (`PPSA83600.zip`, `.ffpkg`, `.ffpfsc`, source tarball, toolchain sources, `SHA256SUMS.txt`), notes, and the version in the ELF.
- [ ] Publish only on the user's explicit yes. Then the tag `v<version>` exists. Update the shared notes.

## Issue reply template

Draft it in English, show it to the user, and post it only after their yes.

```
Thanks for the report!

<One plain sentence: what we think is going on, no internals.>

Could you try this?
1. Open PS5X360Reloaded and highlight <game> in the library.
2. Open the game's settings (<button as shown on screen>).
3. Set "<exact option label>" to "<exact choice label>".
4. Start the game and go to <the same place as before>.

You should see <expected result>. <Cost if any, e.g. "It may run a few FPS slower.">

Please reply with <one thing: a screenshot of X / whether it worked>.
```

Do not mention fix numbers, branches, resolves, readbacks, enumerators or code paths, and do not
promise dates.

## Status message for the user (in their language)

```
<label> (<branch>@<short sha>, run <id>): <built/uploaded/on console/confirmed>, <result in one line>
Next: <what you are waiting for / what the user should do>
```
