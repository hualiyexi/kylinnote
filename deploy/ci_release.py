#!/usr/bin/env python3
"""Reusable validation interface for a CI-produced KylinList release directory.

    validate_ci_release(release_dir) -> (version, info)

release_dir is an extracted kylinlist-update-bundle directory (produced by
packaging/generate-update-manifest.py inside the GitHub Actions "Build and
package" workflow). On success the parsed RELEASE-INFO.json dict is returned
together with the release version; every failure raises ValueError with a
message that explains the rejected check.

Checked requirements:
  * RELEASE-INFO.json exists and parses as a JSON object;
  * partial is exactly false (local dry-run bundles are never publishable);
  * gitSha is a valid 40/64 hex SHA and equals ci.headSha;
  * ci.repository is hualiyexi/kylinnote and ci.workflowPath is
    .github/workflows/build.yml;
  * ci.runId is a plain decimal string and ci.runUrl is exactly
    https://github.com/hualiyexi/kylinnote/actions/runs/<runId>;
  * the artifacts list covers exactly the six release targets, with no
    missing, duplicate or unknown target;
  * updates/v1/stable/manifest.json exists and its version equals the
    RELEASE-INFO.json version.

Honesty about scope: everything in the ci object is self-declared by the CI
job that wrote RELEASE-INFO.json. These checks prove the metadata is
internally consistent and points at the expected repository, workflow and
run id; they are NOT a cryptographic signature and they cannot prove the
bundle really came from that run. The external binding must come from
cross-checking the GitHub API (deploy/fetch_ci_release.py does that before
calling this module) or from the human reviewing the run page.
File bytes inside the release directory are not re-hashed here; package
integrity is enforced later by deploy/package.sh and the publisher's own
validate step.
"""

import json
import re
from pathlib import Path

EXPECTED_REPOSITORY = "hualiyexi/kylinnote"
EXPECTED_WORKFLOW_PATH = ".github/workflows/build.yml"
MANIFEST_RELPATH = "updates/v1/stable/manifest.json"

EXPECTED_TARGETS = (
    "linux-arm64-system-qt",
    "linux-arm64-bundled-qt",
    "linux-amd64-system-qt",
    "linux-amd64-bundled-qt",
    "windows-amd64-setup",
    "windows-amd64-portable",
)

GIT_SHA_RE = re.compile(r"[0-9a-f]{40}|[0-9a-f]{64}")
RUN_ID_RE = re.compile(r"[1-9][0-9]*")

CI_REQUIRED_FIELDS = (
    "repository",
    "workflow",
    "workflowPath",
    "runId",
    "runUrl",
    "ref",
    "eventName",
    "headSha",
)


def _is_strict_version(value):
    """True for exactly three numeric components without leading zeros."""
    if not isinstance(value, str):
        return False
    parts = value.split(".")
    if len(parts) != 3:
        return False
    return all(
        part.isdigit() and not (len(part) > 1 and part.startswith("0"))
        for part in parts)


def _expected_run_url(run_id):
    return "https://github.com/{0}/actions/runs/{1}".format(EXPECTED_REPOSITORY, run_id)


def _load_json(path, description):
    try:
        data = json.loads(Path(path).read_text(encoding="utf-8"))
    except OSError as error:
        raise ValueError("cannot read {0}: {1}".format(description, error)) from error
    except ValueError as error:
        raise ValueError("cannot parse {0}: {1}".format(description, error)) from error
    return data


def _validate_targets(artifacts):
    if not isinstance(artifacts, list):
        raise ValueError(
            "RELEASE-INFO.json 'artifacts' must be a list, got {0}".format(
                type(artifacts).__name__))
    targets = []
    for index, entry in enumerate(artifacts):
        if not isinstance(entry, dict):
            raise ValueError("RELEASE-INFO.json artifacts[{0}] is not an object".format(index))
        target = entry.get("target")
        if not isinstance(target, str) or not target:
            raise ValueError(
                "RELEASE-INFO.json artifacts[{0}] has no string 'target'".format(index))
        targets.append(target)
    duplicates = sorted({name for name in targets if targets.count(name) > 1})
    if duplicates:
        raise ValueError(
            "RELEASE-INFO.json has duplicate artifact targets: {0}".format(
                ", ".join(duplicates)))
    missing = [name for name in EXPECTED_TARGETS if name not in targets]
    if missing:
        raise ValueError("RELEASE-INFO.json is missing artifact targets: {0}".format(
            ", ".join(missing)))
    unknown = sorted(set(targets) - set(EXPECTED_TARGETS))
    if unknown:
        raise ValueError(
            "RELEASE-INFO.json has unexpected artifact targets: {0}".format(
                ", ".join(unknown)))


