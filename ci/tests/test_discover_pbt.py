"""
ci/tests/test_discover_pbt.py — Property-based tests for ci/discover-ports.sh

Feature: github-actions-port-builder
Property 1:  README table parsing completeness (validates Requirements 3.1)
Property 12: Discovery skips ports with missing directory or build.sh (validates Requirements 3.2, 3.3)

Run with:  pytest ci/tests/test_discover_pbt.py -v
Requires:  pip install -r ci/tests/requirements.txt
"""
import json, os, string, textwrap
from pathlib import Path

import pytest
from hypothesis import given, settings, HealthCheck
from hypothesis import strategies as st

from conftest import run_script

DISCOVER = Path(__file__).parent.parent / "discover-ports.sh"

# Strategy: valid port names (lowercase letters, digits, hyphens, underscores, min 2 chars)
port_name_st = st.text(
    alphabet=string.ascii_lowercase + string.digits + "-_",
    min_size=2,
    max_size=20,
).filter(lambda s: s[0].isalnum())  # must start with letter or digit


def make_readme(tmp_path: Path, port_names: list) -> Path:
    """Write a minimal README with a Concluídos table listing the given port names."""
    readme = tmp_path / "README.md"
    rows = "\n".join(
        f"| **{n}** | so-loader | Jogável | [`ports/{n}`](ports/{n}/) |"
        for n in port_names
    )
    readme.write_text(textwrap.dedent(f"""
        # Test Repo

        ### ✅ Concluídos — jogáveis
        | Jogo | Engine | Estado | Pasta |
        |---|---|---|---|
        {rows}

        ### 🚧 Em andamento
        | Jogo | Engine | Estado | Pasta |
        |---|---|---|---|
    """))
    return readme


def make_port(repo: Path, name: str, has_dir: bool = True, has_build: bool = True):
    """Create a port directory with or without build.sh."""
    port_dir = repo / "ports" / name
    if has_dir:
        port_dir.mkdir(parents=True, exist_ok=True)
        if has_build:
            (port_dir / "build.sh").write_text("#!/bin/bash\necho ok\n")


# ── Property 1: README parsing completeness ──────────────────────────────────
# Feature: github-actions-port-builder, Property 1: README table parsing completeness

@given(st.lists(port_name_st, min_size=1, max_size=15, unique=True))
@settings(max_examples=200, suppress_health_check=[HealthCheck.too_slow])
def test_property1_all_ports_discovered(port_names, tmp_path):
    """All port names in the Concluídos table are returned when dirs+build.sh exist."""
    readme = make_readme(tmp_path, port_names)
    for name in port_names:
        make_port(tmp_path, name, has_dir=True, has_build=True)

    rc, stdout, stderr = run_script(DISCOVER, str(readme), str(tmp_path))
    assert rc == 0, f"discover-ports.sh failed: {stderr}"

    result = json.loads(stdout)
    assert sorted(result) == sorted(port_names), (
        f"Expected {sorted(port_names)}, got {sorted(result)}\nstderr: {stderr}"
    )


# ── Property 12: Discovery skips ports with missing directory or build.sh ─────
# Feature: github-actions-port-builder, Property 12: Discovery skips ports with missing directory or build.sh

@given(
    st.lists(port_name_st, min_size=2, max_size=10, unique=True),
    st.integers(min_value=0, max_value=1),  # 0=missing dir, 1=missing build.sh
)
@settings(max_examples=200, suppress_health_check=[HealthCheck.too_slow])
def test_property12_missing_dir_or_build_excluded(port_names, missing_kind, tmp_path):
    """A port with missing directory or build.sh is excluded from output with a stderr warning."""
    # First port is good, last port has the defect
    good_ports = port_names[:-1]
    bad_port = port_names[-1]

    readme = make_readme(tmp_path, port_names)
    for name in good_ports:
        make_port(tmp_path, name, has_dir=True, has_build=True)

    if missing_kind == 0:
        make_port(tmp_path, bad_port, has_dir=False, has_build=False)
    else:
        make_port(tmp_path, bad_port, has_dir=True, has_build=False)

    rc, stdout, stderr = run_script(DISCOVER, str(readme), str(tmp_path))

    if not good_ports:
        # All ports are bad → expect exit 1
        assert rc != 0
        return

    assert rc == 0, f"Should exit 0 when some good ports exist. stderr: {stderr}"
    result = json.loads(stdout)

    assert bad_port not in result, f"Bad port '{bad_port}' should be excluded, got {result}"
    assert "WARNING" in stderr, f"Expected WARNING in stderr for missing port, got: {stderr}"
    for name in good_ports:
        assert name in result, f"Good port '{name}' should be in result, got {result}"
