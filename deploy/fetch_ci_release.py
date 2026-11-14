#!/usr/bin/env python3
"""Download and verify a KylinList update bundle produced by GitHub Actions.

Usage:

    python -I deploy/fetch_ci_release.py <RUN_ID> <NEW_OUTPUT_DIR>

RUN_ID is the GitHub Actions run id of a "Build and package" workflow run
(hualiyexi/kylinnote, .github/workflows/build.yml) that uploaded the
kylinlist-update-bundle artifact. NEW_OUTPUT_DIR must not exist yet; on
success the verified release directory (updates/ + RELEASE-INFO.json) is
extracted there. Existing output directories are never overwritten, and on
any failure every directory created by this run is removed again.

Only the Python standard library and the locally installed gh CLI are used:

  1. gh api repos/hualiyexi/kylinnote/actions/runs/<RUN_ID> must report
     path=.github/workflows/build.yml, status=completed,
     conclusion=success, and a head_sha.
  2. gh run download <RUN_ID> -R hualiyexi/kylinnote
     -n kylinlist-update-bundle -D <fresh temporary directory> downloads
     the artifact. The download directory is independent of both this
     script's directory and the output directory.
  3. The single inner zip inside the download is located and extracted
     safely into the new empty output directory: members with absolute
     paths, drive letters, backslashes or ".." segments, and symlink or
     other special (device/socket/fifo) members are rejected.
  4. deploy/ci_release.py validate_ci_release() validates the extracted
     RELEASE-INFO.json (exactly the six release targets, partial=false,
     self-declared ci metadata consistent with this repository), and the
     ci.runId / ci.headSha recorded in the bundle must match the GitHub
     API answer for the requested run. That API cross-check is what gives
     the self-declared ci fields meaning; nothing here is a signature.

Nothing downloaded is ever executed or imported; the download directory
only ever holds untrusted bytes and is deleted at the end.
"""

import importlib.util
import json
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path, PurePosixPath

REPOSITORY = "hualiyexi/kylinnote"
EXPECTED_WORKFLOW_PATH = ".github/workflows/build.yml"
ARTIFACT_NAME = "kylinlist-update-bundle"

USAGE = "usage: python -I deploy/fetch_ci_release.py <RUN_ID> <NEW_OUTPUT_DIR>"

# python -I does not put this script's directory on sys.path, so load the
# sibling validation module explicitly. It is our own file in the same
# trusted directory as this script, not downloaded content.
_DEPLOY_DIR = Path(__file__).resolve().parent
_CI_RELEASE_SPEC = importlib.util.spec_from_file_location(
    "ci_release", _DEPLOY_DIR / "ci_release.py")
ci_release = importlib.util.module_from_spec(_CI_RELEASE_SPEC)
_CI_RELEASE_SPEC.loader.exec_module(ci_release)


class FetchError(Exception):
    """A controlled failure that must abort the fetch and trigger cleanup."""


def _run_gh(arguments, gh):
    command = list(gh) + list(arguments)
    try:
        result = subprocess.run(command, capture_output=True)
    except FileNotFoundError as error:
        raise FetchError(
            "the gh CLI is not installed or not on PATH "
            "(tried: {0})".format(command[0])) from error
    except OSError as error:
        raise FetchError("cannot run {0}: {1}".format(command[0], error)) from error
    if result.returncode != 0:
        detail = (result.stderr or result.stdout or b"").decode("utf-8", "replace").strip()
        raise FetchError(
            "{0} failed with exit code {1}: {2}".format(
                command[0], result.returncode, detail or "no output"))
    return result.stdout


def fetch_run_metadata(run_id, gh=("gh",)):
    """Return the GitHub Actions run object for run_id via gh api."""
    endpoint = "repos/{0}/actions/runs/{1}".format(REPOSITORY, run_id)
    output = _run_gh(["api", endpoint], gh)
    try:
        data = json.loads(output.decode("utf-8", errors="replace"))
    except ValueError as error:
        raise FetchError("gh api {0} did not return JSON: {1}".format(
            endpoint, error)) from error
    if not isinstance(data, dict):
        raise FetchError("gh api {0} did not return a JSON object".format(endpoint))
    return data


