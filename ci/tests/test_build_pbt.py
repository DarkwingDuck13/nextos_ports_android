"""
ci/tests/test_build_pbt.py — Property-based tests for ci/build-port.sh

Feature: github-actions-port-builder
Property 3: Build environment invariant (validates Requirements 1.2, 1.4, 4.1)

Run with:  pytest ci/tests/test_build_pbt.py -v
Requires:  pip install -r ci/tests/requirements.txt
"""
import os
from pathlib import Path

from hypothesis import given, settings, HealthCheck
from hypothesis import strategies as st

from conftest import run_script

BUILD_PORT = Path(__file__).parent.parent / "build-port.sh"

# Strategy: safe path components (no shell metacharacters)
safe_path_st = st.text(
    alphabet="abcdefghijklmnopqrstuvwxyz0123456789_-",
    min_size=1,
    max_size=20,
)


# ── Property 3: Build environment invariant ───────────────────────────────────
# Feature: github-actions-port-builder, Property 3: Build environment invariant

@given(
    cc_suffix=safe_path_st,
    sr_suffix=safe_path_st,
)
@settings(max_examples=100, suppress_health_check=[HealthCheck.too_slow])
def test_property3_build_env_invariant(cc_suffix, sr_suffix, tmp_path):
    """
    build-port.sh must invoke build.sh with CWD=ports/<portname>/,
    and the CC and SR env vars must be exactly what was passed in.
    """
    portname = "envtest"
    port_dir = tmp_path / "ports" / portname
    port_dir.mkdir(parents=True)

    # Fake CC and SR paths (don't need to exist — build.sh just echoes them)
    fake_cc = f"/fake/cc/{cc_suffix}"
    fake_sr = f"/fake/sr/{sr_suffix}"

    # A build.sh that records what it sees and exits 0
    (port_dir / "build.sh").write_text(
        "#!/bin/bash\n"
        'echo "SAW_CC=$CC"\n'
        'echo "SAW_SR=$SR"\n'
        'echo "SAW_PWD=$(pwd)"\n'
        "exit 0\n"
    )

    rc, stdout, stderr = run_script(
        BUILD_PORT,
        portname,
        str(tmp_path),
        env={"CC": fake_cc, "SR": fake_sr},
    )

    assert rc == 0, f"build-port.sh failed unexpectedly.\nstdout: {stdout}\nstderr: {stderr}"
    assert f"SAW_CC={fake_cc}" in stdout, f"CC not propagated correctly.\nstdout: {stdout}"
    assert f"SAW_SR={fake_sr}" in stdout, f"SR not propagated correctly.\nstdout: {stdout}"

    expected_pwd = str(tmp_path / "ports" / portname)
    assert f"SAW_PWD={expected_pwd}" in stdout, (
        f"CWD not set to port directory.\nExpected: {expected_pwd}\nstdout: {stdout}"
    )


def test_property3_missing_build_sh_exits_nonzero(tmp_path):
    """build-port.sh must exit non-zero with a clear message when build.sh is missing."""
    portname = "nobuilds"
    (tmp_path / "ports" / portname).mkdir(parents=True)

    rc, stdout, stderr = run_script(
        BUILD_PORT,
        portname,
        str(tmp_path),
        env={"CC": "/fake/cc", "SR": "/fake/sr"},
    )
    assert rc != 0, "Should fail when build.sh is missing"
    combined = stdout + stderr
    assert "build.sh" in combined.lower() or "not found" in combined.lower()
