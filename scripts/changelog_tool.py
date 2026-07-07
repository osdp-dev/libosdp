#!/usr/bin/env python3

import argparse
import dataclasses
import email.utils
import json
import pathlib
import re
import sys
from datetime import UTC, datetime


VERSION_RE = re.compile(r"^[0-9A-Za-z][0-9A-Za-z.+-]*$")
RELEASE_FILE_RE = re.compile(r"^RELEASE-(v[0-9A-Za-z][0-9A-Za-z.+-]*)\.md$")
MARKDOWN_HEADING_RE = re.compile(r"^## ([A-Z][A-Za-z ]+)$")


@dataclasses.dataclass
class ReleaseEntry:
    version: str
    date: str
    subject: str
    sections: list[tuple[str, list[str]]]

    @property
    def path_name(self) -> str:
        return f"RELEASE-{self.version}.md"


def die(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="LibOSDP changelog tooling")
    sub = parser.add_subparsers(dest="command", required=True)

    init_cmd = sub.add_parser("init", help="Create a new release changelog template")
    init_cmd.add_argument("--version", required=True)
    init_cmd.add_argument("--output-dir", default="CHANGELOG")
    init_cmd.add_argument("--date", default=datetime.now(UTC).strftime("%Y-%m-%d"))

    validate = sub.add_parser("validate", help="Validate release files")
    validate.add_argument("--file")
    validate.add_argument("--dir")
    validate.add_argument("--stdin", action="store_true")
    validate.add_argument("--expected-version")
    validate.add_argument("--quiet", action="store_true")

    notes = sub.add_parser("notes", help="Extract GitHub release notes from a release file")
    notes.add_argument("--file")
    notes.add_argument("--stdin", action="store_true")
    notes.add_argument("--output")

    return parser.parse_args()


def normalize_version(raw: str) -> str:
    version = raw.strip()
    if version.startswith("v"):
        version = version[1:]
    version = re.sub(r"\s*-\s*", "-", version)
    version = re.sub(r"\s+", "-", version)
    version = re.sub(r"-{2,}", "-", version)
    version = version.lower()
    if not VERSION_RE.fullmatch(version):
        die(f"Invalid release version: {raw}")
    return f"v{version}"


def human_date_to_iso(value: str) -> str:
    value = value.strip()
    if not value:
        die("Missing release date")
    if re.fullmatch(r"\d{4}-\d{2}-\d{2}", value):
        return value
    for fmt in ("%d %B %Y", "%d %b %Y"):
        try:
            return datetime.strptime(value, fmt).strftime("%Y-%m-%d")
        except ValueError:
            pass
    try:
        return email.utils.parsedate_to_datetime(value).strftime("%Y-%m-%d")
    except (TypeError, ValueError):
        die(f"Unsupported release date format: {value}")


def split_front_matter(text: str) -> tuple[dict[str, str], str]:
    if not text.startswith("---\n"):
        die("Release file is missing markdown front matter")
    end = text.find("\n---\n", 4)
    if end < 0:
        die("Release file front matter is not terminated")

    raw_front_matter = text[4:end]
    body = text[end + len("\n---\n"):]
    data: dict[str, str] = {}
    for line in raw_front_matter.splitlines():
        if not line.strip():
            continue
        if ":" not in line:
            die(f"Invalid front matter line: {line}")
        key, value = line.split(":", 1)
        data[key.strip()] = value.strip().strip('"')
    return data, body.lstrip("\n")


def parse_markdown_release(text: str, expected_version: str | None = None) -> ReleaseEntry:
    if "## TODO" in text:
        die("Release file still contains TODO markers")
    meta, body = split_front_matter(text)
    extra_keys = sorted(set(meta) - {"release_date", "release_version"})
    missing_keys = sorted({"release_date", "release_version"} - set(meta))
    if missing_keys:
        die(f"Release file front matter missing keys: {', '.join(missing_keys)}")
    if extra_keys:
        die(f"Release file front matter has unsupported keys: {', '.join(extra_keys)}")

    version = normalize_version(meta["release_version"])
    if expected_version and version != normalize_version(expected_version):
        die(f"Release version mismatch: expected {expected_version}, found {version}")
    date = human_date_to_iso(meta["release_date"])

    lines = body.splitlines()
    i = 0
    while i < len(lines) and not lines[i].strip():
        i += 1
    subject_lines = []
    while i < len(lines):
        line = lines[i]
        if MARKDOWN_HEADING_RE.fullmatch(line.strip()):
            break
        subject_lines.append(line)
        i += 1

    subject = "\n".join(subject_lines).strip()
    if not subject:
        die(f"Release {version} is missing its subject block")

    sections: list[tuple[str, list[str]]] = []
    seen_titles: set[str] = set()
    while i < len(lines):
        while i < len(lines) and not lines[i].strip():
            i += 1
        if i >= len(lines):
            break
        match = MARKDOWN_HEADING_RE.fullmatch(lines[i].strip())
        if not match:
            die(f"Release {version} has invalid section heading: {lines[i]}")
        title = match.group(1)
        if title in seen_titles:
            die(f"Release {version} repeats section {title}")
        seen_titles.add(title)
        i += 1

        items: list[str] = []
        while i < len(lines):
            stripped = lines[i].strip()
            if not stripped:
                i += 1
                if items:
                    break
                continue
            if MARKDOWN_HEADING_RE.fullmatch(stripped):
                break
            bullet = re.fullmatch(r"-\s+(.+)", stripped)
            if not bullet:
                die(f"Release {version} has a non-bullet line in section {title}: {lines[i]}")
            items.append(bullet.group(1).strip())
            i += 1
        if not items:
            die(f"Release {version} section {title} must contain at least one bullet")
        sections.append((title, items))

    if not sections:
        die(f"Release {version} must include at least one section")

    return ReleaseEntry(version=version, date=date, subject=subject, sections=sections)


