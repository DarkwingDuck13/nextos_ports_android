# Implementation Plan: github-actions-port-builder

## Overview

Implement the GitHub Actions CI/CD pipeline for building and packaging PortMaster-compatible zip artifacts from so-loader port binaries. The implementation proceeds in six sequential stages: (1) CI helper scripts, (2) shell unit tests for those scripts, (3) Python Hypothesis property-based tests, (4) the main build workflow, (5) the selftest workflow with its mock port, and (6) any needed updates to existing ports (`ports/bully/port.json`).

All shell scripts go under `ci/`, all tests under `ci/tests/`, the two workflows under `.github/workflows/`. Property-based tests are written in Python using `hypothesis` and `pyelftools`.

---

## Tasks

- [-] 1. Create `ci/discover-ports.sh` — README parser
  - [x] 1.1 Implement `ci/discover-ports.sh`
    - Parse the "### ✅ Concluídos — jogáveis" Markdown table in the README path passed as `$1`
    - Extract link targets from the "Pasta" column (`ports/[a-z0-9_-]+`), strip `ports/` prefix, deduplicate, sort
    - For each name in the list: if `ports/<name>/` does not exist, emit a warning to stderr and exclude the name from output; if `ports/<name>/build.sh` is missing, emit a warning to stderr and exclude it
    - Emit a JSON array to stdout using `jq -R -s 'split("\n") | map(select(length>0))'`
    - Exit 1 only when the parsed list is empty (malformed README); exit 0 for partial warnings
    - _Requirements: 3.1, 3.2, 3.3, 3.4_

  - [x] 1.2 Write property test for discover-ports.sh — Property 1 (README parsing completeness)
    - **Property 1: README table parsing completeness**
    - **Validates: Requirements 3.1**
    - Generate random Markdown table bodies with random valid port names in the "Pasta" column using `hypothesis`; call `discover-ports.sh` with a synthetic README file; assert the output JSON array contains exactly those N names — no more, no fewer
    - Use `hypothesis` strategy `st.lists(st.text(alphabet=string.ascii_lowercase + string.digits + '-_', min_size=2, max_size=20), min_size=0, max_size=30, unique=True)` for port name sets
    - File: `ci/tests/test_discover_pbt.py`

  - [x] 1.3 Write property test for discover-ports.sh — Property 12 (skip missing dir/build.sh)
    - **Property 12: Discovery skips ports with missing directory or build.sh**
    - **Validates: Requirements 3.2, 3.3**
    - For generated port names, selectively omit their `ports/<name>/` directory or `build.sh`; verify those names are absent from the JSON output and that a warning was written to stderr
    - File: `ci/tests/test_discover_pbt.py`

- [-] 2. Create `ci/build-port.sh` — build invocation wrapper
  - [x] 2.1 Implement `ci/build-port.sh`
    - Accept `<portname>` as `$1`; require `CC` and `SR` to be set in the environment
    - If `ports/<portname>/build.sh` does not exist, print a human-readable error and exit 1
    - `cd ports/<portname>/`
    - Log `CC`, `SR`, `$(aarch64-linux-gnu-gcc-10 --version 2>&1)`, and `$(git rev-parse HEAD)` to stdout before running the build
    - Execute `bash build.sh` with `CC` and `SR` exported; propagate its exit code exactly
    - _Requirements: 1.2, 1.4, 1.5, 1.7, 4.1, 4.5_

  - [x] 2.2 Write property test for build-port.sh — Property 3 (build environment invariant)
    - **Property 3: Build environment invariant**
    - **Validates: Requirements 1.2, 1.4, 4.1**
    - Create a fixture `build.sh` that prints `CC=$CC SR=$SR PWD=$(pwd)` and exits 0; run `ci/build-port.sh` with various `CC` and `SR` values; assert printed values match exactly what was passed and that `PWD` is `ports/<portname>/` (absolute)
    - Hypothesis strategy: generate valid path strings for `CC` and `SR`; use a fixture port dir under a temp directory
    - File: `ci/tests/test_build_pbt.py`