def validate_ci_release(release_dir):
    """Validate an extracted CI update bundle; return (version, info).

    Raises ValueError on every failure listed in the module docstring.
    """
    release_dir = Path(release_dir)
    info_path = release_dir / "RELEASE-INFO.json"
    if not info_path.is_file():
        raise ValueError("missing RELEASE-INFO.json in {0}".format(release_dir))
    info = _load_json(info_path, "RELEASE-INFO.json")
    if not isinstance(info, dict):
        raise ValueError("RELEASE-INFO.json is not a JSON object")

    version = info.get("version")
    if not _is_strict_version(version):
        raise ValueError(
            "RELEASE-INFO.json 'version' is not a strict X.Y.Z version: "
            "{0!r}".format(version))

    if info.get("partial") is not False:
        raise ValueError(
            "RELEASE-INFO.json 'partial' must be exactly false, got "
            "{0!r}; partial bundles must never be published".format(
                info.get("partial")))

    git_sha = info.get("gitSha")
    if not isinstance(git_sha, str) or not GIT_SHA_RE.fullmatch(git_sha):
        raise ValueError(
            "RELEASE-INFO.json 'gitSha' is not a 40/64 character lowercase "
            "hex SHA: {0!r}".format(git_sha))

    ci = info.get("ci")
    if not isinstance(ci, dict):
        raise ValueError(
            "RELEASE-INFO.json is missing the 'ci' object; rebuild the "
            "release in GitHub Actions")
    for field in CI_REQUIRED_FIELDS:
        value = ci.get(field)
        if not isinstance(value, str) or not value:
            raise ValueError(
                "RELEASE-INFO.json ci.{0} must be a non-empty string, got "
                "{1!r}".format(field, value))

    if ci["repository"] != EXPECTED_REPOSITORY:
        raise ValueError(
            "RELEASE-INFO.json ci.repository is {0!r}, expected {1!r}".format(
                ci["repository"], EXPECTED_REPOSITORY))
    if ci["workflowPath"] != EXPECTED_WORKFLOW_PATH:
        raise ValueError(
            "RELEASE-INFO.json ci.workflowPath is {0!r}, expected {1!r}".format(
                ci["workflowPath"], EXPECTED_WORKFLOW_PATH))
    if ci["eventName"] not in ("push", "workflow_dispatch"):
        raise ValueError(
            "RELEASE-INFO.json ci.eventName is {0!r}; pull requests are not "
            "publishable".format(ci["eventName"]))
    if not RUN_ID_RE.fullmatch(ci["runId"]):
        raise ValueError(
            "RELEASE-INFO.json ci.runId is not a plain decimal run id: "
            "{0!r}".format(ci["runId"]))
    if ci["runUrl"] != _expected_run_url(ci["runId"]):
        raise ValueError(
            "RELEASE-INFO.json ci.runUrl {0!r} does not exactly match the "
            "expected {1!r}".format(ci["runUrl"], _expected_run_url(ci["runId"])))
    if git_sha != ci["headSha"]:
        raise ValueError(
            "RELEASE-INFO.json gitSha {0} does not equal ci.headSha {1}".format(
                git_sha, ci["headSha"]))

    manifest_path = release_dir / MANIFEST_RELPATH
    if not manifest_path.is_file():
        raise ValueError("missing {0} in {1}".format(MANIFEST_RELPATH, release_dir))
    manifest = _load_json(manifest_path, MANIFEST_RELPATH)
    if not isinstance(manifest, dict):
        raise ValueError("{0} is not a JSON object".format(MANIFEST_RELPATH))
    manifest_version = manifest.get("version")
    if manifest_version != version:
        raise ValueError(
            "{0} version {1!r} does not equal RELEASE-INFO.json version "
            "{2!r}".format(MANIFEST_RELPATH, manifest_version, version))

    _validate_targets(info.get("artifacts"))

    return version, info


if __name__ == "__main__":
    import sys

    if len(sys.argv) != 2:
        print("usage: python -I deploy/ci_release.py <release-dir>", file=sys.stderr)
        sys.exit(2)
    try:
        release_version, _ = validate_ci_release(sys.argv[1])
    except ValueError as error:
        print("ERROR: {0}".format(error), file=sys.stderr)
        sys.exit(1)
    print("OK: KylinList {0} CI release metadata".format(release_version))
