#!/usr/bin/env python3
#
#  Copyright (c) 2024-2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
#
#  SPDX-License-Identifier: Apache-2.0
#
"""
LibOSDP release helper — a human-in-the-loop release flow in two shapes.

QUICK (the default) — one commit, for patch releases and anything else that
does not need a long cycle:

    1) prepare  — bump the version in all five files to their final released
                  state and scaffold CHANGELOG/RELEASE-v<next>.md with commit
                  hints. Staged, NOT committed. No pre-release marker.

    2) (edit the changelog: fold the ## Changes hints in and delete them)

    3) publish  — validate the changelog, then fold the version bump and the
                  changelog into a single "Release v<next>" commit and lay a
                  GPG-signed tag.

CYCLE (prepare --cycle) — two commits, for a release prepared long before it
ships. Identical, except that prepare also sets the pre-release marker and you
commit "Prepare v<next>"; master then reports e.g. 4.0.0-dev.37+ga1b2c3d for
the whole cycle, and publish clears the marker in the "Release v<next>" commit.

Either way the tag's signature must match https://github.com/sidcha.gpg, and
nothing is ever pushed — the push command is printed for you to run.

publish infers which shape it is finishing by comparing the worktree's version
against HEAD's: differing ⇒ the prepare was never committed ⇒ quick. So a
--cycle prepare you decide not to commit simply publishes as a quick release.

The committed marker (LIBOSDP_PRERELEASE in CMakeLists.txt) makes double-prepare
and double-publish impossible during a cycle, and tells the gates (check-staged,
check-changelog) which release file is still WIP and so exempt from the
finalized-changelog rules.

Re-releasing a failed publish:

    publish --re-release            re-cut the SAME version onto fix commits
                                    (force-moves the signed tag). CI idempotency
                                    absorbs already-published artifacts.
    publish --re-release --post N   also emit X.Y.Z.postN to PyPI and X.Y.Z.N to
                                    NuGet, because their version numbers are
                                    immutable and a broken package can never
                                    reuse X.Y.Z.

Examples:
    scripts/make_release.py prepare                 # patch bump, single commit
    scripts/make_release.py prepare --minor
    scripts/make_release.py prepare --cycle --set 5.0.0
    scripts/make_release.py publish
    scripts/make_release.py publish --re-release
    scripts/make_release.py publish --re-release --post 1
"""

import argparse
import dataclasses
import os
import re
import subprocess
import sys
import tempfile
import urllib.request
from datetime import UTC, datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import changelog_tool  # noqa: E402  (sibling script, reused for changelog logic)

DEFAULT_KEY_URL = "https://github.com/sidcha.gpg"
MARKER = "dev"  # the only pre-release marker value

# Version files this script owns. CHANGELOG/CHANGELOG.legacy is a historical log
# header, not a version source, and is deliberately excluded.
VERSION_FILES = [
    "CMakeLists.txt",
    "python/pyproject.toml",
    "library.json",
    "platformio/osdp_config.h",
    "dotnet/Directory.Build.props",
]


def die(message: str) -> None:
    print(f"error: {message}", file=sys.stderr)
    raise SystemExit(1)


@dataclasses.dataclass(frozen=True)
class Version:
    major: int
    minor: int
    patch: int

    def __str__(self) -> str:
        return f"{self.major}.{self.minor}.{self.patch}"

    def bumped(self, kind: str) -> "Version":
        if kind == "major":
            return Version(self.major + 1, 0, 0)
        if kind == "minor":
            return Version(self.major, self.minor + 1, 0)
        return Version(self.major, self.minor, self.patch + 1)


@dataclasses.dataclass(frozen=True)
class State:
    version: Version
    prerelease: bool
    post: int | None = None


def parse_version(raw: str) -> Version:
    match = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)", raw)
    if not match:
        die(f"Not a X.Y.Z version: {raw}")
    return Version(int(match.group(1)), int(match.group(2)), int(match.group(3)))


# ---------------------------------------------------------------------------
# git plumbing
# ---------------------------------------------------------------------------


def git(args: list[str], root: Path, check: bool = True) -> str:
    result = subprocess.run(
        ["git", *args], cwd=root, capture_output=True, text=True
    )
    if check and result.returncode != 0:
        die(f"git {' '.join(args)} failed: {result.stderr.strip()}")
    return result.stdout.strip()


