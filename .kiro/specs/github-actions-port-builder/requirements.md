# Requirements Document

## Introduction

This feature adds a GitHub Actions CI/CD workflow to the `nextos_ports_android` repository. The workflow compiles so-loader port binaries for the `aarch64` architecture using a reproducible cross-compilation environment, then assembles and uploads a PortMaster-compatible zip artifact per port. Anyone who forks the repository can trigger the workflow and receive a ready-to-install zip — no local toolchain or NextOS Elite Edition installation required.

The workflow only builds ports listed in the "Concluídos — jogáveis" (playable) table in README.md. Each port that is not yet playable is silently skipped. The output zip follows the BYO-data model: it contains only the loader binary, launch script, and metadata; the user drops their legally-owned game APK into the unzipped folder before copying it to the handheld's SD card.

---

## Glossary

- **CI_Workflow**: The GitHub Actions YAML file that orchestrates the build pipeline.
- **Cross_Compiler**: `aarch64-linux-gnu-gcc-10` installed from the standard Debian/Ubuntu package repository on the GitHub Actions runner; exposed to port build scripts via the `$CC` environment variable.
- **Sysroot**: A Debian Bullseye aarch64 sysroot directory providing `SDL2`, `EGL`, `GLESv2`, `libpthread`, `libm`, and `libdl` headers and stub libraries; exposed to port build scripts via the `$SR` environment variable.
- **Port_Binary**: The single aarch64 ELF produced by compiling a port's `src/*.c` files; must link against glibc ≥ 2.17 and must not bundle runtime libraries (SDL2, OpenAL, libmali, mpg123).
- **Port_Binary_Name**: The arch-suffixed binary filename placed inside the PortMaster_Zip, following PortMaster convention: `<portname>.aarch64`.
- **Port_Directory**: The `ports/<portname>/` directory in the repository, containing `src/`, `build.sh`, the launch `.sh`, `port.json`, `README.md`, and `screenshot.png`.
- **Playable_Port**: A port whose directory name appears in the "Concluídos — jogáveis" table in the root `README.md`.
- **Playable_List**: The authoritative set of playable port names, parsed from the `README.md` table under "Concluídos — jogáveis".
- **PortMaster_Zip**: The output archive following the PortMaster packaging standard: a top-level launch `.sh` plus a `<portname>/` folder containing the Port_Binary, `port.json`, metadata, and support files — but no game data.
- **Artifact**: The PortMaster_Zip uploaded to GitHub Actions as a downloadable workflow artifact.
- **Release_Asset**: A GitHub Release file attachment created when the workflow is triggered by a `v*` version tag push.
- **Port_Selector**: The workflow dispatch input string identifying which port(s) to build; accepts a single port name, a comma-separated list, or `all`.
- **Build_Matrix**: The GitHub Actions matrix strategy that builds each selected port in a separate parallel job.
- **Packaging_Manifest**: The computed list of files placed into the PortMaster_Zip, which must exactly match the `items` array in `port.json`.

---

## Requirements

### Requirement 1: Reproducible Cross-Compilation Environment

**User Story:** As a contributor or fork owner, I want the CI environment to provide the exact same cross-compiler and sysroot as the local toolchain, so that the aarch64 binary produced in CI is identical in ABI to one built with a compatible local setup.

#### Acceptance Criteria

1. THE CI_Workflow SHALL install `gcc-10-aarch64-linux-gnu` and `binutils-aarch64-linux-gnu` from the standard Ubuntu/Debian apt package repository on the GitHub Actions runner, without requiring any manually maintained binary toolchain.
2. THE CI_Workflow SHALL set the `CC` environment variable to the resolved path of `aarch64-linux-gnu-gcc-10` before invoking any port's `build.sh`.
3. THE CI_Workflow SHALL construct a Sysroot containing headers and stub libraries for `SDL2`, `libEGL`, `libGLESv2`, `libpthread`, `libm`, and `libdl`, sufficient for each port's `build.sh` to link successfully.
4. THE CI_Workflow SHALL set the `SR` environment variable to the absolute path of the Sysroot before invoking any port's `build.sh`.
5. WHEN a port's `build.sh` exits with a non-zero status, THE CI_Workflow SHALL fail the corresponding build job and include the compiler error output in the job log.
6. THE CI_Workflow SHALL NOT require any secrets, credentials, or manually uploaded artifacts to reproduce the Cross_Compiler or Sysroot; all dependencies SHALL be fetchable from public package repositories.
7. THE CI_Workflow SHALL record and log the exact compiler version (`aarch64-linux-gnu-gcc-10 --version` output) and the git commit SHA used for each build, so that builds are reproducible from the log.

---

### Requirement 2: Port Selection via Workflow Dispatch

