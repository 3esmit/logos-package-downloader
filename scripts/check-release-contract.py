#!/usr/bin/env python3

import argparse
import re
import sys
from pathlib import Path


SEMVER = re.compile(
    r"^(0|[1-9][0-9]*)\."
    r"(0|[1-9][0-9]*)\."
    r"(0|[1-9][0-9]*)"
    r"(?:-("
    r"(?:0|[1-9][0-9]*|[0-9]*[A-Za-z-][0-9A-Za-z-]*)"
    r"(?:\.(?:0|[1-9][0-9]*|[0-9]*[A-Za-z-][0-9A-Za-z-]*))*"
    r"))?"
    r"(?:\+([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?$"
)


def fail(message: str) -> None:
    print(f"release contract error: {message}", file=sys.stderr)
    raise SystemExit(1)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path("."))
    parser.add_argument("--tag")
    args = parser.parse_args()

    version_path = args.root / "VERSION"
    changelog_path = args.root / "CHANGELOG.md"
    workflow_path = args.root / ".github" / "workflows" / "release.yml"

    if not version_path.is_file():
        fail("VERSION is missing")
    if not changelog_path.is_file():
        fail("CHANGELOG.md is missing")
    if not workflow_path.is_file():
        fail("release workflow is missing")

    raw_version = version_path.read_text(encoding="utf-8")
    if not raw_version.endswith("\n") or raw_version.count("\n") != 1:
        fail("VERSION must contain one newline-terminated line")

    version = raw_version[:-1]
    if SEMVER.fullmatch(version) is None:
        fail(f"VERSION is not strict SemVer: {version}")

    expected_heading = re.compile(
        rf"^## \[{re.escape(version)}\] - [0-9]{{4}}-[0-9]{{2}}-[0-9]{{2}}$",
        re.MULTILINE,
    )
    heading_count = len(expected_heading.findall(changelog_path.read_text(encoding="utf-8")))
    if heading_count != 1:
        fail(
            "CHANGELOG.md must contain exactly one dated heading for "
            f"[{version}]"
        )

    if args.tag is not None and args.tag != version:
        fail(f"tag {args.tag} does not match VERSION {version}")

    workflow = workflow_path.read_text(encoding="utf-8")
    for unsafe_command in ("gh release delete", "--cleanup-tag"):
        if unsafe_command in workflow:
            fail(f"release workflow contains unsafe rollback command: {unsafe_command}")

    print(version)


if __name__ == "__main__":
    main()