- [-] 3. Create `ci/check-binary.sh` — binary quality gates
  - [x] 3.1 Implement ELF arch check and glibc version check in `ci/check-binary.sh`
    - Accept `<portname>` as `$1` and `<binary_path>` as `$2`
    - **ELF arch check**: run `file "$2"`; assert output contains `ELF 64-bit LSB`, `aarch64`, and `dynamically linked`; on failure print the `file` output and exit 1
    - **glibc version check**: parse all `GLIBC_X.Y` entries from `objdump -p "$2"` output; find the maximum (major, minor) pair; if greater than (2, 17) print the offending symbol versions and exit 1
    - _Requirements: 4.2, 4.3_

  - [x] 3.2 Implement bundled-lib check and cleanliness checks in `ci/check-binary.sh`
    - **Bundled-lib check**: parse `DT_NEEDED` entries from `readelf --dynamic "$2"`; if any entry matches `libSDL2.so*`, `libopenal.so*`, `libmali.so*`, or `libmpg123.so*`, print the offending entry and exit 1
    - **Cleanliness — symbols**: run `nm --defined-only "$2"`; if any line shows `watchdog_thread`, `heartbeat_thread`, `input_selftest`, `dump_framebuffer`, or `log_frame_pixels` with type `T` or `D` (non-weak), print offending names and exit 1
    - **Cleanliness — strings**: run `strings "$2"`; for every match of `GLSTATE` or `GLDRAW`, check whether any line within ±5 lines contains `_VERBOSE`; if not, emit a WARNING to stdout (do not exit 1)
    - _Requirements: 4.4, 6.1, 6.2, 6.3_

  - [ ] 3.3 Write property test for check-binary.sh — Properties 4–7 (binary validation)
    - **Property 4: Binary is a valid AArch64 ELF** — **Validates: Requirements 4.2**
    - **Property 5: Binary glibc version constraint** — **Validates: Requirements 4.3**
    - **Property 6: Binary has no forbidden runtime bundles** — **Validates: Requirements 4.4**
    - **Property 7: Binary has no forbidden debug symbols** — **Validates: Requirements 6.1**
    - Use `pyelftools` to generate minimal synthetic ELF binaries with varying `DT_NEEDED`, `GLIBC_VERSION` sets, and symbol tables; run `check-binary.sh` on each and assert exit code and stdout match expected outcome
    - Hypothesis strategies: `st.frozensets(st.sampled_from([...libs...]))` for DT_NEEDED sets; `st.tuples(st.integers(2,5), st.integers(0,40))` for glibc version pairs; `st.frozensets(st.sampled_from([...forbidden_symbols...]))` for symbol sets
    - File: `ci/tests/test_check_binary_pbt.py`

  - [ ] 3.4 Write property test for check-binary.sh — Property 14 (debug string classifier)
    - **Property 14: Debug string check classifies correctly**
    - **Validates: Requirements 6.2**
    - Generate synthetic `strings` output snippets containing `GLSTATE`/`GLDRAW` with and without `_VERBOSE` guards at various distances (0–10 lines); run the classification logic from `check-binary.sh` (extracted to a small helper or called via a wrapper); assert WARNING is emitted iff no guard within ±5 lines is present
    - File: `ci/tests/test_check_binary_pbt.py`

