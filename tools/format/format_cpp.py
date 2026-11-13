#!/usr/bin/env python3
"""Check or apply the repository C++ format with the pinned clang-format.

The clang-format version is pinned in tools/format/requirements.txt so that every machine and CI runner produces the
same output. The first run installs it into out/tools/clang-format (ignored by Git); later runs reuse it. Files listed
in .clang-format-ignore (third-party and generated code) are skipped by clang-format itself.

  python tools/format/format_cpp.py            # check every tracked C++ file, exit 1 if any needs formatting
  python tools/format/format_cpp.py --fix      # rewrite every tracked C++ file in place
  python tools/format/format_cpp.py --changed  # check only lines changed since the merge base with origin/main
  python tools/format/format_cpp.py --changed --fix
"""

from __future__ import annotations

import argparse
import concurrent.futures
import os
import re
import subprocess
import sys
import venv
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
REQUIREMENTS = REPO_ROOT / "tools" / "format" / "requirements.txt"
TOOL_ROOT = REPO_ROOT / "out" / "tools" / "clang-format"
CPP_SUFFIXES = (".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp")


def pinned_version() -> str:
    for line in REQUIREMENTS.read_text(encoding="utf-8").splitlines():
        match = re.fullmatch(r"\s*clang-format==([0-9.]+)\s*", line)
        if match:
            return match.group(1)
    raise SystemExit(f"no clang-format==<version> pin in {REQUIREMENTS}")


def tool_path(name: str) -> Path:
    scripts = TOOL_ROOT / ("Scripts" if os.name == "nt" else "bin")
    return scripts / (name + (".exe" if os.name == "nt" else ""))


def installed_version(clang_format: Path) -> str | None:
    if not clang_format.is_file():
        return None
    result = subprocess.run([str(clang_format), "--version"], capture_output=True, text=True)
    match = re.search(r"clang-format version ([0-9.]+)", result.stdout)
    return match.group(1) if match else None


def ensure_clang_format() -> Path:
    """Returns the pinned clang-format, installing or replacing it under out/tools/clang-format when needed."""
    version = pinned_version()
    clang_format = tool_path("clang-format")
    if installed_version(clang_format) == version:
        return clang_format
    print(f"Installing clang-format {version} into {TOOL_ROOT.relative_to(REPO_ROOT)}", file=sys.stderr)
    if not tool_path("python").is_file():
        venv.create(TOOL_ROOT, with_pip=True)
    subprocess.run([str(tool_path("python")), "-m", "pip", "install", "--quiet", "--disable-pip-version-check", "-r",
                    str(REQUIREMENTS)], check=True)
    found = installed_version(clang_format)
    if found != version:
        raise SystemExit(f"expected clang-format {version} at {clang_format}, found {found}")
    return clang_format


def tracked_cpp_files() -> list[str]:
    output = subprocess.run(["git", "ls-files", "-z"], cwd=REPO_ROOT, capture_output=True, check=True).stdout
    return [path for path in output.decode("utf-8").split("\0") if path.endswith(CPP_SUFFIXES)]


def run_whole_files(clang_format: Path, fix: bool, jobs: int) -> int:
    files = tracked_cpp_files()
    mode = ["-i"] if fix else ["--dry-run", "--Werror", "--ferror-limit=1"]  # first difference per file

    def run(batch: list[str]) -> subprocess.CompletedProcess:
        return subprocess.run([str(clang_format), *mode, *batch], cwd=REPO_ROOT, capture_output=True, text=True,
                              encoding="utf-8", errors="replace")

    batches = [files[index:index + 40] for index in range(0, len(files), 40)]
    failed = set()
    errors = False
    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        for result in pool.map(run, batches):
            if result.stderr:
                sys.stderr.write(result.stderr)
            if result.returncode != 0:
                unformatted = re.findall(r"^(.+?):\d+:\d+: (?:error|warning): code should be clang-formatted",
                                         result.stderr, re.MULTILINE)
                failed.update(unformatted)
                # A failure without format violations is clang-format itself failing (bad config, unreadable file).
                errors = errors or not unformatted
    if errors:
        print("\nclang-format failed; see the messages above.", file=sys.stderr)
        return 2
    if fix:
        print(f"Formatted {len(files)} tracked C++ files.")
        return 0
    if failed:
        print(f"\n{len(failed)} file(s) need formatting. Run: python tools/format/format_cpp.py --fix", file=sys.stderr)
        return 1
    print(f"All {len(files)} tracked C++ files are formatted.")
    return 0


def run_changed_lines(clang_format: Path, fix: bool, base: str) -> int:
    merge_base = subprocess.run(["git", "merge-base", base, "HEAD"], cwd=REPO_ROOT, capture_output=True, text=True)
    if merge_base.returncode != 0:
        raise SystemExit(f"cannot find the merge base with {base}; fetch it first (git fetch origin main)")
    command = [str(tool_path("git-clang-format")), "--binary", str(clang_format),
               "--extensions", ",".join(suffix[1:] for suffix in CPP_SUFFIXES), merge_base.stdout.strip()]
    # --force: formatting files with unstaged edits is the normal case before a commit.
    command.insert(1, "--force" if fix else "--diff")
    result = subprocess.run(command, cwd=REPO_ROOT, capture_output=True, text=True, encoding="utf-8",
                            errors="replace")
    sys.stdout.write(result.stdout)
    sys.stderr.write(result.stderr)
    if result.returncode == 0:
        return 0
    # git-clang-format exits 1 after changing files, and with --diff when lines need formatting; anything else is the
    # tool failing.
    if fix and result.stdout.startswith("changed files:"):
        return 0
    if not fix and result.stdout.startswith("diff --git"):
        print("\nChanged lines need formatting. Run: python tools/format/format_cpp.py --changed --fix", file=sys.stderr)
        return 1
    print("\ngit-clang-format failed; see the messages above.", file=sys.stderr)
    return 2


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--fix", action="store_true", help="rewrite files in place instead of checking")
    parser.add_argument("--changed", action="store_true",
                        help="only lines changed since the merge base with --base (staged, unstaged and committed)")
    parser.add_argument("--base", default="origin/main", help="base ref for --changed (default: origin/main)")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4, help="parallel clang-format processes")
    parser.add_argument("--print-path", action="store_true", help="install if needed and print the clang-format path")
    args = parser.parse_args()

    clang_format = ensure_clang_format()
    if args.print_path:
        print(clang_format)
        return 0
    if args.changed:
        return run_changed_lines(clang_format, args.fix, args.base)
    return run_whole_files(clang_format, args.fix, args.jobs)


if __name__ == "__main__":
    sys.exit(main())