def verify_run_metadata(data, run_id):
    """Check the workflow path, run state and head_sha; return head_sha."""
    if data.get("path") != EXPECTED_WORKFLOW_PATH:
        raise FetchError(
            "run {0} belongs to workflow path {1!r}, expected {2!r}".format(
                run_id, data.get("path"), EXPECTED_WORKFLOW_PATH))
    if str(data.get("id") or "") != str(run_id):
        raise FetchError(
            "gh api answered for run id {0!r}, requested {1!r}".format(
                data.get("id"), run_id))
    if data.get("status") != "completed":
        raise FetchError(
            "run {0} status is {1!r}, expected 'completed'".format(
                run_id, data.get("status")))
    if data.get("conclusion") != "success":
        raise FetchError(
            "run {0} conclusion is {1!r}, expected 'success'".format(
                run_id, data.get("conclusion")))
    if data.get("event") not in ("push", "workflow_dispatch"):
        raise FetchError(
            "run {0} event is {1!r}; only push/workflow_dispatch runs are allowed"
            .format(run_id, data.get("event")))
    head_repository = data.get("head_repository")
    if not isinstance(head_repository, dict) or \
            head_repository.get("full_name") != REPOSITORY:
        raise FetchError(
            "run {0} does not originate from {1}".format(run_id, REPOSITORY))
    head_sha = data.get("head_sha")
    if not isinstance(head_sha, str) or not ci_release.GIT_SHA_RE.fullmatch(head_sha):
        raise FetchError(
            "run {0} has no valid head_sha in the GitHub API answer: "
            "{1!r}".format(run_id, head_sha))
    return head_sha


def download_artifact(run_id, destination, gh=("gh",)):
    """Download the kylinlist-update-bundle artifact into destination."""
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    _run_gh(
        ["run", "download", run_id,
         "-R", REPOSITORY,
         "-n", ARTIFACT_NAME,
         "-D", str(destination)],
        gh)


def locate_single_zip(directory):
    """Return the unique *.zip file inside a downloaded artifact directory."""
    zips = sorted(path for path in Path(directory).rglob("*.zip") if path.is_file())
    if len(zips) != 1:
        names = ", ".join(str(path.name) for path in zips) or "none"
        raise FetchError(
            "expected exactly one inner bundle zip in the downloaded "
            "artifact, found {0} ({1})".format(len(zips), names))
    return zips[0]


_UNIX_SPECIAL_TYPES = {
    0o140000: "socket",
    0o120000: "symbolic link",
    0o060000: "block device",
    0o020000: "character device",
    0o010000: "fifo",
}


def _check_member_name(name):
    """Validate one zip member name; return its PurePosixPath parts."""
    if not name:
        raise FetchError("bundle zip contains a member with an empty name")
    if "\\" in name:
        raise FetchError(
            "bundle zip member {0!r} uses a backslash path separator".format(name))
    pure = PurePosixPath(name)
    if pure.is_absolute() or name.startswith("/"):
        raise FetchError(
            "bundle zip member {0!r} has an absolute path".format(name))
    parts = pure.parts
    if not parts:
        raise FetchError("bundle zip member {0!r} has no path segments".format(name))
    first = parts[0]
    if len(first) > 1 and first[1] == ":":
        raise FetchError(
            "bundle zip member {0!r} has a Windows drive path".format(name))
    if ".." in parts:
        raise FetchError(
            "bundle zip member {0!r} escapes the output directory with "
            "'..'".format(name))
    return parts


