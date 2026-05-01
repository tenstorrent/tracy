# Tracy fork: versioning and GUI releases

This document describes how we version **Tenstorrent’s Tracy fork** (GUI, capture tools, and Homebrew formula), how **`.tracy` saved traces** embed a format version, and how **viewer builds** accept or reject those files. It applies to releases tagged on this repository, not to tt-metal’s CMake build of the Tracy client library.

## Goals

- Make it obvious **which upstream Tracy release** a binary belongs to (protocol, capture format, general UI behavior).
- Make it obvious **which Tenstorrent-only revision** shipped on top of that exact upstream tree.
- Keep **stable, reproducible installs** (pinned tarballs and checksums in `tracy.rb`), with `--HEAD` reserved for developers.

## Version scheme

Public GUI releases use tags of the form:

```text
v<upstream-version>-tt.<n>
```

| Part | Meaning |
|------|--------|
| `<upstream-version>` | The **upstream Tracy release** we rebased or merged from [wolfpld/tracy](https://github.com/wolfpld/tracy). Prefer the **full version** upstream publishes (`major.minor.patch`, e.g. `0.10.7`) so **minor and patch** are visible in the tag—not only `0.10`, which hides whether you picked up upstream `0.10.4` vs `0.10.7`. If upstream tags only `v0.10` with no patch segment, use `v0.10-tt.<n>` and cite the exact upstream commit in release notes. |
| `tt.<n>` | Tenstorrent fork revision **on top of that exact `<upstream-version>`**. |

**Rules for `tt.<n>`:**

- **Reset to `0`** whenever **`upstream-version`** changes (patch, minor, or major): new upstream tarball → new `v…-tt.0`.
- **Increment** (`tt.1`, `tt.2`, …) only for **fork-only** releases while staying on the **same** `upstream-version`.

Examples:

- `v0.10.7-tt.0` — first Tenstorrent release on upstream **v0.10.7** exactly.
- `v0.10.7-tt.1` — Tenstorrent-only tweak still on upstream **v0.10.7** (no change to wolfpld’s released tree identity).
- `v0.11.0-tt.0` — first Tenstorrent release after integrating upstream **v0.11.0**.

Legacy tags like **`v0.10-tt.0`** (two-segment upstream) remain valid historically; **new** releases should prefer **`v0.10.<patch>-tt.<n>`** whenever wolfpld publishes a three-segment release.

Release notes should always state:

1. **Upstream base** — upstream tag **and** commit SHA if helpful.
2. **Fork changes** — Tenstorrent-specific additions or fixes.
3. **Recommended pairing** — which tt-metal release(s) this GUI is tested with or recommended for (when relevant).

---

## When upstream changes: patch vs minor vs major

Align `<upstream-version>` with wolfpld’s **released** version. When you integrate a newer upstream release, **update all three segments** you take from them, then **reset `tt` to `0`** unless you are only adding TT commits on the same upstream tag (see rules above).

### Patch-level upstream bump (e.g. 0.10.4 → 0.10.7)

Upstream ships another release on the **same major.minor** line; **patch** changes.

**What we do:**

- Merge or rebase onto the new upstream tag (e.g. `v0.10.7`).
- Tag **`v0.10.7-tt.0`** (not `v0.10-tt.N`—the tag should carry **0.10.7**).
- Further TT-only drops on that same upstream tree: **`v0.10.7-tt.1`**, etc.

**Concrete example:**

- Current tag: **`v0.10.4-tt.2`** (two TT iterations on upstream v0.10.4).
- Upstream releases **`v0.10.5`**; we integrate → **`v0.10.5-tt.0`**.
- Later upstream **`v0.10.7`**; we integrate → **`v0.10.7-tt.0`** (again reset `tt`).

Release notes: “Based on wolfpld/tracy v0.10.7.”

### Minor / major upstream bump (e.g. 0.10.x → 0.11.x)

Upstream moves to a **new minor or major** line. Clients and GUI may need to stay in sync; traces and protocol may change.

**What we do:**

- Merge/rebase onto the new upstream tag (e.g. `v0.11.0`).
- Publish **`v0.11.0-tt.0`** (full upstream version + reset `tt`).
- Subsequent fork-only releases: **`v0.11.0-tt.1`**, …; if upstream later ships **`v0.11.3`** and you integrate it → **`v0.11.3-tt.0`**.

**Concrete example:**

- We were on **`v0.10.7-tt.3`**.
- Upstream releases **`v0.11.0`**; we integrate → **`v0.11.0-tt.0`**.
- Release notes: “Upstream base: wolfpld/tracy v0.11.0; Tenstorrent: [list TT-specific commits]. Users on 0.10.x-based Metal builds should keep using **`v0.10.*-tt.*`** GUI until they upgrade Metal.”

---

## When we make tweaks and updates on our end

Fork-only work **without** a new wolfpld **`upstream-version`**—Tenstorrent patches, packaging, GUI polish—increments **`tt`** only (e.g. **`v0.10.7-tt.1`** → **`v0.10.7-tt.2`**).

- **Profiler / library code:** release notes list “Fork changes”; **`upstream-version`** unchanged.
- **`tracy.rb` / install:** same pattern; bump **`tt`** and refresh **`url` / `sha256`**. For formula-only mistakes, Homebrew **`revision`** is possible but use sparingly so Git tags stay authoritative.
- **Docs / comments only:** usually **no** tag unless users need a new checksum or install instruction update.

---

## Rebasing our fork onto upstream

**Rebase** means: take **our commits** and replay them on top of a **new upstream tip** (instead of merging upstream into our branch). Git history and commit SHAs change even when the **resulting source tree** is intentionally the same as before.

That matters for releases because:

- **Git tags** point at commits. After a rebase, the old tag still points at **abandoned** history unless you force-move tags (avoid rewriting public tags users rely on).
- **Tarballs and `sha256`** in `tracy.rb` are content-addressed. Any rebase that produces **new commit objects** needs a **new tag** (or users must use `--HEAD`), and Homebrew **checksums must be updated** for pinned installs.

**Rule of thumb:** Treat a successful “rebase onto upstream” integration like any other integration: **new tag** whose **`upstream-version`** matches the wolfpld tag you rebased onto, and **`tt`** per the rules above. Do not assume an old tag still describes the same bytes after a rebase.

### Example D — Same upstream line, newer patch

| Step | Detail |
|------|--------|
| **Before** | **`v0.10.4-tt.4`** (messy history on wolfpld v0.10.4). |
| **Action** | Rebase TT patches onto **`v0.10.7`**. |
| **After** | Tag **`v0.10.7-tt.0`**; update **`tracy.rb`**. Notes: rebased onto wolfpld v0.10.7; TT delta equivalent to **`v0.10.4-tt.4`** (or list conflict fixes). |

Pinned **`v0.10.4-tt.4`** tarballs stay valid; default installs move to **`v0.10.7-tt.0`**.

### Example E — New upstream line

| Step | Detail |
|------|--------|
| **Before** | **`v0.10.8-tt.2`**. |
| **Action** | Rebase onto **`v0.11.0`**. |
| **After** | **`v0.11.0-tt.0`**. Later rebase onto **`v0.11.3`** → **`v0.11.3-tt.0`**; TT-only work on that tree → **`v0.11.3-tt.1`**, etc. |

### Example F — Broken publish, same upstream version

| Step | Detail |
|------|--------|
| **Before** | **`v0.10.7-tt.3`** shipped with a bad tarball. |
| **Action** | Fix branch; stay on upstream **v0.10.7**. |
| **After** | **`v0.10.7-tt.4`** — do **not** force-retag **`tt.3`**; deprecate it in notes. |

### If upstream rewrites public history (unusual)

Normally wolfpld/tracy does **not** force-push over released tags. If it ever did:

- Your clone’s **old upstream SHAs may disappear**; fetches break until you **rebase onto their new refs**.
- Treat it like a fresh integration: document the **new** upstream tag/commit in release notes, then apply the same **`upstream-version`** / **`tt`** rules as for patch vs minor vs major bumps (sections above).

### Rebase vs merge (for versioning only)

Either integration style still ends with **a new commit at the branch tip**. From a **release/tag/checksum** perspective, both require a **new tag** when you publish binaries. Pick rebase vs merge for **history hygiene** inside the fork; do not use it to skip versioning.

---

## Saved traces (`.tracy` files)

The **`.tracy` suffix** is a naming and desktop-integration convention (open dialog, `capture -o`, MIME globs). **Opening a file does not depend on the extension:** the reader validates a **binary container** (magic bytes, compression streams — see `server/TracyFileRead.hpp` and `server/TracyFileHeader.hpp`), then decodes compressed blocks.

After decoding, the trace payload starts with an **8-byte header**: ASCII **`tracy`** plus **three bytes** that encode the embedded **trace format version** `(major, minor, patch)` — the same numbering family as `public/common/TracyVersion.hpp`. That embedded triple is what loaders compare; **Git tags like `v0.10.7-tt.3` are not stored inside the file.**

---

## Viewer ↔ trace compatibility (upstream rules)

Each profiler binary compiles in a **`CurrentVersion`** from `TracyVersion.hpp` and enforces a floor **`MinSupportedVersion`** (currently **0.9.0** — see `server/TracyWorker.cpp`). For a given viewer build:

| Embedded trace version in the file | Typical outcome |
|-----------------------------------|-----------------|
| **Greater than** this viewer’s `CurrentVersion` | Rejected — viewer is too old (`UnsupportedVersion`). |
| **From `MinSupportedVersion` up to `CurrentVersion` inclusive** | Loaded; older layouts use version branches inside `TracyWorker.cpp`. |
| **Below `MinSupportedVersion`** (with the modern inner header) | Rejected as legacy (`LegacyVersion`). |

**Practical rule:** use a viewer whose **`TracyVersion.hpp`** is **≥** the trace’s embedded `(major, minor, patch)`, and **≥** whatever minimum upstream still supports. To change compression or rewrite metadata offline, upstream provides **`tracy-update`** (`update/`).

These rules come from **wolfpld/tracy**; our fork inherits them unless we deliberately change serialization.

---

## Fork tags (`-tt.N`) and non-breaking TT tweaks

**`-tt.N` does not participate in trace loading.** Two GUI builds with the **same** `Version::Major` / `Minor` / `Patch` in `TracyVersion.hpp` share the **same** `CurrentVersion` and therefore accept the **same range** of `.tracy` files — whether the Git tag is **`v0.10.7-tt.0`** or **`v0.10.7-tt.5`**.

- An older **TT** GUI (**lower `tt`**, same Tracy `M.m.p`) still **opens** traces produced with that Tracy line; **newer TT-only UI features** simply **won’t appear** there.
- That stays true only if TT work stays **non-breaking** in Tracy’s sense: prefer **viewer-only** changes; avoid new payload or protocol/file-layout changes that older workers cannot skip or interpret.

If you **merge upstream** and bump **`TracyVersion.hpp`**, file compatibility moves with that number — same as vanilla Tracy.

---

## GitHub releases and Homebrew

1. **Tag** each GUI release on this repo (`v<upstream-version>-tt.<n>`).
2. **`tracy.rb`** — set **`url`** to that tag’s archive and **`sha256`** to the archive checksum (stable default for users).
3. **`head`** in the formula — optional; supports `brew install --HEAD` for developers tracking branches.
4. **Older tags** stay available so users on older tt-metal can install a matching GUI.

---

## Summary table

| Situation | Typical tag change |
|-----------|-------------------|
| Tenstorrent-only release, **same** upstream tag | `v0.10.7-tt.N` → `v0.10.7-tt.(N+1)` |
| Integrated new upstream **patch** (e.g. 0.10.7 → 0.10.8) | **`v0.10.8-tt.0`** (bump `upstream-version`, reset `tt`) |
| Integrated new upstream **minor/major** | **`v0.11.0-tt.0`** (full upstream version, reset `tt`) |
| **Rebase** onto newer upstream **same line** | New **`upstream-version`** + **`tt.0`** (e.g. **`v0.10.7-tt.0`** after **`v0.10.4-tt.*`**) |
| **Rebase** onto **new** upstream line | **`v0.11.0-tt.0`**, then **`v0.11.3-tt.0`**, etc. |
| Bad tag / wrong tarball, **same** upstream version | **`v0.10.7-tt.(N+1)`**; avoid force-moving published tags |
| Formula/deps fix, same sources | Prefer **`tt.(N+1)`** + update `sha256`, or Homebrew `revision` only if appropriate |

Maintainers should keep tt-metal release notes or profiler docs in sync with **recommended Tracy GUI tag** when compatibility expectations change.