def repo_root() -> Path:
    result = subprocess.run(
        ["git", "rev-parse", "--show-toplevel"], capture_output=True, text=True
    )
    if result.returncode != 0:
        die("not inside a git work tree")
    return Path(result.stdout.strip())


def tag_exists(root: Path, tag: str) -> bool:
    return bool(git(["tag", "-l", tag], root))


# ---------------------------------------------------------------------------
# per-file read/write of the version + marker (each in its native dialect)
# ---------------------------------------------------------------------------


def _sub_once(path: Path, pattern: str, repl: str, text: str) -> str:
    new, count = re.subn(pattern, repl, text, count=1, flags=re.M)
    if count != 1:
        die(f"{path.name}: expected exactly one match for /{pattern}/, found {count}")
    return new


def _pyproject_version_string(state: State) -> str:
    if state.prerelease:
        return f"{state.version}.dev0"
    if state.post is not None:
        return f"{state.version}.post{state.post}"
    return f"{state.version}"


def _semver_string(state: State) -> str:
    return f"{state.version}-{MARKER}" if state.prerelease else f"{state.version}"


def _nuget_version_string(state: State) -> str:
    if state.prerelease:
        return f"{state.version}-{MARKER}"
    if state.post is not None:
        return f"{state.version}.{state.post}"
    return f"{state.version}"


def read_state(root: Path) -> State:
    """CMakeLists.txt is the source of truth for the state."""
    text = (root / "CMakeLists.txt").read_text()
    ver = re.search(r"^project\(libosdp VERSION (\d+\.\d+\.\d+)\)$", text, re.M)
    marker = re.search(r'^set\(LIBOSDP_PRERELEASE "([^"]*)"\)$', text, re.M)
    if not ver or not marker:
        die("CMakeLists.txt: could not parse project() version / LIBOSDP_PRERELEASE")
    if marker.group(1) not in ("", MARKER):
        die(f"CMakeLists.txt: unexpected LIBOSDP_PRERELEASE {marker.group(1)!r}")
    return State(parse_version(ver.group(1)), prerelease=bool(marker.group(1)))


def head_state(root: Path) -> tuple[Version, bool]:
    """The (numeric, is_prerelease) state recorded in the last commit.

    Compared against the worktree, this is what distinguishes a quick release
    (bump still uncommitted) from a cycle whose Prepare commit already landed.
    """
    text = git(["show", "HEAD:CMakeLists.txt"], root, check=False)
    if not text:
        die("cannot read CMakeLists.txt at HEAD — not a libosdp checkout, or "
            "the branch has no commits yet")
    return parse_version_file("CMakeLists.txt", text)


def parse_version_file(name: str, text: str) -> tuple[Version, bool]:
    """Return (numeric, is_prerelease) parsed from a version file's text."""
    if name == "CMakeLists.txt":
        v = re.search(r"^project\(libosdp VERSION (\d+\.\d+\.\d+)\)$", text, re.M)
        m = re.search(r'^set\(LIBOSDP_PRERELEASE "([^"]*)"\)$', text, re.M)
        if not v or not m:
            die("CMakeLists.txt: could not parse version / LIBOSDP_PRERELEASE")
        return parse_version(v.group(1)), bool(m.group(1))
    if name == "python/pyproject.toml":
        v = re.search(r'^version = "(\d+\.\d+\.\d+)(\.dev\d+|\.post\d+)?"$', text, re.M)
        if not v:
            die("python/pyproject.toml: could not parse version")
        return parse_version(v.group(1)), (v.group(2) or "").startswith(".dev")
    if name == "library.json":
        v = re.search(r'^  "version": "(\d+\.\d+\.\d+)(-dev)?",$', text, re.M)
        if not v:
            die("library.json: could not parse version")
        return parse_version(v.group(1)), bool(v.group(2))
    if name == "platformio/osdp_config.h":
        v = re.search(r'^#define LIBOSDP_VERSION_STR\s+"(\d+\.\d+\.\d+)(-dev)?"$', text, re.M)
        if not v:
            die("platformio/osdp_config.h: could not parse LIBOSDP_VERSION_STR")
        return parse_version(v.group(1)), bool(v.group(2))
    if name == "dotnet/Directory.Build.props":
        v = re.search(r"^    <Version>(\d+\.\d+\.\d+)(-dev|\.\d+)?</Version>$", text, re.M)
        if not v:
            die("dotnet/Directory.Build.props: could not parse <Version>")
        return parse_version(v.group(1)), v.group(2) == "-dev"
    die(f"unknown version file {name}")


