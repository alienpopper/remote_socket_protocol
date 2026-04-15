"""Combine all protobuf and abseil static libraries into a single archive."""
from __future__ import annotations

import platform
import subprocess
import sys
import tempfile
from pathlib import Path


def combine_with_libtool(libs: list[Path], output: Path) -> None:
    result = subprocess.run(
        ["libtool", "-static", "-o", str(output), *[str(lib) for lib in libs]],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip() or result.stdout.strip())


def combine_with_ar_script(libs: list[Path], output: Path) -> None:
    mri_script = f"create {output}\n"
    for lib in libs:
        mri_script += f"addlib {lib}\n"
    mri_script += "save\nend\n"

    result = subprocess.run(["ar", "-M"], input=mri_script, text=True, capture_output=True)
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip() or result.stdout.strip())


def combine_by_extracting_objects(libs: list[Path], output: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="protobuf-combined-") as tmp:
        tmpdir = Path(tmp)
        for index, lib in enumerate(libs):
            extract_dir = tmpdir / f"lib_{index}"
            extract_dir.mkdir(parents=True, exist_ok=True)
            result = subprocess.run(["ar", "-x", str(lib)], cwd=extract_dir, capture_output=True, text=True)
            if result.returncode != 0:
                raise RuntimeError(f"failed to extract {lib}: {result.stderr.strip() or result.stdout.strip()}")

        objects = sorted(str(path.relative_to(tmpdir)) for path in tmpdir.rglob("*.o"))
        if not objects:
            raise RuntimeError("no object files were extracted from the input archives")

        result = subprocess.run(["ar", "-rcs", str(output), *objects], cwd=tmpdir, capture_output=True, text=True)
        if result.returncode != 0:
            raise RuntimeError(result.stderr.strip() or result.stdout.strip())


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <lib_dir> <output>", file=sys.stderr)
        return 1

    lib_dir = Path(sys.argv[1])
    output = Path(sys.argv[2]).resolve()

    if not lib_dir.is_dir():
        print(f"library directory does not exist: {lib_dir}", file=sys.stderr)
        return 1

    output.parent.mkdir(parents=True, exist_ok=True)

    if platform.system() == "Windows":
        libs = sorted(lib for lib in lib_dir.glob("*.lib") if lib.resolve() != output)
        if not libs:
            print(f"no .lib files found in {lib_dir}", file=sys.stderr)
            return 1
        cmd = ["lib", "/nologo", f"/OUT:{output}"] + [str(lib) for lib in libs]
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0:
            print(f"lib failed: {result.stderr}", file=sys.stderr)
            return 1
    else:
        libs = sorted(lib for lib in lib_dir.glob("*.a") if lib.resolve() != output)
        if not libs:
            print(f"no .a files found in {lib_dir}", file=sys.stderr)
            return 1

        if output.exists():
            output.unlink()

        try:
            if platform.system() == "Darwin":
                combine_with_libtool(libs, output)
            else:
                combine_with_ar_script(libs, output)
        except RuntimeError:
            combine_by_extracting_objects(libs, output)

        subprocess.run(["ranlib", str(output)], check=True)

    print(f"combined {len(libs)} libraries into {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
