"""
ci/tests/test_assemble_zip_pbt.py — Property-based tests for ci/assemble-zip.sh

Feature: github-actions-port-builder
Property 8:  Zip contents correctness       (validates Requirements 5.1, 5.3)
Property 9:  port.json items consistency    (validates Requirements 5.2, 5.4)
Property 10: port.json arch always aarch64  (validates Requirements 5.7)
Property 11: Zip filename = port.json .name (validates Requirements 5.6)

Run with:  pytest ci/tests/test_assemble_zip_pbt.py -v
Requires:  pip install -r ci/tests/requirements.txt
"""
import json, zipfile
from pathlib import Path

from hypothesis import given, settings, HealthCheck
from hypothesis import strategies as st

from conftest import make_fixture_port, run_script

ASSEMBLE = Path(__file__).parent.parent / "assemble-zip.sh"

OPTIONAL_FILES = ["cover.png", "gameinfo.xml", "alsoft.conf", "licenses/"]
GPTK_FILE = "testgame.gptk"
ALL_OPTIONALS = OPTIONAL_FILES + [GPTK_FILE]

arch_st = st.sampled_from(["aarch64", "armhf", "x86_64", "arm"])
optional_set_st = st.frozensets(st.sampled_from(ALL_OPTIONALS), max_size=len(ALL_OPTIONALS))


def run_assemble(portname, tmp_path, optional_files=(), arch="aarch64"):
    port_dir = make_fixture_port(tmp_path, portname, optional_files=optional_files, arch=arch)
    dist = tmp_path / "dist"
    dist.mkdir(exist_ok=True)
    binary = port_dir / portname
    rc, stdout, stderr = run_script(
        ASSEMBLE,
        portname,
        str(binary),
        str(dist),
        str(tmp_path),
    )
    return rc, stdout, stderr, dist / f"{portname}.zip"


# ── Property 8: Zip contents correctness ─────────────────────────────────────
# Feature: github-actions-port-builder, Property 8: Zip contents correctness

@given(optional_set_st)
@settings(max_examples=200, suppress_health_check=[HealthCheck.too_slow])
def test_property8_zip_contents_correct(optional_files, tmp_path):
    """Mandatory files always present; optional files iff in source; no forbidden patterns."""
    portname = "ziptest"
    rc, stdout, stderr, zip_path = run_assemble(portname, tmp_path, optional_files)
    assert rc == 0, f"assemble-zip failed.\nstdout: {stdout}\nstderr: {stderr}"
    assert zip_path.exists()

    with zipfile.ZipFile(zip_path) as zf:
        names = set(zf.namelist())

    # Mandatory files must always be present
    assert f"{portname}/{portname}.aarch64" in names
    assert f"{portname}/README.md" in names
    assert f"{portname}/screenshot.png" in names
    assert f"{portname}/port.json" in names
    assert any(n.endswith(".sh") and "/" not in n for n in names), "Launch .sh missing from top level"

    # Optional files: present iff they were in source
    if "cover.png" in optional_files:
        assert f"{portname}/cover.png" in names
    if "gameinfo.xml" in optional_files:
        assert f"{portname}/gameinfo.xml" in names
    if "alsoft.conf" in optional_files:
        assert f"{portname}/alsoft.conf" in names
    if GPTK_FILE in optional_files:
        assert f"{portname}/{GPTK_FILE}" in names

    # Forbidden: no source files
    for n in names:
        assert not n.endswith(".c"), f"Source file in zip: {n}"
        assert not n.endswith(".h"), f"Header file in zip: {n}"


# ── Property 9: port.json items consistency ───────────────────────────────────
# Feature: github-actions-port-builder, Property 9: port.json items consistency with zip contents

@given(optional_set_st)
@settings(max_examples=200, suppress_health_check=[HealthCheck.too_slow])
def test_property9_items_match_zip_manifest(optional_files, tmp_path):
    """port.json .items must exactly match the set of files in the zip."""
    portname = "itemstest"
    rc, stdout, stderr, zip_path = run_assemble(portname, tmp_path, optional_files)
    assert rc == 0, f"assemble-zip failed.\nstdout: {stdout}\nstderr: {stderr}"

    with zipfile.ZipFile(zip_path) as zf:
        actual_files = {n for n in zf.namelist() if not n.endswith("/")}
        port_json = json.loads(zf.read(f"{portname}/port.json"))

    items_set = set(port_json["items"])
    assert items_set == actual_files, (
        f"items mismatch.\nport.json items: {sorted(items_set)}\n"
        f"actual files:    {sorted(actual_files)}"
    )
    assert any("aarch64" in i for i in items_set), "No aarch64 binary in items"


# ── Property 10: port.json arch always aarch64 ────────────────────────────────
# Feature: github-actions-port-builder, Property 10: port.json arch field is always aarch64

@given(arch_st)
@settings(max_examples=100, suppress_health_check=[HealthCheck.too_slow])
def test_property10_arch_always_aarch64(input_arch, tmp_path):
    """Regardless of original arch value, assembled port.json must have arch=['aarch64']."""
    portname = "archtest"
    rc, stdout, stderr, zip_path = run_assemble(portname, tmp_path, arch=input_arch)
    assert rc == 0, f"assemble-zip failed.\nstdout: {stdout}\nstderr: {stderr}"

    with zipfile.ZipFile(zip_path) as zf:
        port_json = json.loads(zf.read(f"{portname}/port.json"))

    assert port_json["attr"]["arch"] == ["aarch64"], (
        f"Expected arch=['aarch64'], got {port_json['attr']['arch']}"
    )


# ── Property 11: Zip filename matches port.json .name ─────────────────────────
# Feature: github-actions-port-builder, Property 11: Zip filename matches port.json name field

@given(st.text(alphabet="abcdefghijklmnopqrstuvwxyz0123456789_-", min_size=2, max_size=15))
@settings(max_examples=100, suppress_health_check=[HealthCheck.too_slow])
def test_property11_zip_filename_matches_portjson_name(portname, tmp_path):
    """The produced zip filename must equal the .name field in port.json."""
    rc, stdout, stderr, zip_path = run_assemble(portname, tmp_path)
    assert rc == 0, f"assemble-zip failed.\nstdout: {stdout}\nstderr: {stderr}"
    assert zip_path.exists(), f"Expected zip at {zip_path}"
    assert zip_path.name == f"{portname}.zip"