def safe_extract_zip(zip_path, output_dir):
    """Extract an untrusted zip into output_dir, rejecting abnormal members.

    Rejected: absolute paths, drive letters, backslashes, ".." segments,
    symlinks, sockets, devices and fifos. Regular files are written without
    preserving any executable bit, and nothing from the archive is executed.
    """
    output_dir = Path(output_dir)
    with zipfile.ZipFile(str(zip_path)) as archive:
        for info in archive.infolist():
            parts = _check_member_name(info.filename)
            name = info.filename
            mode = (info.external_attr >> 16) & 0xFFFF
            file_type = mode & 0xF000
            if file_type in _UNIX_SPECIAL_TYPES:
                raise FetchError(
                    "bundle zip member {0!r} is a {1}, not a regular "
                    "file".format(name, _UNIX_SPECIAL_TYPES[file_type]))
            is_directory = name.endswith("/") or file_type == 0o040000
            if not is_directory and file_type not in (0, 0o100000):
                raise FetchError(
                    "bundle zip member {0!r} has an unsupported unix file "
                    "type 0o{1:o}".format(name, file_type))
            target = output_dir.joinpath(*parts)
            if is_directory:
                target.mkdir(parents=True, exist_ok=True)
                continue
            if target.exists():
                raise FetchError(
                    "bundle zip declares member {0!r} twice or clashes with "
                    "a directory".format(name))
            target.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(info) as source, open(target, "wb") as sink:
                shutil.copyfileobj(source, sink)


def create_output_dir(raw_output):
    """Create the (not yet existing) output directory; track what was made.

    Returns (output, created) where created lists the directories this call
    created, outermost first, so a failure can remove exactly those.
    """
    output = Path(raw_output)
    if output.exists() or output.is_symlink():
        raise FetchError(
            "output directory already exists, refusing to overwrite: "
            "{0}".format(output))
    missing = []
    probe = output
    while not probe.exists():
        missing.append(probe)
        parent = probe.parent
        if parent == probe:
            break
        probe = parent
    created = []
    try:
        for path in reversed(missing):
            path.mkdir()
            created.append(path)
    except Exception:
        _cleanup_created(output, created)
        raise
    return output, created


def _cleanup_created(output, created):
    """Remove the directories this run created, nothing else."""
    shutil.rmtree(output, ignore_errors=True)
    for path in reversed(created):
        if path == output:
            continue
        try:
            path.rmdir()
        except OSError:
            break


def execute(run_id, raw_output, gh=("gh",)):
    """Fetch, extract and validate; return 0 on success."""
    metadata = fetch_run_metadata(run_id, gh)
    head_sha = verify_run_metadata(metadata, run_id)

    temp_root = Path(tempfile.mkdtemp(prefix="kylinlist-fetch-"))
    output = None
    created = []
    try:
        download_artifact(run_id, temp_root, gh)
        bundle_zip = locate_single_zip(temp_root)
        output, created = create_output_dir(raw_output)
        safe_extract_zip(bundle_zip, output)

        version, info = ci_release.validate_ci_release(output)
        ci = info["ci"]
        if ci["runId"] != run_id:
            raise FetchError(
                "RELEASE-INFO.json ci.runId {0} does not match the requested "
                "run {1}".format(ci["runId"], run_id))
        if ci["headSha"] != head_sha:
            raise FetchError(
                "RELEASE-INFO.json ci.headSha {0} does not match the GitHub "
                "API head_sha {1} for run {2}".format(
                    ci["headSha"], head_sha, run_id))

        print("==> {0}".format(output))
        print("    KylinList {0} from run {1} ({2})".format(
            version, run_id, ci["runUrl"]))
        print("    {0} artifact targets, partial={1}".format(
            len(info["artifacts"]), info["partial"]))
        return 0
    except Exception:
        if output is not None:
            _cleanup_created(output, created)
        raise
    finally:
        shutil.rmtree(temp_root, ignore_errors=True)


def main(argv=None, gh=("gh",)):
    if argv is None:
        argv = sys.argv[1:]
    if len(argv) != 2:
        print(USAGE, file=sys.stderr)
        return 2
    run_id, raw_output = argv
    if not ci_release.RUN_ID_RE.fullmatch(run_id):
        print("ERROR: RUN_ID must be a plain decimal GitHub Actions run id: "
              "{0!r}".format(run_id), file=sys.stderr)
        return 2
    try:
        return execute(run_id, raw_output, gh)
    except (FetchError, ValueError) as error:
        print("ERROR: {0}".format(error), file=sys.stderr)
        return 1
    except (OSError, zipfile.BadZipFile) as error:
        print("ERROR: {0}: {1}".format(type(error).__name__, error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
