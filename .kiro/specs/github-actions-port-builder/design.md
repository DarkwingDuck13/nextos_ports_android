# Design Document: github-actions-port-builder

## Overview

This feature adds a fully self-contained GitHub Actions CI/CD pipeline to the `nextos_ports_android` repository. The pipeline cross-compiles aarch64 so-loader port binaries and assembles PortMaster-compatible zip artifacts with zero external secrets or pre-provisioned infrastructure. Any user who forks the repository can trigger a build via workflow dispatch and receive a ready-to-install zip.

The pipeline covers four concerns:

1. **Environment setup** — install `gcc-10-aarch64-linux-gnu` from apt, bootstrap a Debian Bullseye aarch64 sysroot with the headers and stub libraries every port `build.sh` needs.
2. **Port selection and matrix build** — parse `README.md` to discover playable ports, resolve the caller's port selection against that list, then fan out to parallel per-port jobs.
3. **Per-port pipeline** — compile → validate binary (ELF arch, glibc version, bundled-lib check, cleanliness check) → assemble zip → validate zip manifest → upload artifact.
4. **Release publishing** — on `v*` tag pushes, create or update a GitHub Release and attach each zip as a release asset.

The workflow is implemented entirely in `.github/workflows/build-ports.yml`, backed by three helper scripts (`ci/discover-ports.sh`, `ci/build-port.sh`, `ci/check-binary.sh`, `ci/assemble-zip.sh`) that encapsulate the logic that would otherwise make the YAML unreadable.

---

## Architecture

```mermaid
flowchart TD
    subgraph Triggers
        T1[workflow_dispatch\nport: all | name | list]
        T2[push tag v*]
    end

    subgraph setup-job["Job: setup (ubuntu-22.04)"]
        S1[apt-get install\ngcc-10-aarch64-linux-gnu\nbinutils-aarch64-linux-gnu]
        S2[Bootstrap sysroot\nDebian Bullseye aarch64\nSDL2, EGL, GLESv2,\npthread, m, dl]
        S3[discover-ports.sh\nparse README.md]
        S4[Resolve port selector\nvs playable list]
        S5[Upload sysroot tarball\nas artifact: ci-sysroot]
        S6[Output matrix JSON]
    end

    subgraph build-matrix["Job: build (matrix, ubuntu-22.04)"]
        B1[Download ci-sysroot artifact]
        B2[Export CC, SR]
        B3[build-port.sh\ninvoke ports/portname/build.sh]
        B4[check-binary.sh\nELF arch check\nglibc version check\nbundled-lib check\ncleanliness check]
        B5[assemble-zip.sh\ncopy files, patch port.json\ncreate zip]
        B6[Validate manifest\nvs port.json items]
        B7[Upload artifact\nportname-portmaster-zip\n14-day retention]
    end

    subgraph release-job["Job: release (on v* tag only)"]
        R1[Download all portmaster-zip artifacts]
        R2[gh release create/update\ntag ref]
        R3[Attach each zip\nas release asset --clobber]
        R4[Write GITHUB_STEP_SUMMARY\nsummary table]
    end

    subgraph summary-job["Job: summary (always)"]
        SUM[Write GITHUB_STEP_SUMMARY\nper-port outcome table\nartifact URLs]
    end

    T1 --> setup-job
    T2 --> setup-job
    S1 --> S2 --> S3 --> S4 --> S5 --> S6
    S6 -->|matrix JSON| build-matrix
    setup-job -->|needs| build-matrix
    B1 --> B2 --> B3 --> B4 --> B5 --> B6 --> B7
    build-matrix -->|needs| release-job
    build-matrix -->|needs| summary-job
```

### Design Decisions

**Single sysroot artifact shared across matrix jobs.** Building the sysroot inside every matrix job would waste 3–5 minutes per port. Instead, the setup job bootstraps the sysroot once, tarballs it, uploads it as a GitHub Actions artifact named `ci-sysroot`, and each build job downloads and extracts it. This keeps build job startup under 30 seconds.

**Helper scripts instead of inline YAML shell.** Each non-trivial step (discover, build, check, assemble) is implemented as a shell script under `ci/`. This makes the YAML readable, allows the scripts to be run locally for debugging, and makes each step independently testable.

