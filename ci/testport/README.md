# CI Testport — Integration Test Fixture

> **This is not a real game port.**

This directory is a minimal stub port used to verify the GitHub Actions CI
build pipeline end-to-end without requiring a real game binary or APK.

It compiles a `main.c` that immediately returns `EXIT_SUCCESS`, then runs the
binary through all quality gates in `ci/check-binary.sh` and packages it with
`ci/assemble-zip.sh`.

Do not copy this to your device — there is no game here.