- [-] 4. Create `ci/assemble-zip.sh` — PortMaster zip assembler
  - [x] 4.1 Implement file staging and port.json patching in `ci/assemble-zip.sh`
    - Accept `<portname>` as `$1`, `<binary_path>` as `$2`, `<output_dir>` as `$3`
    - Read `ports/<portname>/port.json`; exit 1 with message if missing
    - Determine zip name from `.name` field
    - Create staging dir `<output_dir>/staging/<portname>/`
    - Copy mandatory files: `ports/<portname>/*.sh` → staging top level; `port.json`, `README.md`, `screenshot.png` → `staging/<portname>/`; binary → `staging/<portname>/<portname>.aarch64`
    - Copy optional files only if present: `cover.png`, `gameinfo.xml`, `*.gptk`, `alsoft.conf` → `staging/<portname>/`; `licenses/` dir → `staging/<portname>/licenses/`
    - Patch the staged `port.json`: set `.attr.arch = ["aarch64"]`; in `.items`, replace the non-`.sh` binary entry with `"<portname>/<portname>.aarch64"`; ensure all port-subfolder items carry the `<portname>/` prefix
    - _Requirements: 5.1, 5.2, 5.5, 5.6, 5.7_

  - [x] 4.2 Implement manifest validation and zip creation in `ci/assemble-zip.sh`
    - Compute manifest: list all files in `staging/` relative to `staging/`, sorted
    - Validate manifest vs updated `.items` in `port.json`: they must match exactly (after path normalisation); on mismatch print a `diff` of expected vs actual and exit 1
    - Assemble zip: `cd <output_dir>/staging && zip -r <output_dir>/<zipname> .`
    - Remove staging directory
    - _Requirements: 5.3, 5.4_

  - [x] 4.3 Write property test for assemble-zip.sh — Properties 8–11 (zip assembly)
    - **Property 8: Zip contents correctness** — **Validates: Requirements 5.1, 5.3**
    - **Property 9: port.json items consistency with zip contents** — **Validates: Requirements 5.2, 5.4**
    - **Property 10: port.json arch field is always aarch64** — **Validates: Requirements 5.7**
    - **Property 11: Zip filename matches port.json name field** — **Validates: Requirements 5.6**
    - Use Hypothesis `st.sets(st.sampled_from(possible_optional_files))` to generate random combinations of optional files present in a fixture port directory; run `assemble-zip.sh` on the fixture; inspect the zip with Python `zipfile`; assert mandatory files always present, optional files iff in source, forbidden patterns absent, `port.json` mutations correct, zip filename correct
    - File: `ci/tests/test_assemble_zip_pbt.py`

- [ ] 5. Create shell unit tests under `ci/tests/`
  - [x] 5.1 Create `ci/tests/run-all.sh` test runner
    - Iterate over `ci/tests/test_*.sh`; run each; track pass/fail counts; exit non-zero if any test failed
    - Print a summary line per test: `PASS`, `FAIL`, or `SKIP`

  - [x] 5.2 Write shell unit tests for `discover-ports.sh`
    - File: `ci/tests/test_discover.sh`
    - Test: synthetic README with 3 known port names → JSON array equals those 3 names
    - Test: README where one playable port directory is absent → that name excluded, warning on stderr, exit 0
    - Test: README with no "Concluídos" section → exit 1
    - Test: README where a port directory exists but `build.sh` is missing → that name excluded, warning on stderr
    - _Requirements: 3.1, 3.2, 3.3_

  - [ ] 5.3 Write shell unit tests for `check-binary.sh`
    - File: `ci/tests/test_check_binary.sh`
    - Fixture: minimal ELF (compiled with `as`) with `DT_NEEDED = libSDL2.so.0` → bundled-lib check exits 1
    - Fixture: tiny C file defining `void watchdog_thread(void){}` compiled to ELF → cleanliness symbol check exits 1
    - Fixture: clean minimal ELF with no forbidden DT_NEEDED or symbols → all checks pass (exit 0)
    - _Requirements: 4.4, 6.1_

  - [x] 5.4 Write shell unit tests for `assemble-zip.sh`
    - File: `ci/tests/test_assemble_zip.sh`
    - Fixture: port with all optional files (cover.png, gameinfo.xml, *.gptk, alsoft.conf, licenses/) → all present in zip, no forbidden patterns
    - Fixture: port with no optional files → only mandatory files in zip
    - Fixture: port directory containing a `.c` source file → `.c` must NOT appear in zip
    - Fixture: `port.json` with `"arch": ["armhf"]` → assembled `port.json` has `"arch": ["aarch64"]`
    - Fixture: `port.json` with `"items": ["PortName.sh", "portname"]` (bare binary name) → assembled items contain `"portname/portname.aarch64"`
    - Fixture: intentional mismatch between items and actual files → script exits 1 and prints diff
    - _Requirements: 5.1, 5.2, 5.3, 5.4, 5.7_

  - [x] 5.5 Checkpoint — run all shell unit tests
    - Run `bash ci/tests/run-all.sh` and confirm all shell tests pass; investigate and fix any failures before proceeding.