**`debootstrap` strategy for sysroot.** Ubuntu 22.04 ships `debootstrap` in apt. A Bullseye aarch64 sysroot can be bootstrapped with `sudo debootstrap --arch=arm64 --variant=minbase --include=libsdl2-dev,libegl-dev,libgles2-mesa-dev bullseye ./sysroot http://deb.debian.org/debian`. This installs the exact same headers and stub `.so` files that the NextOS Elite Edition toolchain sysroot contains.

**glibc 2.17 target.** The `build.sh` scripts pass `--sysroot=$SR` but do not constrain the glibc version by default; it is the CI check step, not the compile step, that enforces the 2.17 ceiling. To help hit 2.17 a compile flag `-D_GNU_SOURCE` is not needed, but passing `-Wl,--hash-style=gnu` and avoiding `__stack_chk_fail@GLIBC_2.17+` symbols will usually land in range naturally when compiled with `gcc-10` against a Bullseye sysroot.

**Port selector resolution in setup job.** Resolving the `port` dispatch input against the playable list in a dedicated setup job, before the matrix starts, lets us fail fast and clearly (with a human-readable error listing non-playable ports) before any build resources are allocated.

---

## Components and Interfaces

### `.github/workflows/build-ports.yml`

The top-level orchestration file. It declares:

- `on.workflow_dispatch.inputs.port` — string, default `all`
- `on.push.tags` — pattern `v*`
- Three jobs: `setup`, `build` (matrix), `release`

All third-party action references are pinned to immutable SHA or version tags.

### `ci/discover-ports.sh`

**Purpose:** Parse `README.md` and emit a JSON array of playable port names.

**Interface:**
```
ci/discover-ports.sh <readme_path>
```
Outputs to stdout a JSON array: `["bully","dysmantle","sotn",...]`

**Logic:**
1. Find the `### ✅ Concluídos — jogáveis` section in the Markdown.
2. For each table row, extract the link target in the "Pasta" column using `grep -oE 'ports/[a-z0-9_-]+'` and strip the `ports/` prefix.
3. Deduplicate and sort.
4. Emit the JSON array using `jq -R -s 'split("\n") | map(select(length>0))'`.

**Warnings emitted to stderr** (not stdout, to avoid corrupting the JSON):
- Port in playable list but `ports/<name>/` does not exist
- Port in playable list but `ports/<name>/build.sh` is missing

The script exits 0 even when warnings are emitted. Only malformed README output (empty array) triggers exit 1.

### `ci/build-port.sh`

**Purpose:** Invoke a port's `build.sh` in the correct working directory with the correct environment.

**Interface:**
```
CC=<path> SR=<path> ci/build-port.sh <portname>
```

**Logic:**
1. Verify `ports/<portname>/build.sh` exists; if not, print human-readable error and exit 1.
2. `cd ports/<portname>/`
3. Log `CC`, `SR`, and `$(aarch64-linux-gnu-gcc-10 --version)` to stdout.
4. Log `git rev-parse HEAD` to stdout.
5. Execute `bash build.sh` with `CC` and `SR` set.
6. Propagate the exit code.

### `ci/check-binary.sh`

**Purpose:** Validate the ELF produced by `build.sh` against all binary quality gates.

**Interface:**
```
ci/check-binary.sh <portname> <binary_path>
```

**Logic (in order, exit 1 on any failure):**

1. **ELF arch check** — `file <binary>` must contain `ELF 64-bit LSB` and `aarch64` and `dynamically linked`. Failure: print the actual `file` output and exit 1.

2. **glibc version check** — Parse all `GLIBC_X.Y` version strings from `objdump -p <binary>` (or `readelf -V`). Extract the highest. If > 2.17 (comparing as (major, minor) tuples), print the offending symbol versions and exit 1.

3. **Bundled-lib check** — Parse `DT_NEEDED` entries from `readelf --dynamic <binary>`. If any entry is `libSDL2.so`, `libopenal.so*`, `libmali.so*`, or `libmpg123.so*`, print the offending entry and exit 1. (Note: these appear in `DT_NEEDED` only when statically linked or explicitly linked with `-l`; runtime-only `dlopen` usage does not create `DT_NEEDED` entries, so this check is exact.)