def _file_numeric_and_pre(root: Path, name: str) -> tuple[Version, bool]:
    return parse_version_file(name, (root / name).read_text())


def check_agreement(root: Path) -> None:
    """All version files must agree on numeric core and marker state."""
    reference = _file_numeric_and_pre(root, "CMakeLists.txt")
    for name in VERSION_FILES[1:]:
        got = _file_numeric_and_pre(root, name)
        if got != reference:
            die(
                f"version files disagree: CMakeLists.txt has {reference[0]}"
                f"{'-dev' if reference[1] else ''}, {name} has {got[0]}"
                f"{'-dev' if got[1] else ''}"
            )


def write_state(root: Path, state: State) -> None:
    """Write the numeric version + marker into all five version files."""
    v = state.version

    cmake = root / "CMakeLists.txt"
    text = cmake.read_text()
    text = _sub_once(cmake, r"^project\(libosdp VERSION \d+\.\d+\.\d+\)$",
                     f"project(libosdp VERSION {v})", text)
    text = _sub_once(cmake, r'^set\(LIBOSDP_PRERELEASE "[^"]*"\)$',
                     f'set(LIBOSDP_PRERELEASE "{MARKER if state.prerelease else ""}")', text)
    cmake.write_text(text)

    pyproject = root / "python/pyproject.toml"
    text = pyproject.read_text()
    text = _sub_once(pyproject, r'^version = "[^"]+"$',
                     f'version = "{_pyproject_version_string(state)}"', text)
    pyproject.write_text(text)

    library = root / "library.json"
    text = library.read_text()
    text = _sub_once(library, r'^  "version": "[^"]+",$',
                     f'  "version": "{_semver_string(state)}",', text)
    library.write_text(text)

    pio = root / "platformio/osdp_config.h"
    text = pio.read_text()
    text = _sub_once(pio, r'^#define PROJECT_VERSION(\s+)"[^"]+"$',
                     rf'#define PROJECT_VERSION\g<1>"{v}"', text)
    text = _sub_once(pio, r'^#define LIBOSDP_VERSION_STR(\s+)"[^"]+"$',
                     rf'#define LIBOSDP_VERSION_STR\g<1>"{_semver_string(state)}"', text)
    pio.write_text(text)

    props = root / "dotnet/Directory.Build.props"
    text = props.read_text()
    text = _sub_once(props, r"^    <Version>[^<]+</Version>$",
                     f"    <Version>{_nuget_version_string(state)}</Version>", text)
    props.write_text(text)


# ---------------------------------------------------------------------------
# changelog
# ---------------------------------------------------------------------------


def release_file(root: Path, version: Version) -> Path:
    return root / "CHANGELOG" / f"RELEASE-v{version}.md"


def last_release_tag(root: Path) -> str | None:
    tags = git(["tag", "-l", "v*", "--sort=-version:refname"], root).splitlines()
    return tags[0] if tags else None


def commit_hints(root: Path) -> list[str]:
    base = last_release_tag(root)
    rng = f"{base}..HEAD" if base else "HEAD"
    log = git(["log", rng, "--no-merges", "--format=%h %s"], root, check=False)
    hints = []
    for line in log.splitlines():
        # drop prior release/prepare commits so they don't seed the next log
        if re.match(r"^\w+ (Release|Prepare|Re-release) v", line):
            continue
        hints.append(f"- {line}")
    return hints


