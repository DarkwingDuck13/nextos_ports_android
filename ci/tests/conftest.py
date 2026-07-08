"""
ci/tests/conftest.py — shared pytest fixtures for property-based tests.
Feature: github-actions-port-builder
"""
import json, os, shutil, subprocess, tempfile
import pytest


@pytest.fixture
def tmp_repo(tmp_path):
    """Create a minimal fake repo root with a ports/ directory."""
    (tmp_path / "ports").mkdir()
    return tmp_path


def make_fixture_port(repo_root, portname, optional_files=(), arch="aarch64"):
    """
    Create a minimal port directory under repo_root/ports/<portname>/.
    Returns path to the port directory.
    """
    port_dir = repo_root / "ports" / portname
    port_dir.mkdir(parents=True, exist_ok=True)
    (port_dir / "build.sh").write_text("#!/bin/bash\necho ok\n")
    (port_dir / "README.md").write_text(f"# {portname}\n")
    (port_dir / "screenshot.png").write_bytes(b"\x89PNG\r\n\x1a\n")  # minimal PNG header
    (port_dir / f"{portname}.sh").write_text("#!/bin/bash\n./binary\n")
    port_json = {
        "version": 2,
        "name": f"{portname}.zip",
        "items": [f"{portname}.sh", portname],
        "items_opt": [],
        "attr": {
            "title": portname, "porter": ["test"], "desc": "test",
            "inst": "test", "genres": ["other"], "image": {}, "rtr": False,
            "runtime": [], "reqs": [], "arch": [arch]
        }
    }
    (port_dir / "port.json").write_text(json.dumps(port_json, indent=2))
    # Create a stub binary file
    (port_dir / portname).write_bytes(b"\x00")

    for fname in optional_files:
        if fname == "licenses/":
            (port_dir / "licenses").mkdir(exist_ok=True)
            (port_dir / "licenses" / "LICENSE.md").write_text("MIT\n")
        else:
            (port_dir / fname).write_bytes(b"\x00")

    return port_dir


def run_script(script_path, *args, env=None, capture=True):
    """Run a shell script and return (returncode, stdout, stderr)."""
    full_env = os.environ.copy()
    if env:
        full_env.update(env)
    result = subprocess.run(
        ["bash", str(script_path)] + list(args),
        capture_output=capture,
        text=True,
        env=full_env,
    )
    return result.returncode, result.stdout, result.stderr
