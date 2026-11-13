#!/usr/bin/env python3
"""Generate the stable-channel update manifest from CI build artifacts.

Run with the standard library only (python -I):

    python -I packaging/generate-update-manifest.py --dist <dir> --version <X.Y.Z> \
        [--notes-file <file>] [--git-sha <sha>] --output <release-dir> [--allow-partial linux]

The CMakeLists.txt project version is the single version source. --version must
be a strict X.Y.Z release version (no leading-zero components, no prefix, no
suffix) and must equal the CMake project version. Local experiment packages
(suffix or different versions) never reach this generator in CI; if they show
up here, the generator rejects them, which keeps them out of the stable update
channel.

Output layout (inside --output):

    updates/v1/stable/manifest.json
    updates/files/<version>/<platform>-<arch>-<variant>/<original file name>
    RELEASE-INFO.json

manifest.json "path" values are relative to <server-base>/updates/. The Qt
client downloads <server-base>/updates/<path> and verifies sha256.
"""

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path

SCHEMA_VERSION = 1
CHANNEL = "stable"
PRODUCT = "kylinlist"
UPDATE_SCHEMA_DIR = "v1"

# (deb architecture, install-info variant, file-name suffix)
LINUX_LAYOUT = (
    ("arm64", "system-qt", ""),
    ("arm64", "bundled-qt", "_bundle"),
    ("amd64", "system-qt", ""),
    ("amd64", "bundled-qt", "_bundle"),
)

WINDOWS_SIDECAR_NAME = "windows-update-artifacts.json"
WINDOWS_KIND_VARIANTS = ("setup", "portable")

INSTALL_INFO_LOOKUP_PATHS = (
    "usr/share/kylinlist/install-info.json",
    "usr/local/share/kylinlist/install-info.json",
)

PROJECT_VERSION_RE = re.compile(r"(?m)^\s*VERSION\s+(\S+)\s*$")
GIT_SHA_RE = re.compile(r"[0-9a-f]{40}|[0-9a-f]{64}")


class UpdateError(Exception):
    """A validation failure that must abort manifest generation."""


def is_strict_version(value):
    """True for exactly three numeric components without leading zeros."""
    if not isinstance(value, str):
        return False
    parts = value.split(".")
    if len(parts) != 3:
        return False
    for part in parts:
        if not part.isdigit():
            return False
        if len(part) > 1 and part.startswith("0"):
            return False
    return True


def parse_project_version(cmake_text):
    """Return the first strict X.Y.Z project() VERSION, or None."""
    for match in PROJECT_VERSION_RE.finditer(cmake_text):
        candidate = match.group(1)
        if is_strict_version(candidate):
            return candidate
    return None


def linux_deb_filename(version, arch, suffix):
    return "kylinlist_{0}_{1}{2}.deb".format(version, arch, suffix)


def windows_expected_files(version):
    return {
        "setup": "KylinList-{0}-Windows-x64-Setup.exe".format(version),
        "portable": "KylinList-{0}-Windows-x64.zip".format(version),
    }


def target_directory(platform, arch, variant):
    return "{0}-{1}-{2}".format(platform, arch, variant)


def artifact_relative_path(version, platform, arch, variant, filename):
    return "files/{0}/{1}/{2}".format(
        version, target_directory(platform, arch, variant), filename)