def render_release(entry: ReleaseEntry) -> str:
    parts = [
        "---",
        f"release_date: {entry.date}",
        f"release_version: {entry.version}",
        "---",
        "",
        entry.subject.strip(),
        "",
    ]
    for title, items in entry.sections:
        parts.append(f"## {title}")
        parts.append("")
        for item in items:
            parts.append(f"- {item}")
        parts.append("")
    return "\n".join(parts).rstrip() + "\n"


def read_text(path: pathlib.Path) -> str:
    return path.read_text(encoding="utf-8")


def write_text(path: pathlib.Path, data: str) -> None:
    path.write_text(data, encoding="utf-8")


def release_files_in_dir(directory: pathlib.Path) -> list[pathlib.Path]:
    if not directory.exists():
        die(f"No release files found in {directory}")
    if not directory.is_dir():
        die(f"Expected a changelog directory: {directory}")
    paths = []
    for path in directory.iterdir():
        match = RELEASE_FILE_RE.fullmatch(path.name)
        if match and path.is_file():
            paths.append(path)
    return sorted(paths, key=lambda path: path.name, reverse=True)


def validate_release_file(path: pathlib.Path, expected_version: str | None = None, quiet: bool = False) -> ReleaseEntry:
    file_match = RELEASE_FILE_RE.fullmatch(path.name)
    if not file_match:
        die(f"Invalid release file name: {path.name}")
    file_version = normalize_version(file_match.group(1))
    expected = normalize_version(expected_version) if expected_version else file_version
    entry = parse_markdown_release(read_text(path), expected)
    if entry.version != file_version:
        die(f"Release version in {path.name} does not match the file name")
    if not quiet:
        print(json.dumps(dataclasses.asdict(entry), indent=2))
    return entry


def command_init(args: argparse.Namespace) -> None:
    version = normalize_version(args.version)
    date = human_date_to_iso(args.date)
    output_dir = pathlib.Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    output_path = output_dir / f"RELEASE-{version}.md"
    if output_path.exists():
        die(f"Release file already exists: {output_path}")

    template = render_release(
        ReleaseEntry(
            version=version,
            date=date,
            subject="Release subject ## TODO",
            sections=[
                ("Enhancements", ["## TODO"]),
                ("Fixes", ["## TODO"]),
            ],
        )
    )
    write_text(output_path, template)
    print(output_path)


def load_single_release(args: argparse.Namespace, expected_version: str | None = None) -> ReleaseEntry:
    if args.stdin:
        text = sys.stdin.read()
        return parse_markdown_release(text, expected_version)
    if not args.file:
        die("A release file path is required")
    return validate_release_file(pathlib.Path(args.file), expected_version, quiet=True)


def command_validate(args: argparse.Namespace) -> None:
    if args.stdin:
        entry = parse_markdown_release(sys.stdin.read(), args.expected_version)
        if not args.quiet:
            print(json.dumps(dataclasses.asdict(entry), indent=2))
        return

    if args.file:
        validate_release_file(pathlib.Path(args.file), args.expected_version, quiet=args.quiet)
        return

    if args.dir:
        directory = pathlib.Path(args.dir)
        files = release_files_in_dir(directory)
        if not files:
            die(f"No release files found in {directory}")
        entries = [validate_release_file(path, quiet=True) for path in files]
        if not args.quiet:
            print(json.dumps([dataclasses.asdict(entry) for entry in entries], indent=2))
        return

    die("Expected one of --file, --dir, or --stdin")


def command_notes(args: argparse.Namespace) -> None:
    entry = load_single_release(args)
    lines = [entry.subject.strip(), ""]
    for title, items in entry.sections:
        lines.append(f"## {title}")
        lines.append("")
        for item in items:
            lines.append(f"- {item}")
        lines.append("")
    content = "\n".join(lines).rstrip() + "\n"

    if args.output:
        write_text(pathlib.Path(args.output), content)
    else:
        sys.stdout.write(content)


def main() -> None:
    args = parse_args()
    if args.command == "init":
        command_init(args)
        return
    if args.command == "validate":
        command_validate(args)
        return
    if args.command == "notes":
        command_notes(args)
        return
    die(f"Unknown command: {args.command}")


if __name__ == "__main__":
    main()
