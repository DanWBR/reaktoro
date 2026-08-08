#!/usr/bin/env python3
"""Collects the flat C API and every shared library it needs into one flat folder.

The C API links against Reaktoro, which links against Optima, phreeqc4rkt, ThermoFun and the rest,
all of them shared libraries that live in the conda environment the build happened in. A machine
running DWSIM has no conda environment, so the closure has to travel with the library.

The closure is walked rather than listed. A hand-written list is right until a dependency of a
dependency changes, and then it is wrong in a way that only shows up as a load failure on a user's
machine.

Copying is not enough on Unix: the libraries record where they expect to find each other, and what
they record points into the build machine's conda prefix. The last step rewrites those paths to say
"next to me" instead.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

# Libraries that belong to the operating system. Shipping these is at best pointless and at worst
# breaks the host: the system's own copy is the one that has to win.
SYSTEM_DIRECTORIES = {
    "linux": ("/lib/", "/lib64/", "/usr/lib/", "/usr/lib64/"),
    "darwin": ("/usr/lib/", "/System/"),
    "win32": ("c:\\windows\\",),
}


def platform_key() -> str:
    if sys.platform.startswith("linux"):
        return "linux"
    if sys.platform == "darwin":
        return "darwin"
    return "win32"


def is_system_library(path: Path) -> bool:
    text = str(path).lower().replace("\\", "/") if platform_key() != "win32" else str(path).lower()
    for prefix in SYSTEM_DIRECTORIES[platform_key()]:
        if text.startswith(prefix.lower().replace("\\", "/") if platform_key() != "win32" else prefix.lower()):
            return True
    return False


def dependencies_linux(path: Path, search: list[Path]) -> list[Path]:
    result = subprocess.run(["ldd", str(path)], capture_output=True, text=True)
    found = []

    for line in result.stdout.splitlines():
        match = re.search(r"=>\s+(/\S+)", line)
        if match:
            found.append(Path(match.group(1)))
            continue

        # An installed library carries no RPATH back to the build tree, so ldd reports its
        # siblings as "not found" and has no path to give. Look them up where the build put them.
        match = re.match(r"\s*(\S+)\s+=>\s+not found", line)
        if match:
            leaf = match.group(1)
            for directory in search:
                candidate = directory / leaf
                if candidate.exists():
                    found.append(candidate)
                    break

    return found


def dependencies_macos(path: Path, search: list[Path]) -> list[Path]:
    result = subprocess.run(["otool", "-L", str(path)], capture_output=True, text=True)
    found = []
    for line in result.stdout.splitlines()[1:]:
        name = line.strip().split(" (")[0]
        if not name:
            continue
        # An @rpath entry names a file, not a place. Look for it where the build put things.
        if name.startswith("@rpath/") or name.startswith("@loader_path/"):
            leaf = name.split("/")[-1]
            for directory in search:
                candidate = directory / leaf
                if candidate.exists():
                    found.append(candidate)
                    break
            continue
        found.append(Path(name))
    return found


def dependencies_windows(path: Path, search: list[Path]) -> list[Path]:
    import pefile

    binary = pefile.PE(str(path), fast_load=True)
    binary.parse_data_directories(
        directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"]])

    found = []
    for entry in getattr(binary, "DIRECTORY_ENTRY_IMPORT", []):
        name = entry.dll.decode("utf-8")
        for directory in search:
            candidate = directory / name
            if candidate.exists():
                found.append(candidate)
                break
    binary.close()
    return found


def dependencies(path: Path, search: list[Path]) -> list[Path]:
    key = platform_key()
    if key == "linux":
        return dependencies_linux(path, search)
    if key == "darwin":
        return dependencies_macos(path, search)
    return dependencies_windows(path, search)


def closure(entry: Path, search: list[Path]) -> list[Path]:
    """Every non-system library reachable from the entry point, the entry point included."""
    seen: dict[str, Path] = {}
    pending = [entry]

    while pending:
        current = pending.pop()
        key = current.name.lower()
        if key in seen:
            continue
        seen[key] = current

        for dependency in dependencies(current, search):
            if not dependency.exists() or is_system_library(dependency):
                continue
            if dependency.name.lower() not in seen:
                pending.append(dependency)

    return list(seen.values())


def relocate_linux(directory: Path) -> None:
    """Points every library at the folder it is in, rather than at the conda prefix it was built in."""
    for path in sorted(directory.glob("*.so*")):
        subprocess.run(["patchelf", "--set-rpath", "$ORIGIN", str(path)], check=True)


def relocate_macos(directory: Path) -> None:
    for path in sorted(directory.glob("*.dylib")):
        subprocess.run(["install_name_tool", "-id", "@loader_path/" + path.name, str(path)],
                       check=True)

        result = subprocess.run(["otool", "-L", str(path)], capture_output=True, text=True)
        for line in result.stdout.splitlines()[1:]:
            name = line.strip().split(" (")[0]
            leaf = name.split("/")[-1]
            if not name or name.startswith("/usr/lib") or name.startswith("/System"):
                continue
            if (directory / leaf).exists() and name != "@loader_path/" + leaf:
                subprocess.run(
                    ["install_name_tool", "-change", name, "@loader_path/" + leaf, str(path)],
                    check=True)

        subprocess.run(["codesign", "--force", "--sign", "-", str(path)], check=False)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rid", required=True, help="the .NET runtime identifier, e.g. linux-x64")
    parser.add_argument("--prefix", required=True, help="where the build was installed")
    parser.add_argument("--out", required=True, help="where to write <rid>/")
    arguments = parser.parse_args()

    prefix = Path(arguments.prefix).resolve()
    conda = Path(os.environ.get("CONDA_PREFIX", ""))

    if platform_key() == "win32":
        entry = prefix / "bin" / "ReaktoroC.dll"
        search = [prefix / "bin", conda / "Library" / "bin", conda / "bin"]
    else:
        suffix = "dylib" if platform_key() == "darwin" else "so"
        entry = prefix / "lib" / f"libReaktoroC.{suffix}"
        search = [prefix / "lib", conda / "lib"]

    if not entry.exists():
        print(f"No library at {entry}.", file=sys.stderr)
        return 1

    destination = Path(arguments.out) / arguments.rid
    destination.mkdir(parents=True, exist_ok=True)

    libraries = closure(entry, search)

    for library in sorted(libraries, key=lambda p: p.name.lower()):
        shutil.copy2(library, destination / library.name)
        print(f"  {library.name}  <-  {library.parent}")

    if platform_key() == "linux":
        relocate_linux(destination)
    elif platform_key() == "darwin":
        relocate_macos(destination)

    total = sum(f.stat().st_size for f in destination.iterdir())
    print(f"\n{len(libraries)} libraries, {total / 1024 / 1024:.1f} MB, in {destination}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