4. **Cleanliness check — symbols** — `nm --defined-only <binary>` must not contain any of `watchdog_thread`, `heartbeat_thread`, `input_selftest`, `dump_framebuffer`, `log_frame_pixels` as non-weak (`T` or `D`) symbols. Failure: print offending symbol names and exit 1.

5. **Cleanliness check — strings** — `strings <binary>` scanned for `GLSTATE` or `GLDRAW`. For each match, check whether the same line (or an adjacent line within ±5 lines in the strings output) contains a guard string matching `_VERBOSE`. If no guard is found, emit a **warning** to stdout but do NOT exit 1.

### `ci/assemble-zip.sh`

**Purpose:** Assemble a PortMaster-compatible zip from a compiled port directory.

**Interface:**
```
ci/assemble-zip.sh <portname> <binary_path> <output_dir>
```

**Logic:**

1. **Read `port.json`** from `ports/<portname>/port.json`. Exit 1 if missing.

2. **Determine zip name** from `port.json` `.name` field.

3. **Create staging directory** `<output_dir>/staging/<portname>/`.

4. **Copy mandatory files:**
   - `ports/<portname>/*.sh` → `<staging>/` (top level, filename preserved)
   - `ports/<portname>/port.json` → `<staging>/<portname>/port.json`
   - `ports/<portname>/README.md` → `<staging>/<portname>/README.md`
   - `ports/<portname>/screenshot.png` → `<staging>/<portname>/screenshot.png`
   - `<binary_path>` → `<staging>/<portname>/<portname>.aarch64`

5. **Copy optional files (only if present):**
   - `cover.png`, `gameinfo.xml`, `*.gptk`, `alsoft.conf` → `<staging>/<portname>/`
   - `licenses/` directory → `<staging>/<portname>/licenses/`

6. **Patch `port.json`** inside staging:
   - Set `.attr.arch = ["aarch64"]`
   - In the `.items` array, replace any entry that does not end in `.sh` (i.e., the binary entry) with `"<portname>/<portname>.aarch64"`. All other entries are updated to include the `<portname>/` prefix if not already present.
   - Write the updated JSON back.

7. **Compute manifest** — list all files in `<staging>/` relative to `<staging>/`, sorted.

8. **Validate manifest vs `port.json` items** — the updated `.items` array must exactly match the manifest (after normalising paths). If not, print a diff and exit 1.

9. **Assemble zip**: `cd <staging> && zip -r <output_dir>/<zipname> .`

10. **Clean up** staging directory.

---

## Data Models

### Port metadata (`port.json`)

The existing format is version 2 as seen in `ports/bully/port.json`. The CI pipeline reads and writes only these fields:

```json
{
  "version": 2,
  "name": "<portname>.zip",
  "items": [
    "<Title Case Launch>.sh",
    "<portname>/<portname>.aarch64",
    "<portname>/README.md",
    "<portname>/screenshot.png",
    "<portname>/port.json"
  ],
  "items_opt": [],
  "attr": {
    "arch": ["aarch64"]
  }
}
```

The CI patches `items` (to replace the bare binary name with the arch-suffixed path) and `attr.arch`. All other fields are passed through unchanged.

### Playable port list (runtime)

The discovery step produces a JSON array that is used as the matrix value:

```json
["bully", "dysmantle", "sotn", "ninjago", "katanazero"]
```

This is set as a job output (`matrix.port`) and consumed by the matrix strategy:

```yaml
strategy:
  matrix:
    port: ${{ fromJson(needs.setup.outputs.matrix) }}
```

### Sysroot layout

The bootstrapped sysroot at `$SR` must expose at minimum:

```
$SR/usr/include/SDL2/SDL.h
$SR/usr/include/SDL2/SDL_events.h
$SR/usr/lib/aarch64-linux-gnu/libSDL2.so   (stub)
$SR/usr/include/EGL/egl.h
$SR/usr/lib/aarch64-linux-gnu/libEGL.so    (stub)
$SR/usr/include/GLES2/gl2.h
$SR/usr/lib/aarch64-linux-gnu/libGLESv2.so (stub)
$SR/usr/include/pthread.h
$SR/usr/lib/aarch64-linux-gnu/libpthread.so (stub)
$SR/usr/lib/aarch64-linux-gnu/libm.so      (stub)
$SR/usr/lib/aarch64-linux-gnu/libdl.so     (stub)
```

The `debootstrap` approach with `--include=libsdl2-dev,libegl-dev,libgles2-mesa-dev` provides exactly this.

### Workflow job outputs

```
setup job outputs:
  matrix: '["bully","dysmantle",...]'   # JSON array, consumed by build matrix

build job outputs (per matrix item):
  artifact_url: https://github.com/.../artifacts/...
  outcome: built | failed | skipped
```

---

## Correctness Properties

*A property is a characteristic or behavior that should hold true across all valid executions of a system — essentially, a formal statement about what the system should do. Properties serve as the bridge between human-readable specifications and machine-verifiable correctness guarantees.*

### Property 1: README table parsing completeness

*For any* `README.md` containing a "Concluídos — jogáveis" Markdown table with N rows, `discover-ports.sh` must return a JSON array of exactly those N port names that appear as link targets in the "Pasta" column — no more, no fewer.

**Validates: Requirements 3.1**

### Property 2: Port name resolution validity

*For any* set of selected port names S and playable list P: if every name in S is in P, then the matrix must contain exactly the ports in S (when S = {"all"}, the matrix equals P). If any name in S is not in P, the resolve step must exit non-zero and print each non-playable name.

**Validates: Requirements 2.2, 2.3**

### Property 3: Build environment invariant

*For any* port in the build matrix, `build-port.sh` must invoke `build.sh` with CWD set to `ports/<portname>/`, `CC` set to the resolved path of `aarch64-linux-gnu-gcc-10`, and `SR` set to the absolute path of the bootstrapped sysroot.

**Validates: Requirements 1.2, 1.4, 4.1**

### Property 4: Binary is a valid AArch64 ELF

*For any* port binary produced by a successful `build.sh` invocation, the output of `file <binary>` must contain the substrings `ELF 64-bit LSB`, `aarch64`, and `dynamically linked`.

**Validates: Requirements 4.2**

### Property 5: Binary glibc version constraint

*For any* port binary, the highest `GLIBC_X.Y` versioned symbol dependency found by `objdump -p` / `readelf -V` must be no greater than `GLIBC_2.17`. Any higher version must cause `check-binary.sh` to exit non-zero and print the offending symbol names.

**Validates: Requirements 4.3**

### Property 6: Binary has no forbidden runtime bundles

*For any* port binary, the `DT_NEEDED` entries in the ELF dynamic section must not include `libSDL2.so*`, `libopenal.so*`, `libmali.so*`, or `libmpg123.so*`.

**Validates: Requirements 4.4**

### Property 7: Binary has no forbidden debug symbols

*For any* port binary, `nm --defined-only` must not list `watchdog_thread`, `heartbeat_thread`, `input_selftest`, `dump_framebuffer`, or `log_frame_pixels` as a non-weak (`T` or `D`) symbol. If any such symbol is present, `check-binary.sh` must exit non-zero listing all offending names.

**Validates: Requirements 6.1**

### Property 8: Zip contents correctness

*For any* port directory containing any combination of optional files (cover.png, gameinfo.xml, *.gptk, alsoft.conf, licenses/), the assembled zip must contain every mandatory file (launch .sh, port.json, README.md, screenshot.png, renamed binary), every optional file that is present in the source directory, and no file matching the forbidden patterns (*.apk, *.so, *.c, *.h, build*.sh, *.md other than README.md, STATUS*.md, CHANGELOG.md, HANDOFF*.md, *.o, dot-files other than permitted support files).

**Validates: Requirements 5.1, 5.3**

### Property 9: port.json items consistency with zip contents

