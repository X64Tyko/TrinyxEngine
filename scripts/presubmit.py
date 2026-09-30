#!/usr/bin/env python3
"""Local presubmit for TrinyxEngine - runs the same gates on Windows and Linux.

Steps (each can be skipped):
  format  clang-format --dry-run over all first-party C/C++ files (--fix rewrites them)
  tidy    clang-tidy over C/C++ files changed since the upstream branch
  tests   the full local configuration array:
            headless        Release, headless, rollback            -> Testbed --headless
            net-norollback  Release, headless, network, no rollback -> Testbed --headless
            windowed        Debug, windowed, network, rollback      -> Testbed in a window
            editor          Debug editor build                      -> build/link check
          CI runs headless only; locally everything runs. --quick = headless only.

Runs automatically as a git pre-push hook (see .pre-commit-config.yaml), or by hand:
  python scripts/presubmit.py              # everything
  python scripts/presubmit.py --fix        # also apply clang-format fixes
  python scripts/presubmit.py --skip-tests # lint only
  python scripts/presubmit.py --quick      # lint + headless config only

Tools come from the pre-commit hook environment, or from `pip install -r scripts/requirements-dev.txt`.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import shutil
import subprocess
import sys
import sysconfig
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FIRST_PARTY_DIRS = ("src", "Testbed", "Playground", "TrinyxParser", "DeterminismTest")
CXX_SUFFIXES = {".h", ".hpp", ".inl", ".c", ".cpp", ".cc", ".cxx"}
TU_SUFFIXES = {".c", ".cpp", ".cc", ".cxx"}
CLANG_FORMAT_VERSION = "23.1.1"  # keep in sync with .pre-commit-config.yaml
HEADLESS_ARGS = ["--headless", "--max-frames", "120"]  # keep in sync with .github/workflows/ci.yml
WINDOWED_ARGS = ["--max-frames", "120"]
TEST_TIMEOUT_SEC = 900

IS_WINDOWS = platform.system() == "Windows"
OS_SUFFIX = "windows" if IS_WINDOWS else "linux"


def preset_binary_dir(preset_name: str) -> Path:
    """Build directory for a configure preset, resolved from CMakePresets.json (following `inherits`)."""
    presets = {p["name"]: p for p in json.loads((ROOT / "CMakePresets.json").read_text(encoding="utf-8"))["configurePresets"]}

    def find(name):
        preset = presets[name]
        if "binaryDir" in preset:
            return preset["binaryDir"]
        parents = preset.get("inherits", [])
        for parent in [parents] if isinstance(parents, str) else parents:
            found = find(parent)
            if found:
                return found
        return None

    raw = find(preset_name)
    if raw is None:
        sys.exit(f"presubmit: preset {preset_name} has no binaryDir")
    return Path(raw.replace("${sourceDir}", str(ROOT)).replace("${presetName}", preset_name))


class TestConfig:
    """One entry in the local test array: a preset to build and, optionally, a Testbed run."""

    def __init__(self, name, preset_base, run_args, needs_display=False):
        self.name = name
        self.preset = f"{preset_base}-{OS_SUFFIX}"
        self.build_dir = preset_binary_dir(self.preset)
        self.run_args = run_args  # None = build only
        self.needs_display = needs_display


# The full local array. CI runs headless only (no display on runners); locally we test everything.
TEST_CONFIGS = [
    TestConfig("headless", "presubmit", HEADLESS_ARGS),
    TestConfig("net-norollback", "presubmit-net-norollback", HEADLESS_ARGS),
    TestConfig("windowed", "presubmit-windowed", WINDOWED_ARGS, needs_display=True),
    TestConfig("editor", "presubmit-editor", None),  # Playground is interactive: build/link check only
]
# clang-tidy gets its own configure-only tree with every feature enabled, so it sees editor and
# networking sources too. On Windows this is a Ninja tree because the VS generator can't export
# compile_commands.json.
TIDY_PRESET = "tidy-windows" if IS_WINDOWS else "tidy-linux"
TIDY_DIR = preset_binary_dir(TIDY_PRESET)


def banner(text: str) -> None:
    print(f"\n=== {text} ===", flush=True)


def run(cmd: list[str], **kwargs) -> subprocess.CompletedProcess:
    print("$ " + " ".join(str(c) for c in cmd), flush=True)
    return subprocess.run(cmd, cwd=ROOT, **kwargs)


def git(*args: str) -> str:
    return subprocess.run(["git", *args], cwd=ROOT, check=True, capture_output=True, text=True).stdout


def require_tool(name: str) -> str:
    # pip --user installs often land outside PATH (e.g. %APPDATA%\Python\Scripts), so also look there.
    search = [os.environ.get("PATH", ""), sysconfig.get_path("scripts"), sysconfig.get_path("scripts", f"{os.name}_user")]
    path = shutil.which(name, path=os.pathsep.join(search))
    if not path:
        sys.exit(f"presubmit: '{name}' not found. Run: python -m pip install -r scripts/requirements-dev.txt")
    return path


def first_party_files() -> list[str]:
    files = git("ls-files", "--", *FIRST_PARTY_DIRS).splitlines()
    # Skip paths still in the index but gone from disk (e.g. an unstaged delete or move).
    return [f for f in files if Path(f).suffix in CXX_SUFFIXES and (ROOT / f).exists()]


def changed_files(base: str | None) -> list[str]:
    if base is None:
        for candidate in ("@{upstream}", "origin/Dev-Main"):
            if subprocess.run(["git", "rev-parse", "--verify", "-q", candidate], cwd=ROOT, capture_output=True).returncode == 0:
                base = candidate
                break
        else:
            return []
    merge_base = git("merge-base", "HEAD", base).strip()
    changed = git("diff", "--name-only", "--diff-filter=ACMR", merge_base, "--", *FIRST_PARTY_DIRS).splitlines()
    return [f for f in changed if Path(f).suffix in CXX_SUFFIXES and (ROOT / f).exists()]


# ---------------------------------------------------------------------------
# format
# ---------------------------------------------------------------------------
def step_format(fix: bool) -> bool:
    banner("clang-format")
    clang_format = require_tool("clang-format")
    version = subprocess.run([clang_format, "--version"], capture_output=True, text=True).stdout
    if CLANG_FORMAT_VERSION not in version:
        sys.exit(f"presubmit: clang-format {CLANG_FORMAT_VERSION} required, found: {version.strip()}")

    files = first_party_files()
    mode = ["-i"] if fix else ["--dry-run", "--Werror"]
    failed = False
    for i in range(0, len(files), 100):  # batch to stay under Windows command-line limits
        if subprocess.run([clang_format, *mode, *files[i:i + 100]], cwd=ROOT).returncode != 0:
            failed = True
    if failed:
        print("Formatting issues found. Fix with: python scripts/presubmit.py --fix --skip-tidy --skip-tests")
    return not failed


# ---------------------------------------------------------------------------
# tidy
# ---------------------------------------------------------------------------
def msvc_environment() -> dict[str, str]:
    """Environment from vcvars64.bat, so Ninja + cl.exe work outside a developer prompt."""
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
    install = subprocess.run([str(vswhere), "-latest", "-products", "*", "-requires",
        "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
        capture_output=True, text=True, check=True).stdout.strip()
    if not install:
        sys.exit("presubmit: no Visual Studio C++ toolset found (vswhere)")
    vcvars = Path(install) / "VC/Auxiliary/Build/vcvars64.bat"
    out = subprocess.run(f'"{vcvars}" >nul && set', shell=True, capture_output=True, text=True, check=True).stdout
    env = dict(line.split("=", 1) for line in out.splitlines() if "=" in line)
    ninja_dir = Path(install) / "Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja"
    if ninja_dir.exists():
        env["PATH"] = f"{ninja_dir};{env.get('PATH', '')}"
    return env


def ensure_compile_commands() -> Path:
    db = TIDY_DIR / "compile_commands.json"
    if db.exists():
        return db
    env = msvc_environment() if IS_WINDOWS else None
    if run(["cmake", "--preset", TIDY_PRESET], env=env).returncode != 0 or not db.exists():
        sys.exit(f"presubmit: configuring {TIDY_PRESET} did not produce {db}")
    return db


def step_tidy(base: str | None, all_files: bool) -> bool:
    banner("clang-tidy")
    clang_tidy = require_tool("clang-tidy")
    changed = first_party_files() if all_files else changed_files(base)
    if not changed:
        print("No changed C/C++ files.")
        return True

    db = ensure_compile_commands()
    in_db = {Path(e["file"]).resolve() for e in json.loads(db.read_text())}
    units = [f for f in changed if Path(f).suffix in TU_SUFFIXES and (ROOT / f).resolve() in in_db]
    headers = [f for f in changed if Path(f).suffix not in TU_SUFFIXES]
    if headers:
        # Headers are checked through the translation units that include them (HeaderFilterRegex).
        print(f"{len(headers)} changed header(s) are checked via including sources.")
    skipped = [f for f in changed if Path(f).suffix in TU_SUFFIXES and f not in units]
    if skipped:
        print("Not in this build configuration, skipped: " + ", ".join(skipped))
    if not units:
        return True

    # On Windows clang-tidy finds the MSVC standard library through the vcvars environment.
    env = msvc_environment() if IS_WINDOWS else None

    def tidy(unit: str) -> tuple[str, int, str]:
        result = subprocess.run([clang_tidy, "-p", str(TIDY_DIR), "--quiet", unit],
            cwd=ROOT, env=env, capture_output=True, encoding="utf-8", errors="replace")
        output = result.stdout + result.stderr if result.returncode else result.stdout
        return unit, result.returncode, output

    print(f"Checking {len(units)} file(s)...", flush=True)
    failed = []
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        for unit, code, output in pool.map(tidy, units):
            if output.strip():
                print(f"--- {unit}\n{output.rstrip()}", flush=True)
            if code != 0:
                failed.append(unit)
    if failed:
        print("clang-tidy errors (these block the push) in: " + ", ".join(failed))
    return not failed


# ---------------------------------------------------------------------------
# tests
# ---------------------------------------------------------------------------
def find_testbed(build_dir: Path) -> Path:
    exe = "Testbed.exe" if IS_WINDOWS else "Testbed"
    for sub in ("bin", "bin/Release", "bin/Debug", "Testbed"):
        candidate = build_dir / sub / exe
        if candidate.exists():
            return candidate
    sys.exit(f"presubmit: built {exe} not found under {build_dir}")


def has_display() -> bool:
    return IS_WINDOWS or bool(os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY"))


def run_config(cfg: TestConfig) -> bool:
    banner(f"build{' + Testbed' if cfg.run_args is not None else ''} ({cfg.preset})")
    if run(["cmake", "--preset", cfg.preset]).returncode != 0:
        return False
    if run(["cmake", "--build", "--preset", cfg.preset, "--parallel"]).returncode != 0:
        return False
    if cfg.run_args is None:
        return True
    if cfg.needs_display and not has_display():
        print(f"No display available — skipping the {cfg.name} Testbed run (build passed).")
        return True
    testbed = find_testbed(cfg.build_dir)
    try:
        result = run([str(testbed), *cfg.run_args], timeout=TEST_TIMEOUT_SEC)
    except subprocess.TimeoutExpired:
        print(f"Testbed timed out after {TEST_TIMEOUT_SEC}s")
        return False
    if result.returncode != 0:
        print(f"Testbed exited with code {result.returncode}")
    return result.returncode == 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--fix", action="store_true", help="apply clang-format fixes instead of only checking")
    parser.add_argument("--base", help="git ref to diff against for clang-tidy (default: upstream branch)")
    parser.add_argument("--all", action="store_true", help="clang-tidy every first-party file, not just changed ones")
    parser.add_argument("--skip-format", action="store_true")
    parser.add_argument("--skip-tidy", action="store_true")
    parser.add_argument("--skip-tests", action="store_true")
    parser.add_argument("--quick", action="store_true", help="test only the headless configuration")
    parser.add_argument("--configs", help="comma-separated test configs to run: "
        + ", ".join(c.name for c in TEST_CONFIGS) + " (default: all)")
    args = parser.parse_args()

    # clang-tidy echoes source lines, which may contain non-ASCII characters.
    sys.stdout.reconfigure(errors="replace")

    if os.environ.get("TNX_SKIP_PRESUBMIT") == "1":
        print("presubmit: skipped (TNX_SKIP_PRESUBMIT=1)")
        return 0

    results = {}
    if not args.skip_format:
        results["format"] = step_format(args.fix)
    if not args.skip_tidy:
        results["tidy"] = step_tidy(args.base, args.all)
    if not args.skip_tests:
        wanted = {"headless"} if args.quick else (set(args.configs.split(",")) if args.configs else None)
        for cfg in TEST_CONFIGS:
            if wanted is None or cfg.name in wanted:
                results[cfg.name] = run_config(cfg)

    banner("summary")
    for name, ok in results.items():
        print(f"  {name:<15} {'PASS' if ok else 'FAIL'}")
    return 0 if all(results.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
