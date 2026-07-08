"""
ci/tests/test_workflow_yaml_pbt.py — Property 13: all workflow action refs are pinned

Feature: github-actions-port-builder
Property 13: All action references are pinned to immutable tags (validates Requirements 8.2)

Run with:  pytest ci/tests/test_workflow_yaml_pbt.py -v
Requires:  pip install -r ci/tests/requirements.txt
"""
import re
from pathlib import Path

import yaml
import pytest

WORKFLOWS = [
    Path(__file__).parent.parent.parent / ".github" / "workflows" / "build-ports.yml",
    Path(__file__).parent.parent.parent / ".github" / "workflows" / "ci-selftest.yml",
]

PINNED_PATTERN = re.compile(r"^[a-zA-Z0-9_-]+/[a-zA-Z0-9_.-]+@v[0-9]")
FORBIDDEN_REFS = ["@main", "@master", "@latest"]


def collect_uses(obj):
    """Recursively collect all 'uses:' values from a parsed YAML object."""
    if isinstance(obj, dict):
        if "uses" in obj:
            yield obj["uses"]
        for v in obj.values():
            yield from collect_uses(v)
    elif isinstance(obj, list):
        for item in obj:
            yield from collect_uses(item)


@pytest.mark.parametrize("workflow_path", WORKFLOWS)
def test_property13_all_actions_pinned(workflow_path):
    """
    Feature: github-actions-port-builder, Property 13: All action references are pinned to immutable tags
    Every 'uses:' reference in the workflow YAML must be pinned to an immutable
    version tag (e.g. @v4). Floating refs like @main, @master, @latest are forbidden.
    """
    assert workflow_path.exists(), f"Workflow file not found: {workflow_path}"
    with open(workflow_path) as f:
        doc = yaml.safe_load(f)

    uses_refs = list(collect_uses(doc))
    assert uses_refs, f"No 'uses:' references found in {workflow_path.name}"

    unpinned = []
    for ref in uses_refs:
        if not PINNED_PATTERN.match(ref):
            unpinned.append(ref)
        for bad in FORBIDDEN_REFS:
            if bad in ref:
                unpinned.append(f"{ref} (contains '{bad}')")

    assert not unpinned, (
        f"Unpinned action references in {workflow_path.name}:\n"
        + "\n".join(f"  - {r}" for r in unpinned)
    )