*For any* port, after `assemble-zip.sh` completes, the `.items` array in the embedded `port.json` must exactly match the manifest of files placed in the zip (excluding the top-level launch `.sh` which is not a port subfolder item), and the binary entry in `.items` must be `<portname>/<portname>.aarch64`.

**Validates: Requirements 5.2, 5.4**

### Property 10: port.json arch field is always aarch64

*For any* port.json regardless of its original `.attr.arch` value (missing, wrong, or already correct), after `assemble-zip.sh` processes it the embedded `port.json` must have `.attr.arch == ["aarch64"]`.

**Validates: Requirements 5.7**

### Property 11: Zip filename matches port.json name field

*For any* `port.json` with any `.name` field value, the zip file produced by `assemble-zip.sh` must have a filename equal to that `.name` value.

**Validates: Requirements 5.6**

### Property 12: Discovery skips ports with missing directory or build.sh

*For any* port name that appears in the playable list but whose `ports/<name>/` directory does not exist, or whose `ports/<name>/build.sh` does not exist, `discover-ports.sh` must not include that name in the output matrix JSON and must emit a warning to stderr.

**Validates: Requirements 3.2, 3.3**

### Property 13: All action references are pinned to immutable tags

*For any* GitHub Actions step reference in the workflow YAML of the form `uses: owner/action`, the version specifier must be an immutable tag (e.g., `@v4`, `@v3`) and must not be `@main`, `@master`, `@latest`, or a bare branch name.

**Validates: Requirements 8.2**

### Property 14: Debug string check classifies correctly

*For any* binary whose `strings` output contains the literal `GLSTATE` or `GLDRAW`, `check-binary.sh` must emit a warning if the matching line has no adjacent guard string containing `_VERBOSE`; and must emit no warning if a guard string is present within ±5 lines of the match.

**Validates: Requirements 6.2**

---

## Error Handling

### Setup job failures

| Failure | Behaviour |
|---------|-----------|
| `apt-get` fails to install cross-compiler | Job exits non-zero; all build matrix jobs are cancelled immediately (`fail-fast: true` on the build matrix, combined with `needs: [setup]` making setup a prerequisite) |
| `debootstrap` fails | Setup job exits non-zero; same cancellation chain |
| README parse returns empty list | `discover-ports.sh` exits 1 with message "No playable ports found in README.md"; setup job fails |
| Named port not in playable list | `discover-ports.sh` exits 1 listing the unknown port names |
| Port in playable list but directory/build.sh missing | Warning logged, port excluded from matrix, job continues |

### Build job failures

| Failure | Behaviour |
|---------|-----------|
| `build.sh` exits non-zero | `build-port.sh` propagates exit code; full compiler stderr is in job log |
| Binary fails ELF arch check | `check-binary.sh` exits 1 with `file` output; job fails |
| Binary fails glibc check | `check-binary.sh` exits 1 listing offending GLIBC_X.Y symbol versions |
| Binary contains bundled lib | `check-binary.sh` exits 1 listing DT_NEEDED entry |
| Binary contains forbidden symbols | `check-binary.sh` exits 1 listing offending symbol names |
| Binary contains GLSTATE/GLDRAW without guard | WARNING in log, job continues |
| `port.json` missing | `assemble-zip.sh` exits 1 with missing file message |
| Manifest mismatch | `assemble-zip.sh` exits 1 and prints a `diff` of expected vs actual |

### Release job failures

| Failure | Behaviour |
|---------|-----------|
| `gh release` command fails | Release job fails; previously uploaded artifacts remain downloadable from the workflow run |
| Release asset already exists | `gh release upload --clobber` overwrites; no failure |
| No zips produced (all ports failed) | Release job has no assets to upload; it exits 0 with a note in the summary |

### Summary job

The summary job runs with `if: always()` so it executes even when build jobs fail. It reads the outcomes of each matrix job and writes a Markdown table to `$GITHUB_STEP_SUMMARY`:

```markdown
| Port | Outcome | Artifact |
|------|---------|---------|
| bully | ✅ built | [bully-portmaster-zip](url) |
| dysmantle | ❌ failed | — |
| sotn | ⏭ skipped | — |
```

---

## Testing Strategy

### Unit tests for helper scripts