def file_sha256_and_size(path):
    digest = hashlib.sha256()
    size = 0
    with open(path, "rb") as handle:
        while True:
            chunk = handle.read(1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
            size += len(chunk)
    return digest.hexdigest(), size


def utc_timestamp(moment=None):
    return (moment or datetime.now(timezone.utc)).strftime("%Y-%m-%dT%H:%M:%SZ")


def build_manifest(version, notes, published_at, artifacts):
    return {
        "schemaVersion": SCHEMA_VERSION,
        "channel": CHANNEL,
        "version": version,
        "publishedAt": published_at,
        "notes": notes,
        "artifacts": [
            {
                "platform": entry["platform"],
                "arch": entry["arch"],
                "variant": entry["variant"],
                "packageVersion": entry["packageVersion"],
                "path": entry["path"],
                "size": entry["size"],
                "sha256": entry["sha256"],
            }
            for entry in artifacts
        ],
    }


def expected_dist_names(version, include_windows):
    names = [linux_deb_filename(version, arch, suffix) for arch, _, suffix in LINUX_LAYOUT]
    if include_windows:
        names.append(WINDOWS_SIDECAR_NAME)
        names.extend(windows_expected_files(version).values())
    return names


def require_dist_files(dist_dir, names):
    missing = [name for name in names if not (dist_dir / name).is_file()]
    if missing:
        raise UpdateError(
            "missing expected artifacts in {0}: {1}".format(dist_dir, ", ".join(missing)))


def run_tool(argv):
    try:
        result = subprocess.run(argv, capture_output=True, text=True, check=True)
    except FileNotFoundError as error:
        raise UpdateError("required tool is not installed: {0}".format(argv[0])) from error
    except subprocess.CalledProcessError as error:
        detail = (error.stderr or error.stdout or "").strip()
        raise UpdateError("{0} failed: {1}".format(argv[0], detail)) from error
    return result.stdout.strip()


def read_deb_field(deb_path, field):
    return run_tool(["dpkg-deb", "-f", str(deb_path), field])


def validate_install_info(info, source, platform, arch, variant, version):
    """Check an install-info.json object against the expected identity."""
    expected_keys = ("product", "platform", "version", "arch", "variant")
    if not isinstance(info, dict):
        raise UpdateError("{0}: install-info.json is not a JSON object".format(source))
    for key in expected_keys:
        if key not in info:
            raise UpdateError("{0}: install-info.json is missing '{1}'".format(source, key))
    expected = {
        "product": PRODUCT,
        "platform": platform,
        "version": version,
        "arch": arch,
        "variant": variant,
    }
    for key, want in expected.items():
        got = info[key]
        if got != want:
            raise UpdateError(
                "{0}: install-info.json '{1}' is {2!r}, expected {3!r}".format(
                    source, key, got, want))


def load_install_info(source, extract_root):
    for relative in INSTALL_INFO_LOOKUP_PATHS:
        path = extract_root / relative
        if path.is_file():
            try:
                return json.loads(path.read_text(encoding="utf-8"))
            except (OSError, ValueError) as error:
                raise UpdateError(
                    "{0}: cannot parse {1}: {2}".format(source, relative, error)) from error
    raise UpdateError(
        "{0}: no /usr[/local]/share/kylinlist/install-info.json inside the package".format(
            source))


def inspect_deb(dist_dir, filename, version, arch, variant, work_dir):
    deb_path = dist_dir / filename
    package = read_deb_field(deb_path, "Package")
    if package != PRODUCT:
        raise UpdateError(
            "{0}: dpkg Package is {1!r}, expected {2!r}".format(filename, package, PRODUCT))
    deb_version = read_deb_field(deb_path, "Version")
    if deb_version != version:
        raise UpdateError(
            "{0}: dpkg Version is {1!r}, expected {2!r}".format(filename, deb_version, version))
    deb_arch = read_deb_field(deb_path, "Architecture")
    if deb_arch != arch:
        raise UpdateError(
            "{0}: dpkg Architecture is {1!r}, expected {2!r}".format(filename, deb_arch, arch))
    extract_root = work_dir / (filename + ".extract")
    extract_root.mkdir(parents=True, exist_ok=True)
    run_tool(["dpkg-deb", "-x", str(deb_path), str(extract_root)])
    info = load_install_info(filename, extract_root)
    validate_install_info(info, filename, "linux", arch, variant, version)
    sha256, size = file_sha256_and_size(deb_path)
    return {
        "platform": "linux",
        "arch": arch,
        "variant": variant,
        "packageVersion": deb_version,
        "filename": filename,
        "sha256": sha256,
        "size": size,
    }


def inspect_windows(dist_dir, version):
    sidecar_path = dist_dir / WINDOWS_SIDECAR_NAME
    try:
        sidecar = json.loads(sidecar_path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise UpdateError(
            "cannot read {0}: {1}".format(WINDOWS_SIDECAR_NAME, error)) from error
    if not isinstance(sidecar, dict) or sidecar.get("version") != version:
        got = sidecar.get("version") if isinstance(sidecar, dict) else None
        raise UpdateError(
            "{0}: version {1!r} does not match --version {2!r}".format(
                WINDOWS_SIDECAR_NAME, got, version))
    entries = sidecar.get("artifacts")
    if not isinstance(entries, list) or not entries:
        raise UpdateError("{0}: 'artifacts' is missing or empty".format(WINDOWS_SIDECAR_NAME))
    expected = windows_expected_files(version)
    seen = {}
    for entry in entries:
        kind = entry.get("kind") if isinstance(entry, dict) else None
        if kind not in WINDOWS_KIND_VARIANTS:
            raise UpdateError(
                "{0}: unknown artifact kind {1!r}".format(WINDOWS_SIDECAR_NAME, kind))
        if kind in seen:
            raise UpdateError(
                "{0}: duplicate artifact kind {1!r}".format(WINDOWS_SIDECAR_NAME, kind))
        seen[kind] = entry.get("file")
    for kind in WINDOWS_KIND_VARIANTS:
        want = expected[kind]
        got = seen.get(kind)
        if got != want:
            raise UpdateError(
                "{0}: {1} file is {2!r}, expected {3!r}".format(
                    WINDOWS_SIDECAR_NAME, kind, got, want))
    results = []
    for kind in WINDOWS_KIND_VARIANTS:
        filename = expected[kind]
        path = dist_dir / filename
        if not path.is_file():
            raise UpdateError("missing artifact: {0}".format(path))
        sha256, size = file_sha256_and_size(path)
        results.append({
            "platform": "windows",
            "arch": "amd64",
            "variant": kind,
            "packageVersion": version,
            "filename": filename,
            "sha256": sha256,
            "size": size,
        })
    return results


def collect_artifacts(dist_dir, version, work_dir, include_windows):
    artifacts = []
    for arch, variant, suffix in LINUX_LAYOUT:
        artifacts.append(
            inspect_deb(dist_dir, linux_deb_filename(version, arch, suffix),
                        version, arch, variant, work_dir))
    if include_windows:
        artifacts.extend(inspect_windows(dist_dir, version))
    return artifacts


def load_notes(notes_file, version):
    if not notes_file:
        return "KylinList {0}。".format(version)
    try:
        text = Path(notes_file).read_text(encoding="utf-8")
    except (OSError, ValueError) as error:
        raise UpdateError(
            "cannot read --notes-file {0}: {1}".format(notes_file, error)) from error
    text = text.rstrip("\r\n")
    if not text.strip():
        raise UpdateError("--notes-file is empty: {0}".format(notes_file))
    return text


def prepare_output(raw):
    output = Path(raw)
    if output.exists():
        if not output.is_dir():
            raise UpdateError("--output exists and is not a directory: {0}".format(output))
        leftovers = sorted(entry.name for entry in output.iterdir())
        if leftovers:
            raise UpdateError(
                "--output directory is not empty: {0} ({1})".format(output, ", ".join(leftovers)))
    else:
        output.mkdir(parents=True)
    return output


def write_release(dist_dir, output, version, git_sha, notes, artifacts, partial):
    """Copy artifacts into the release layout and write manifest + RELEASE-INFO."""
    published = utc_timestamp()
    manifest_artifacts = []
    info_artifacts = []
    for entry in artifacts:
        relative = artifact_relative_path(
            version, entry["platform"], entry["arch"], entry["variant"], entry["filename"])
        destination = output / "updates" / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(dist_dir / entry["filename"], destination)
        manifest_artifacts.append({
            "platform": entry["platform"],
            "arch": entry["arch"],
            "variant": entry["variant"],
            "packageVersion": entry["packageVersion"],
            "path": relative,
            "size": entry["size"],
            "sha256": entry["sha256"],
        })
        info_artifacts.append({
            "target": target_directory(entry["platform"], entry["arch"], entry["variant"]),
            "file": entry["filename"],
            "sha256": entry["sha256"],
            "size": entry["size"],
        })
    manifest = build_manifest(version, notes, published, manifest_artifacts)
    manifest_path = output / "updates" / UPDATE_SCHEMA_DIR / CHANNEL / "manifest.json"
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    release_info = {
        "version": version,
        "gitSha": git_sha,
        "generatedAt": published,
        "partial": partial,
        "artifacts": info_artifacts,
    }
    (output / "RELEASE-INFO.json").write_text(
        json.dumps(release_info, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return manifest, manifest_path


def parse_args(argv):
    parser = argparse.ArgumentParser(
        prog="generate-update-manifest.py",
        description="Validate CI artifacts and generate the stable update manifest.")
    parser.add_argument("--dist", required=True, help="directory containing the CI artifacts")
    parser.add_argument("--version", required=True, help="strict X.Y.Z release version")
    parser.add_argument("--notes-file", default=None,
                        help="plain-text release notes file (default: 'KylinList <version>。')")
    parser.add_argument("--git-sha", default="",
                        help="commit SHA recorded in RELEASE-INFO.json (CI passes GITHUB_SHA)")
    parser.add_argument("--output", required=True, help="release directory to create")
    parser.add_argument("--allow-partial", choices=["linux"], default=None,
                        help="local dry-run only: require just the four debs, skip Windows; "
                             "RELEASE-INFO.json is marked partial and must never be published")
    return parser.parse_args(argv)


def run(args):
    if not is_strict_version(args.version):
        raise UpdateError(
            "--version must be a strict X.Y.Z version without leading zeros: {0!r}".format(
                args.version))
    git_sha = args.git_sha.strip()
    if git_sha and not GIT_SHA_RE.fullmatch(git_sha):
        raise UpdateError("--git-sha must be a 40/64 character lowercase hex SHA: {0!r}".format(
            args.git_sha))
    project_root = Path(__file__).resolve().parents[1]
    try:
        cmake_text = (project_root / "CMakeLists.txt").read_text(encoding="utf-8")
    except OSError as error:
        raise UpdateError("cannot read CMakeLists.txt: {0}".format(error)) from error
    project_version = parse_project_version(cmake_text)
    if project_version is None:
        raise UpdateError("cannot find a strict X.Y.Z project VERSION in CMakeLists.txt")
    if project_version != args.version:
        raise UpdateError(
            "--version {0} does not equal the CMakeLists.txt project version {1}".format(
                args.version, project_version))
    dist_dir = Path(args.dist)
    if not dist_dir.is_dir():
        raise UpdateError("--dist is not a directory: {0}".format(dist_dir))
    include_windows = args.allow_partial != "linux"
    partial = not include_windows
    require_dist_files(dist_dir, expected_dist_names(args.version, include_windows))
    notes = load_notes(args.notes_file, args.version)
    with tempfile.TemporaryDirectory(prefix="kylinlist-manifest-") as work:
        artifacts = collect_artifacts(dist_dir, args.version, Path(work), include_windows)
    output = prepare_output(args.output)
    manifest, manifest_path = write_release(
        dist_dir, output, args.version, git_sha, notes, artifacts, partial)
    print("==> {0}".format(manifest_path))
    print("==> {0} ({1} artifacts, partial={2})".format(
        output / "RELEASE-INFO.json", len(artifacts), partial))
    for artifact in manifest["artifacts"]:
        print("    {0}/{1}/{2}: {3} ({4} bytes)".format(
            artifact["platform"], artifact["arch"], artifact["variant"],
            artifact["path"], artifact["size"]))
    return 0


def main(argv=None):
    args = parse_args(argv)
    try:
        return run(args)
    except UpdateError as error:
        print("ERROR: {0}".format(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