- [ ] 6. Write Python Hypothesis property-based tests
  - [ ] 6.1 Create `ci/tests/requirements.txt` and test scaffolding
    - Add `hypothesis>=6.100`, `pyelftools>=0.31` to `ci/tests/requirements.txt`
    - Create `ci/tests/conftest.py` with shared fixtures: temp fixture port directory builder (`make_fixture_port`), minimal ELF builder using `pyelftools` (`make_elf`), and a helper to call a shell script from Python and capture stdout/stderr/exit code

  - [ ] 6.2 Implement `ci/tests/test_discover_pbt.py` (Properties 1, 12)
    - **Property 1**: as described in task 1.2 — generated README tables, exact JSON output match
    - **Property 12**: as described in task 1.3 — selectively missing dirs/build.sh → excluded from JSON with stderr warning
    - `settings(max_examples=200)` for both properties
    - Tag comments: `# Feature: github-actions-port-builder, Property 1: README parsing completeness`
    - _Requirements: 3.1, 3.2, 3.3_

  - [ ] 6.3 Implement `ci/tests/test_build_pbt.py` (Property 3)
    - **Property 3**: as described in task 2.2 — verify `CC`, `SR`, and `CWD` invariants
    - `settings(max_examples=100)`
    - Tag comment: `# Feature: github-actions-port-builder, Property 3: Build environment invariant`
    - _Requirements: 1.2, 1.4, 4.1_

  - [ ] 6.4 Implement `ci/tests/test_check_binary_pbt.py` (Properties 4–7, 14)
    - **Property 4**: synthetic ELF with correct/incorrect arch → exit 0 / exit 1
    - **Property 5**: synthetic ELF with various GLIBC version sets → exit 1 iff max version > 2.17
    - **Property 6**: synthetic ELF with various DT_NEEDED sets → exit 1 iff forbidden lib present
    - **Property 7**: synthetic ELF with various defined symbol sets → exit 1 iff forbidden non-weak symbol present
    - **Property 14**: generate strings-output snippets with GLSTATE/GLDRAW at various guard distances → warning iff no guard within ±5 lines
    - `settings(max_examples=200)` for Properties 5 and 14; `settings(max_examples=100)` for others
    - Tag comments for each property
    - _Requirements: 4.2, 4.3, 4.4, 6.1, 6.2_

  - [ ] 6.5 Implement `ci/tests/test_assemble_zip_pbt.py` (Properties 8–11)
    - **Property 8**: random optional-file sets → zip contents correctness
    - **Property 9**: port.json items consistency with zip manifest
    - **Property 10**: port.json arch always set to `["aarch64"]` regardless of input value
    - **Property 11**: zip filename always equals `port.json .name` field
    - `settings(max_examples=200)`
    - Tag comments for each property
    - _Requirements: 5.1, 5.2, 5.3, 5.4, 5.6, 5.7_

  - [ ] 6.6 Checkpoint — run all Python property-based tests
    - Install dependencies: `pip install -r ci/tests/requirements.txt`
    - Run `python -m pytest ci/tests/test_*_pbt.py -v` and confirm all properties pass; fix any falsifying examples found by Hypothesis.

- [ ] 7. Create `ci/testport/` — mock port for integration testing
  - [x] 7.1 Create `ci/testport/build.sh` — minimal ELF generator
    - Generate a minimal valid aarch64 ELF using `aarch64-linux-gnu-gcc-10 --sysroot=$SR -o testport ci/testport/src/main.c -lm -lpthread` (or using `aarch64-linux-gnu-as` for a pure assembly stub that links with no forbidden libs and has glibc ≤ 2.17)
    - The ELF must pass all four checks in `check-binary.sh`: correct arch, glibc ≤ 2.17, no forbidden DT_NEEDED, no forbidden symbols
    - _Requirements: 4.1, 4.2, 4.3, 4.4_

  - [ ] 7.2 Create `ci/testport/src/main.c`, `ci/testport/port.json`, `ci/testport/README.md`, `ci/testport/screenshot.png`, and `ci/testport/Testport.sh`
    - `main.c`: minimal `int main(void){ return 0; }` — no forbidden symbols, no forbidden DT_NEEDED, no GLSTATE/GLDRAW strings
    - `port.json`: valid version-2 JSON with `"name": "testport.zip"`, `"items": ["Testport.sh", "testport"]`, `"attr": {"title": "CI Testport", "arch": ["aarch64"]}`
    - `README.md`: brief description marking this as a CI test fixture, not a real port
    - `screenshot.png`: 1×1 transparent PNG (valid file, not game data)
    - `Testport.sh`: minimal launcher stub (no systemctl, no watchdog, no emustation references)
    - _Requirements: 5.1, 5.5_