**User Story:** As a developer, I want to trigger the workflow for a specific port or for all playable ports at once, so that I can build and test a single port without waiting for the entire repository to compile.

#### Acceptance Criteria

1. THE CI_Workflow SHALL expose a `workflow_dispatch` trigger with a string input named `port` that accepts a single port name, a comma-separated list of port names, or the literal value `all`.
2. WHEN the `port` input is `all`, THE CI_Workflow SHALL build every port whose name appears in the Playable_List.
3. WHEN the `port` input is a single name or comma-separated list, THE CI_Workflow SHALL build only the named ports; IF any named port is not in the Playable_List, THE CI_Workflow SHALL fail with a descriptive error identifying the non-playable port names.
4. THE CI_Workflow SHALL use a Build_Matrix so that each selected port compiles in a separate parallel job.
5. WHEN a push is made to the default branch with no `workflow_dispatch` event, THE CI_Workflow SHALL NOT automatically trigger a build (builds are manual or tag-driven only).
6. WHEN a Git tag matching the pattern `v*` is pushed, THE CI_Workflow SHALL automatically build all Playable_List ports and additionally create Release_Assets (see Requirement 6).

---

### Requirement 3: Playable Port Discovery

**User Story:** As a maintainer, I want the CI to automatically pick up newly-graduated ports as soon as they are added to the "Concluídos — jogáveis" table in README.md, so that I do not need to maintain a separate config list.

#### Acceptance Criteria

1. THE CI_Workflow SHALL parse the root `README.md` to extract port directory names from the "Concluídos — jogáveis" Markdown table, using the Markdown link targets in the "Pasta" column (e.g., `ports/bully` → `bully`) as the authoritative Playable_List.
2. WHEN a port name appears in the Playable_List but its Port_Directory does not exist under `ports/`, THE CI_Workflow SHALL skip it and log a warning without failing the run.
3. WHEN a port name appears in the Playable_List and its Port_Directory is missing `build.sh`, THE CI_Workflow SHALL skip it and log a warning without failing the run.
4. THE CI_Workflow SHALL output the resolved Playable_List to the job log at the start of each run for traceability.

---

### Requirement 4: Port Binary Compilation

**User Story:** As an end user, I want the compiled binary to run on any aarch64 Linux device with glibc ≥ 2.17, so that the same zip works on muOS, ROCKNIX, and ArkOS handhelds without modification.

#### Acceptance Criteria

1. THE CI_Workflow SHALL invoke each port's `build.sh` from within the port's own directory (`ports/<portname>/`) with `CC` and `SR` set, preserving the existing build script interface.
2. THE CI_Workflow SHALL verify that the produced Port_Binary is an ELF file for `AArch64`, `LSB`, `dynamically linked` using the `file` command output; IF the check fails, THE CI_Workflow SHALL abort the job with an error message.
3. THE CI_Workflow SHALL verify that the Port_Binary requires a minimum glibc version no higher than `2.17` using `objdump -p` or `readelf --dynamic`; IF a higher glibc version requirement is detected, THE CI_Workflow SHALL abort the job with an error message listing the offending symbol versions.
4. THE CI_Workflow SHALL verify that the Port_Binary does NOT statically link or bundle `libSDL2.so`, `libopenal.so`, `libmali.so`, or `libmpg123.so`; runtime-only dependencies loaded via `dlopen` are acceptable and SHALL NOT trigger this check.
5. WHEN a `build.sh` does not exist in a Port_Directory that was explicitly selected, THE CI_Workflow SHALL fail the job with a human-readable message identifying the missing file.

---

### Requirement 5: PortMaster Zip Assembly

**User Story:** As an end user, I want to unzip the downloaded Artifact directly into `/ROMs/ports/` and have a playable structure immediately (after adding my APK), so that installation requires no manual file reorganisation.

#### Acceptance Criteria

1. THE CI_Workflow SHALL assemble the PortMaster_Zip with the following top-level structure:
   - `<Title Case>.sh` — the port's launch script (copied from `ports/<portname>/*.sh`; filename preserved as-is from the repo)
   - `<portname>/port.json`
   - `<portname>/README.md`
   - `<portname>/screenshot.png`
   - `<portname>/cover.png` — included only if present in the Port_Directory
   - `<portname>/gameinfo.xml` — included only if present in the Port_Directory
   - `<portname>/licenses/` — entire directory included only if present
   - `<portname>/<portname>.gptk` — included only if present
   - `<portname>/alsoft.conf` — included only if present
   - `<portname>/<Port_Binary_Name>` — the compiled ELF, renamed to `<portname>.aarch64`
