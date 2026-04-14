"""Combine all protobuf and abseil static libraries into a single archive."""
from __future__ import annotations

import glob
import os
import platform
import subprocess
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <lib_dir> <output>", file=sys.stderr)
        return 1

    lib_dir = Path(sys.argv[1])
    output = Path(sys.argv[2])

    if not lib_dir.is_dir():
        print(f"library directory does not exist: {lib_dir}", file=sys.stderr)
        return 1

    output.parent.mkdir(parents=True, exist_ok=True)

    if platform.system() == "Windows":
        libs = sorted(lib_dir.glob("*.lib"))
        if not libs:
            print(f"no .lib files found in {lib_dir}", file=sys.stderr)
            return 1
        cmd = ["lib", "/nologo", f"/OUT:{output}"] + [str(lib) for lib in libs]
    else:
        libs = sorted(lib_dir.glob("*.a"))
        if not libs:
            print(f"no .a files found in {lib_dir}", file=sys.stderr)
            return 1

        # Use ar script mode to combine archives
        mri_script = f"create {output}\n"
        for lib in libs:
            mri_script += f"addlib {lib}\n"
        mri_script += "save\nend\n"

        result = subprocess.run(["ar", "-M"], input=mri_script, text=True, capture_output=True)
        if result.returncode != 0:
            print(f"ar failed: {result.stderr}", file=sys.stderr)
            return 1

        subprocess.run(["ranlib", str(output)], check=True)
        print(f"combined {len(libs)} libraries into {output}")
        return 0

    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"lib failed: {result.stderr}", file=sys.stderr)
        return 1

    print(f"combined {len(libs)} libraries into {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