- [ ] 8. Create `.github/workflows/build-ports.yml` — main CI workflow
  - [x] 8.1 Implement the `setup` job in `build-ports.yml`
    - Declare triggers: `on.workflow_dispatch.inputs.port` (string, default `all`) and `on.push.tags` pattern `v*`; no trigger on plain branch pushes (_Requirements: 2.1, 2.5, 2.6_)
    - `setup` job on `ubuntu-22.04`
    - Step: `actions/checkout@v4` (pinned immutable tag)
    - Step: `apt-get install -y gcc-10-aarch64-linux-gnu binutils-aarch64-linux-gnu debootstrap jq` — log package versions; exit non-zero propagates to cancel all build jobs (`fail-fast: true` on the matrix) (_Requirements: 1.1, 8.4, 8.5_)
    - Step: bootstrap Debian Bullseye aarch64 sysroot with `debootstrap --arch=arm64 --variant=minbase --include=libsdl2-dev,libegl-dev,libgles2-mesa-dev`; tarball it as `ci-sysroot.tar.gz` (_Requirements: 1.3_)
    - Step: `actions/upload-artifact@v4` uploading `ci-sysroot.tar.gz` as artifact `ci-sysroot`
    - Step: run `ci/discover-ports.sh README.md`; resolve `port` dispatch input against playable list; emit matrix JSON as a job output `matrix`; fail with descriptive message if any named port is not in the playable list (_Requirements: 2.2, 2.3, 3.1, 3.4_)
    - Step: log resolved playable list and compiler version (_Requirements: 1.7_)
    - _Requirements: 1.1, 1.6, 2.1, 2.2, 2.3, 8.1, 8.2, 8.3, 8.4_

  - [x] 8.2 Implement the `build` matrix job in `build-ports.yml`
    - `build` job: `needs: [setup]`, `strategy: matrix: port: ${{ fromJson(needs.setup.outputs.matrix) }}`, `fail-fast: false` (allow other ports to continue if one fails)
    - Step: `actions/checkout@v4`
    - Step: `actions/download-artifact@v4` downloading `ci-sysroot`; extract tarball; set `SR` env var to absolute path (_Requirements: 1.4_)
    - Step: set `CC` env var to `aarch64-linux-gnu-gcc-10` resolved path (_Requirements: 1.2_)
    - Step: run `ci/build-port.sh ${{ matrix.port }}`; full stderr in log on failure (_Requirements: 1.5, 4.1, 4.5_)
    - Step: run `ci/check-binary.sh ${{ matrix.port }} ports/${{ matrix.port }}/${{ matrix.port }}`; abort on any exit 1 (_Requirements: 4.2, 4.3, 4.4, 6.1, 6.2_)
    - Step: run `ci/assemble-zip.sh ${{ matrix.port }} ports/${{ matrix.port }}/${{ matrix.port }} dist/`; abort on exit 1 (_Requirements: 5.1–5.7_)
    - Step: `actions/upload-artifact@v4` uploading `dist/<portname>.zip` as `${{ matrix.port }}-portmaster-zip` with `retention-days: 14`; set output `artifact_url` (_Requirements: 7.1, 7.2_)
    - _Requirements: 1.2, 1.4, 1.5, 4.1–4.5, 5.1–5.7, 7.1, 7.2, 8.2_

  - [x] 8.3 Implement the `release` job in `build-ports.yml`
    - `release` job: `needs: [build]`, `if: startsWith(github.ref, 'refs/tags/v')`
    - Step: `actions/checkout@v4`
    - Step: download all `*-portmaster-zip` artifacts using `actions/download-artifact@v4`
    - Step: `gh release create "${{ github.ref_name }}" --generate-notes` (or update if exists); attach each zip using `gh release upload "${{ github.ref_name }}" <zip> --clobber`
    - Requires only `GITHUB_TOKEN` from the default `secrets.GITHUB_TOKEN` (_Requirements: 7.3, 7.4, 8.1_)
    - _Requirements: 2.6, 7.3, 7.4, 7.5_

  - [x] 8.4 Implement the `summary` job in `build-ports.yml`
    - `summary` job: `needs: [build]`, `if: always()`
    - Read outcome of each matrix job; write a Markdown table to `$GITHUB_STEP_SUMMARY` with columns Port, Outcome (✅ built / ❌ failed / ⏭ skipped), Artifact URL
    - _Requirements: 7.6_

  - [x] 8.5 Write property test for `build-ports.yml` — Property 13 (pinned action references)
    - **Property 13: All action references are pinned to immutable tags**
    - **Validates: Requirements 8.2**
    - Parse `.github/workflows/build-ports.yml` with Python `yaml`; extract all `uses:` values from every step; assert each matches `r'^[a-zA-Z0-9_-]+/[a-zA-Z0-9_-]+@v[0-9]'`; assert none matches `@main`, `@master`, or `@latest`
    - File: `ci/tests/test_workflow_yaml_pbt.py`

  - [ ] 8.6 Checkpoint — validate the workflow YAML
    - Run `python -m pytest ci/tests/test_workflow_yaml_pbt.py -v` to confirm all action refs are pinned.