def scaffold_changelog(root: Path, version: Version) -> Path:
    path = release_file(root, version)
    if path.exists():
        die(f"Release file already exists: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    today = datetime.now(UTC).strftime("%Y-%m-%d")
    template = changelog_tool.render_release(
        changelog_tool.ReleaseEntry(
            version=f"v{version}",
            date=today,
            subject="Release subject ## TODO",
            sections=[("Enhancements", ["## TODO"]), ("Fixes", ["## TODO"])],
        )
    )
    hints = commit_hints(root)
    changes = "\n## Changes\n\n" + ("\n".join(hints) if hints else "-") + "\n"
    path.write_text(template + changes)
    return path


def stamp_release_date(path: Path) -> None:
    today = datetime.now(UTC).strftime("%Y-%m-%d")
    text = path.read_text()
    text = _sub_once(path, r"^release_date: .*$", f"release_date: {today}", text)
    path.write_text(text)


def validate_changelog(root: Path, path: Path, tag: str) -> None:
    if not path.exists():
        die(f"Missing release file: {path}")
    if "## Changes" in path.read_text():
        die(f"{path.name} still has a '## Changes' section — fold the hints into "
            "Enhancements/Fixes and delete it before publishing.")
    result = subprocess.run(
        [sys.executable, str(root / "scripts/changelog_tool.py"), "validate",
         "--file", str(path), "--expected-version", tag, "--quiet"],
        cwd=root,
    )
    if result.returncode != 0:
        die(f"{path.name} failed changelog validation")


# ---------------------------------------------------------------------------
# signature gate
# ---------------------------------------------------------------------------


def _fetch_key(key_url: str) -> bytes:
    try:
        data = urllib.request.urlopen(key_url, timeout=20).read()
    except Exception as exc:  # noqa: BLE001 - any fetch failure blocks the release
        die(f"could not fetch signing key from {key_url}: {exc}")
    if not data.strip():
        die(f"signing key at {key_url} is empty")
    return data


def _fingerprints(colons_output: str) -> set[str]:
    return {line.split(":")[9] for line in colons_output.splitlines()
            if line.startswith("fpr:")}


def check_signing_key_available(key_url: str) -> None:
    """Fail BEFORE any mutation if no local secret key matches the published key,
    so a mis-configured signing key never leaves a half-made release behind."""
    with tempfile.TemporaryDirectory() as home:
        os.chmod(home, 0o700)
        env = {**os.environ, "GNUPGHOME": home}
        imported = subprocess.run(["gpg", "--batch", "--import"],
                                  input=_fetch_key(key_url), env=env,
                                  capture_output=True)
        if imported.returncode != 0:
            die(f"failed to import signing key: {imported.stderr.decode().strip()}")
        published = _fingerprints(subprocess.run(
            ["gpg", "--with-colons", "--list-keys"], env=env,
            capture_output=True, text=True).stdout)
    local = _fingerprints(subprocess.run(
        ["gpg", "--with-colons", "--list-secret-keys"],
        capture_output=True, text=True).stdout)
    if not (published & local):
        die(f"no local secret key matches {key_url}; point git's user.signingkey "
            "at your published key before publishing")


# Primary key of the release identity. Signatures are accepted from any valid
# subkey it certifies, so a routine subkey rotation needs no change here.
RELEASE_KEY = "D8861B9C6B9C4284D980D6016804DFEE281234B2"


def verify_tag_signature(root: Path, tag: str, key_url: str) -> None:
    """Verify `tag` was signed by a subkey of RELEASE_KEY.

    A throwaway keyring is not sufficient on its own: key_url serves *every* key
    the account publishes. gpg names the certifying primary key in the last
    field of its VALIDSIG status line, so that is asserted instead of a specific
    subkey — a rotated subkey keeps working. GOODSIG is also required, since gpg
    withholds it (emitting EXPKEYSIG or REVKEYSIG) for an expired or revoked
    key. Raises SystemExit (via die) on any mismatch — callers roll back their
    mutations."""
    with tempfile.TemporaryDirectory() as home:
        os.chmod(home, 0o700)
        env = {**os.environ, "GNUPGHOME": home}
        imported = subprocess.run(["gpg", "--batch", "--import"],
                                  input=_fetch_key(key_url), env=env,
                                  capture_output=True)
        if imported.returncode != 0:
            die(f"failed to import signing key: {imported.stderr.decode().strip()}")
        verified = subprocess.run(
            ["git", "-c", "gpg.program=gpg", "verify-tag", "--raw", tag],
            cwd=root, env=env, capture_output=True, text=True,
        )
        status = f"{verified.stdout}{verified.stderr}"
        primary = ""
        for line in status.splitlines():
            if line.startswith("[GNUPG:] VALIDSIG "):
                primary = line.split()[-1]
        if "[GNUPG:] GOODSIG " not in status or primary != RELEASE_KEY:
            die(f"tag {tag} is not signed by the release key:\n{status.strip()}")


def require_clean_tree(root: Path) -> None:
    # Untracked files are ignored: they can't be committed by us and survive the
    # rollback's `reset --hard`. Only tracked modifications would make the commit
    # ambiguous or be lost on rollback.
    if git(["status", "--porcelain", "--untracked-files=no"], root):
        die("working tree has uncommitted changes to tracked files; "
            "commit or stash them before publishing")


def require_release_only_dirt(root: Path, path: Path) -> None:
    """Quick-release counterpart of require_clean_tree: the release's own files
    are expected to be uncommitted (that is the whole point), but nothing else
    may be, or the single Release commit would absorb unrelated work."""
    allowed = set(VERSION_FILES) | {str(path.relative_to(root))}
    stray = set()
    for line in git(["status", "--porcelain", "--untracked-files=no"],
                    root).splitlines():
        name = line[3:]
        if " -> " in name:  # rename entries read "R  old -> new"
            name = name.split(" -> ", 1)[1]
        name = name.strip('"')
        if name not in allowed:
            stray.add(name)
    if stray:
        die("working tree has uncommitted changes outside the release: "
            + ", ".join(sorted(stray))
            + "\ncommit or stash them before publishing")


# ---------------------------------------------------------------------------
# commands
# ---------------------------------------------------------------------------


STAGE_FILES = VERSION_FILES  # the five version files; changelog added per-command


def cmd_prepare(root: Path, args: argparse.Namespace) -> None:
    state = read_state(root)
    if state.prerelease:
        die(f"already in a prepared cycle for v{state.version}; run 'publish', "
            "or revert the Prepare commit to abandon it")
    head_version, _ = head_state(root)
    if state.version != head_version:
        die(f"a release of v{state.version} is already prepared and staged; run "
            f"'publish', or abandon it with:\n"
            f"  git restore --staged --worktree {' '.join(VERSION_FILES)}")
    if not tag_exists(root, f"v{state.version}"):
        die(f"v{state.version} is released in HEAD but not tagged; tag it with "
            f"'git tag -s -a v{state.version} -m \"Release v{state.version}\"' "
            "before preparing another release")
    check_agreement(root)

    if args.set:
        nxt = parse_version(args.set)
        if not (nxt.major, nxt.minor, nxt.patch) > (
            state.version.major, state.version.minor, state.version.patch
        ):
            die(f"--set {nxt} must be greater than the current v{state.version}")
    else:
        nxt = state.version.bumped(args.bump)

    path = release_file(root, nxt)
    if path.exists():
        die(f"Release file already exists: {path}")

    write_state(root, State(nxt, prerelease=args.cycle))
    scaffold_changelog(root, nxt)

    git(["add", *STAGE_FILES, str(path.relative_to(root))], root)
    rel = path.relative_to(root)
    if args.cycle:
        print(f"Prepared v{nxt} for an extended cycle "
              f"(marker set: builds now report {nxt}-{MARKER}...).")
    else:
        print(f"Prepared v{nxt} (staged, not committed — publish makes the "
              "one and only commit).")
    print(f"  edited + staged: {', '.join(STAGE_FILES)}")
    print(f"  scaffolded:      {rel}")
    if args.cycle:
        print("\nReview, then commit:")
        print(f'  git commit -s -m "Prepare v{nxt}"')
        print(f"\nEdit {rel} over the cycle; before publishing, fold the "
              "## Changes hints\ninto Enhancements/Fixes and delete that section.")
    else:
        print(f"\nEdit {rel} — fold the ## Changes hints into Enhancements/Fixes\n"
              "and delete that section — then:")
        print("  scripts/make_release.py publish")


def cmd_publish(root: Path, args: argparse.Namespace) -> None:
    if args.re_release:
        cmd_republish(root, args)
        return

    state = read_state(root)
    head_version, head_prerelease = head_state(root)
    # The bump is uncommitted ⇒ this is a quick release and the Release commit
    # carries it; equal versions ⇒ a committed Prepare, i.e. a cycle.
    quick = state.version != head_version
    if not quick and not state.prerelease:
        die("nothing prepared to publish; run 'prepare' first "
            "(or 'publish --re-release' to re-cut an existing release)")
    if quick and head_prerelease:
        die(f"HEAD is in a prepared cycle for v{head_version} but the worktree "
            f"says v{state.version}; reconcile the version files before publishing")
    check_agreement(root)

    version = state.version
    tag = f"v{version}"
    path = release_file(root, version)
    if quick:
        require_release_only_dirt(root, path)
    else:
        require_clean_tree(root)

    if tag_exists(root, tag):
        die(f"tag {tag} already exists; use 'publish --re-release' to re-cut it")

    # Validate and fail fast on a wrong signing key BEFORE touching anything, so a
    # rejected release leaves the tree clean and the command re-runnable.
    validate_changelog(root, path, tag)
    check_signing_key_available(args.key_url)

    saved = git(["rev-parse", "HEAD"], root)
    stamp_release_date(path)
    if state.prerelease:
        write_state(root, State(version, prerelease=False))
    git(["add", *STAGE_FILES, str(path.relative_to(root))], root)
    git(["commit", "-s", "-m", f"Release {tag}"], root)

    # Tagging is inside the rollback too: a missed YubiKey touch must not leave
    # a Release commit without its tag, which the next prepare would bump past.
    try:
        git(["tag", "-s", "-a", tag, "-m", f"Release {tag}"], root)
        verify_tag_signature(root, tag, args.key_url)
    except SystemExit:
        git(["tag", "-d", tag], root, check=False)
        # A quick release's changelog prose exists only in the index and worktree,
        # so --hard would destroy it. --soft restores exactly the pre-publish
        # state, and publish stays re-runnable: the bump is still uncommitted.
        git(["reset", "--soft" if quick else "--hard", saved], root, check=False)
        kept = ("the release files are left staged" if quick
                else "Release commit undone")
        die(f"signing or its check failed — rolled back (tag {tag} deleted, {kept}). "
            "Fix your signing key and re-run publish.")

    print(f"Released {tag} (builds now report {version}).")
    print(f"Signed and verified against {args.key_url}. Push with:")
    print(f"  git push origin {branch(root)} {tag}")


def cmd_republish(root: Path, args: argparse.Namespace) -> None:
    state = read_state(root)
    version = state.version
    tag = f"v{version}"
    if state.prerelease:
        die(f"v{version} is still prepared, not released; run a normal 'publish' first")
    if not tag_exists(root, tag):
        die(f"no existing tag {tag} to re-release; run a normal 'publish'")

    require_clean_tree(root)
    path = release_file(root, version)
    validate_changelog(root, path, tag)
    check_signing_key_available(args.key_url)

    saved = git(["rev-parse", "HEAD"], root)
    if args.post is not None:
        write_state(root, State(version, prerelease=False, post=args.post))
        git(["add", "python/pyproject.toml", "dotnet/Directory.Build.props"], root)
        git(["commit", "-s", "-m", f"Re-release {tag} (post{args.post})"], root)

    git(["tag", "-f", "-s", "-a", tag, "-m", f"Release {tag}"], root)
    try:
        verify_tag_signature(root, tag, args.key_url)
    except SystemExit:
        git(["reset", "--hard", saved], root, check=False)
        die(f"signature check failed for the re-cut {tag} — the post-bump commit "
            "(if any) was undone. The tag was force-moved; fix your signing key "
            "and re-run 'publish --re-release' to move it onto a good signature.")

    what = (f"{version}.post{args.post} to PyPI, "
            f"{version}.{args.post} to NuGet") if args.post is not None else version
    print(f"Re-cut {tag} onto HEAD ({what}). Force-push with:")
    print(f"  git push --force origin {tag}")
    if args.post is not None:
        print(f"  git push origin {branch(root)}   # the post-bump commit")


def branch(root: Path) -> str:
    name = git(["symbolic-ref", "--short", "-q", "HEAD"], root, check=False)
    return name or "HEAD"


def _label(state: tuple[Version, bool]) -> str:
    return f"{state[0]}{'-dev' if state[1] else ''}"


RELEASE_PATH_RE = re.compile(r"^CHANGELOG/RELEASE-(v[0-9A-Za-z][0-9A-Za-z.+-]*)\.md$")


def cmd_check_staged(root: Path) -> None:
    """Pre-commit gate. Reads the index (== the future commit) so a manual commit
    can't drift the version files or ship a broken release changelog. Silent + 0
    when the commit touches neither."""
    staged = set(git(["diff", "--cached", "--name-only"], root).splitlines())
    release_files = sorted(f for f in staged if RELEASE_PATH_RE.match(f))
    if not (staged & set(VERSION_FILES)) and not release_files:
        return

    def index_text(path: str) -> str:
        return git(["show", f":{path}"], root)

    states = {name: parse_version_file(name, index_text(name)) for name in VERSION_FILES}
    reference = states["CMakeLists.txt"]
    for name, state in states.items():
        if state != reference:
            die(f"pre-commit: version files disagree — {name} is {_label(state)}, "
                f"CMakeLists.txt is {_label(reference)}")

    numeric, prerelease = reference
    for rf in release_files:
        file_version = RELEASE_PATH_RE.match(rf).group(1)
        content = index_text(rf)
        if file_version == f"v{numeric}":
            # the in-flight release: a prepared cycle may commit a WIP stub;
            # once the marker is cleared (publish) it must be finalized + valid.
            if prerelease:
                continue
            if "## Changes" in content:
                die(f"pre-commit: {rf} still has a ## Changes section — fold it "
                    "into Enhancements/Fixes and delete it before publishing")
        result = subprocess.run(
            [sys.executable, str(root / "scripts/changelog_tool.py"), "validate",
             "--stdin", "--expected-version", file_version, "--quiet"],
            input=content, text=True, cwd=root)
        if result.returncode != 0:
            die(f"pre-commit: {rf} failed changelog validation")


def cmd_check_changelog(root: Path) -> None:
    """CI gate over the whole CHANGELOG directory. Only *published* releases are
    held to the finalized format; the in-flight file of a prepared cycle is a WIP
    stub by design (TODO markers, ## Changes hints) until publish clears the
    marker, so it is skipped."""
    state = read_state(root)
    directory = root / "CHANGELOG"
    files = changelog_tool.release_files_in_dir(directory)
    if not files:
        die(f"no release files found in {directory}")

    in_flight = f"v{state.version}" if state.prerelease else None
    checked = 0
    for path in files:
        version = changelog_tool.RELEASE_FILE_RE.fullmatch(path.name).group(1)
        if version == in_flight:
            continue
        validate_changelog(root, path, version)
        checked += 1

    if in_flight:
        print(f"{checked} published release file(s) valid; "
              f"skipped {in_flight} (prepared, not published)")
    else:
        print(f"{checked} published release file(s) valid")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="LibOSDP two-phase release helper",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    sub = parser.add_subparsers(dest="command", required=True)

    prepare = sub.add_parser("prepare", help="stage the next release")
    bump = prepare.add_mutually_exclusive_group()
    bump.add_argument("--major", dest="bump", action="store_const", const="major")
    bump.add_argument("--minor", dest="bump", action="store_const", const="minor")
    bump.add_argument("--patch", dest="bump", action="store_const", const="patch")
    prepare.add_argument("--set", help="set an explicit X.Y.Z instead of bumping")
    prepare.add_argument("--cycle", action="store_true",
                         help="extended release cycle: set the pre-release marker "
                              "and expect a 'Prepare vX.Y.Z' commit (default: a "
                              "single-commit release, published without one)")
    prepare.set_defaults(bump="patch")

    publish = sub.add_parser("publish", help="finalize + sign the prepared release")
    publish.add_argument("--re-release", action="store_true",
                         help="re-cut an already-published version onto fix commits")
    publish.add_argument("--post", type=int,
                         help="with --re-release: also emit X.Y.Z.postN to PyPI and "
                              "X.Y.Z.N to NuGet")
    publish.add_argument("--key-url", default=DEFAULT_KEY_URL,
                         help=f"public key the tag signature must match "
                              f"(default {DEFAULT_KEY_URL})")

    sub.add_parser("check-staged",
                   help="pre-commit gate: verify staged version files agree and a "
                        "staged release changelog is consistent")

    sub.add_parser("check-changelog",
                   help="CI gate: validate every published release file, skipping "
                        "the in-flight file of a prepared cycle")

    args = parser.parse_args()
    if getattr(args, "post", None) is not None and not args.re_release:
        parser.error("--post requires --re-release")
    if getattr(args, "post", None) is not None and args.post < 1:
        parser.error("--post must be 1 or more: NuGet normalises X.Y.Z.0 to X.Y.Z, "
                     "which is already published")
    if getattr(args, "set", None) and args.bump != "patch":
        parser.error("--set cannot be combined with --major/--minor/--patch")
    return args


def main() -> None:
    args = parse_args()
    root = repo_root()
    if args.command == "prepare":
        cmd_prepare(root, args)
    elif args.command == "publish":
        cmd_publish(root, args)
    elif args.command == "check-staged":
        cmd_check_staged(root)
    elif args.command == "check-changelog":
        cmd_check_changelog(root)


if __name__ == "__main__":
    main()