2. THE CI_Workflow SHALL update the `items` array in the zip's embedded `port.json` to use the arch-suffixed binary name `<portname>.aarch64`, replacing any prior binary name entry.
3. THE CI_Workflow SHALL NOT include any file matching `*.apk`, `*.so` (game libraries), `*.obb`, `*.dat`, `assets/`, `libGame.so`, `libc++_shared.so`, `*.db`, `shared_prefs/`, `*.dex`, `*.c`, `*.h`, `CMakeLists.txt`, `build*.sh`, `STATUS*.md`, `PLAN*.md`, `CHANGELOG.md`, `HANDOFF*.md`, `*.md` files other than `README.md`, compiled intermediate `.o` files, or any file beginning with `.` other than permitted support files in the PortMaster_Zip.
4. THE CI_Workflow SHALL compute the Packaging_Manifest (the list of all items placed in the zip) and verify it matches the `items` array in the updated `port.json`; IF a mismatch is detected, THE CI_Workflow SHALL print a diff and fail the job.
5. WHEN a port's `port.json` is missing, THE CI_Workflow SHALL fail the packaging step with an error message identifying the missing file.
6. THE CI_Workflow SHALL name the output zip file using the `name` field from `port.json` (e.g., `bully.zip`).
7. THE CI_Workflow SHALL set the `arch` field in the zip's embedded `port.json` to `["aarch64"]` if it is not already set to that value.

---

### Requirement 6: Binary Cleanliness Validation

**User Story:** As an end user, I want the shipped binary to be production-clean so that it doesn't spam logs, selftest inputs, or dump framebuffer data during normal gameplay.

#### Acceptance Criteria

1. THE CI_Workflow SHALL scan the Port_Binary's symbol table using `nm --defined-only`; IF any of the symbol names `watchdog_thread`, `heartbeat_thread`, `input_selftest`, `dump_framebuffer`, or `log_frame_pixels` are present as non-weak defined symbols, THE CI_Workflow SHALL fail the job with a message listing the offending symbols.
2. THE CI_Workflow SHALL scan the Port_Binary's printable strings using `strings`; IF the literal substrings `GLSTATE` or `GLDRAW` appear without any adjacent environment-variable guard string (e.g., `_VERBOSE`), THE CI_Workflow SHALL emit a warning in the job log without failing the job, since the pattern may appear in string constants that are already gated.
3. THE CI_Workflow SHALL apply cleanliness checks only to freshly compiled Port_Binary artifacts produced in the current workflow run; it SHALL NOT check pre-existing binaries committed to the repository.

---

### Requirement 7: Artifact and Release Upload

**User Story:** As a developer or user, I want the PortMaster_Zip to be available for download directly from the GitHub Actions run page and as a GitHub Release asset on tagged releases, so that I can retrieve it without cloning the repository or building locally.

#### Acceptance Criteria

1. THE CI_Workflow SHALL upload each PortMaster_Zip as a GitHub Actions artifact named `<portname>-portmaster-zip` using `actions/upload-artifact`, retaining each artifact for a minimum of 14 days.
2. WHEN multiple ports are built in the same run, THE CI_Workflow SHALL upload a separate artifact per port, not a single merged archive.
3. WHEN the workflow is triggered by a `v*` tag push, THE CI_Workflow SHALL create or update a GitHub Release for that tag and attach each PortMaster_Zip as a Release_Asset.
4. WHEN a Release_Asset with the same filename already exists on the tag's release, THE CI_Workflow SHALL overwrite it rather than failing.
5. THE CI_Workflow SHALL NOT upload any game data, APK, `.so` game library, or asset file to artifacts, releases, or the repository at any point during the workflow.
6. THE CI_Workflow SHALL produce a summary table at the end of the run listing each port, its build outcome (skipped / built / failed), and the artifact download URL when a zip was produced.

---

### Requirement 8: Workflow Self-Containment and Security

**User Story:** As a fork owner, I want to fork the repository and run the workflow without any repository-specific configuration, so that I receive a working zip on the first attempt.

#### Acceptance Criteria

1. THE CI_Workflow SHALL require only the default `GITHUB_TOKEN` (automatically provided by GitHub Actions) for artifact upload and release creation; it SHALL NOT require any additional secrets or environment variables pre-configured by the user.
2. THE CI_Workflow SHALL pin all third-party Actions to a specific immutable version tag (e.g., `actions/checkout@v4`) rather than using floating `@main` or `@latest` references.
3. THE CI_Workflow SHALL NOT execute any script or binary fetched from a URL outside of official Debian/Ubuntu package mirrors, the GitHub Actions marketplace, or the repository itself.
4. THE CI_Workflow SHALL use `ubuntu-22.04` (or a later LTS runner) as the build host to ensure consistent package availability across runs.
5. THE CI_Workflow SHALL fail fast: if the cross-compiler installation step fails, all port build jobs SHALL be cancelled immediately without proceeding to zip assembly.