- [ ] 9. Create `.github/workflows/ci-selftest.yml` — integration selftest workflow
  - [x] 9.1 Implement `ci-selftest.yml`
    - Trigger: `on.push` (branches: `master`) and `on.pull_request`; run in under 5 minutes
    - Single job on `ubuntu-22.04`; `actions/checkout@v4` (pinned)
    - Install cross-compiler and dependencies: `gcc-10-aarch64-linux-gnu binutils-aarch64-linux-gnu debootstrap jq python3-pip`
    - Bootstrap sysroot (same `debootstrap` command as the main workflow)
    - Set `CC` and `SR`; run `ci/build-port.sh testport` where `testport`'s `build.sh` builds from `ci/testport/src/main.c`
    - Run `ci/check-binary.sh testport ci/testport/testport` — must pass all checks
    - Run `ci/assemble-zip.sh testport ci/testport/testport dist/` — must produce `dist/testport.zip`
    - Verify zip contents with a small inline Python snippet or `unzip -l`
    - `actions/upload-artifact@v4` uploading `dist/testport.zip` as `testport-selftest-zip` to confirm artifact upload works end-to-end
    - All action refs pinned to immutable tags
    - _Requirements: 8.1, 8.2, 8.3, 8.4_

- [x] 10. Update `ports/bully/port.json` to use arch-suffixed binary naming
  - [x] 10.1 Update `ports/bully/port.json` `items` array
    - Replace the bare `"bully"` binary entry in `items` with `"bully/bully.aarch64"` (arch-suffixed, with portname prefix)
    - Ensure `"Bully.sh"` is the first item (top-level launch script, no portname prefix)
    - Ensure `"attr.arch"` is set to `["aarch64"]` if not already
    - Add `"bully/README.md"`, `"bully/screenshot.png"`, `"bully/port.json"` to `items` if not already present, to match the manifest that `assemble-zip.sh` will produce
    - _Requirements: 5.1, 5.2, 5.7_

- [ ] 11. Final checkpoint — end-to-end validation
  - [ ] 11.1 Run all shell tests and Python PBT tests
    - Run `bash ci/tests/run-all.sh`
    - Run `python -m pytest ci/tests/ -v`
    - Fix any remaining failures; all tests must be green before marking this task complete.
  - [ ] 11.2 Validate workflow YAML with actionlint (if available)
    - If `actionlint` is available in the environment, run `actionlint .github/workflows/build-ports.yml .github/workflows/ci-selftest.yml`; fix any issues reported
    - This is a smoke-level check; skip gracefully if actionlint is not installed

---

## Notes

- Tasks marked with `*` are optional; tasks without `*` are required and must be implemented
- Property-based tests (tasks 1.2, 1.3, 2.2, 3.3, 3.4, 4.3, 6.2–6.5, 8.5) are **required** per the user's specification — they are NOT marked optional
- Each property test references the design document's Correctness Properties section (Properties 1–14) and the Requirements clause it validates
- The `ci/testport/` fixture is a real directory committed to the repository, not generated at test time; its binaries are never committed (only source and metadata)
- The `debootstrap` sysroot is built at CI run time and shared via a GitHub Actions artifact (`ci-sysroot`) to avoid rebuilding it in every matrix job
- All third-party GitHub Actions are pinned to `@v4` or equivalent immutable tags; Property 13 enforces this automatically
- `ports/bully/port.json` update (Task 10) may reveal whether other completed ports also need the same `items` migration; apply to each port's `port.json` as the same pattern when `assemble-zip.sh` is run against them
