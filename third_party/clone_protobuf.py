from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path


REPO_URL = "https://github.com/protocolbuffers/protobuf.git"
VERSION = "v29.6"


def init_submodules(destination: Path) -> None:
    subprocess.run(
        ["git", "-C", str(destination), "submodule", "update", "--init", "--depth", "1"],
        check=True,
    )


def main() -> int:
    third_party_dir = Path(__file__).resolve().parent
    destination = third_party_dir / "protobuf"

    if shutil.which("git") is None:
        print("git is required to clone protobuf", file=sys.stderr)
        return 1

    if destination.exists():
        if not (destination / ".git").exists():
            print(f"protobuf path exists but is not a git checkout: {destination}", file=sys.stderr)
            return 1

        subprocess.run(
            ["git", "-C", str(destination), "fetch", "--depth", "1", "origin",
             f"refs/tags/{VERSION}:refs/tags/{VERSION}"],
            check=True,
        )
        subprocess.run(["git", "-C", str(destination), "checkout", "--force", VERSION], check=True)
        init_submodules(destination)
        print(f"updated protobuf in {destination} to {VERSION}")
        return 0

    subprocess.run([
        "git", "clone", "--branch", VERSION, "--depth", "1",
        "--recurse-submodules", "--shallow-submodules",
        REPO_URL, str(destination),
    ], check=True)
    print(f"cloned protobuf {VERSION} into {destination}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())