Each CI helper script is testable in isolation using a local fixture directory structure. Tests are shell scripts under `ci/tests/` that can be run with `bash ci/tests/run-all.sh` locally and in CI.

**`ci/discover-ports.sh` tests:**
- Feed a synthetic README with a known playable table; verify output JSON matches expected names.
- Feed a README where one playable port has no directory; verify it is absent from output and a warning is printed.
- Feed a README with no playable table section; verify exit 1.
- (Property 1, Property 12)

**`ci/check-binary.sh` tests:**
- Build a minimal ELF with `as` that has `DT_NEEDED = libSDL2.so.0`; verify bundled-lib check fails.
- Use a binary compiled with `gcc -static` against a newish libc; verify glibc check fails.
- Use the actual bully binary (if available locally); verify all checks pass.
- Define a symbol `watchdog_thread` in a tiny C file; compile it; verify cleanliness check fails.
- (Properties 4, 5, 6, 7, 14)

**`ci/assemble-zip.sh` tests:**
- Fixture port with all optional files present; verify all are in zip, no forbidden files are included.
- Fixture port with no optional files; verify only mandatory files are in zip.
- Fixture port with a `.c` source file in the directory; verify it is not in the zip.
- Fixture port.json with `arch: ["armhf"]`; verify assembled port.json has `arch: ["aarch64"]`.
- Fixture port.json with `items: ["PortName.sh", "portname"]`; verify assembled items contain `portname/portname.aarch64`.
- Fixture with intentional mismatch between items and actual files; verify script exits 1.
- (Properties 8, 9, 10, 11)

### Property-based tests

For the helper scripts that implement pure data transformation logic (parsing, JSON mutation, path filtering), property-based tests are written in **Python using `hypothesis`**, targeting the specific functions extracted from the scripts or re-implemented as testable Python modules.

**Minimum 100 iterations per test** (hypothesis default; increase with `settings(max_examples=200)` for parser tests).

Tag format in test source: `# Feature: github-actions-port-builder, Property N: <property_text>`

**P1 — README parsing completeness:**
Generate random Markdown tables with random port names; verify `discover-ports.sh` returns exactly those names.

**P2 — Port name resolution:**
Generate random (playable_list, input_selector) pairs; verify matrix output and error behaviour match expectations.

**P3 — Build environment invariant:**
Not amenable to property-based test (requires actual toolchain execution); covered by integration test.

**P4–P7 — Binary validation properties:**
Generate synthetic ELF binaries using `pyelftools` with varying DT_NEEDED, GLIBC_VERSION, and symbol tables; verify `check-binary.sh` classifies each correctly.

**P8–P11 — Zip assembly properties:**
Generate random sets of files (using hypothesis `st.sets(st.sampled_from(possible_files))`); run `assemble-zip.sh` on fixture; verify zip contents, port.json mutations, and zip filename.

**P12 — Discovery skip on missing directory/build.sh:**
Generate random port names; for each, selectively omit the directory or build.sh; verify they are absent from the matrix JSON.

**P13 — Pinned action refs:**
Parse the workflow YAML and extract all `uses:` values; verify each matches the pattern `owner/action@v[0-9]+`.

**P14 — Debug string classifier:**
Generate strings output snippets with GLSTATE/GLDRAW present with and without `_VERBOSE` guards; verify the warning/no-warning classification is correct.

### Integration tests

Integration tests run the full pipeline against a lightweight mock port (a port with a `build.sh` that just generates a minimal ELF using `as` rather than compiling a real game) committed to `ci/testport/`. They run on every push to `main` as a separate workflow (`.github/workflows/ci-selftest.yml`) that takes less than 5 minutes.

This validates end-to-end: sysroot setup → compile mock port → all binary checks → zip assembly → artifact upload.

### What is NOT tested with property-based tests

- GitHub Actions YAML structure (SMOKE checks — verified by YAML linting with `actionlint`)
- `gh release` command integration (INTEGRATION — verified by the release job itself on tag pushes)
- apt package installation (SMOKE — runs in CI, not mocked)
- Artifact retention policy (INTEGRATION — GitHub-managed, not our code)
