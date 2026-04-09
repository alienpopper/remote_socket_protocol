from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path


REPO_URL = "https://github.com/nlohmann/json.git"
VERSION = "v3.12.0"


def main() -> int:
    third_party_dir = Path(__file__).resolve().parent
    destination = third_party_dir / "json"

    if shutil.which("git") is None:
        print("git is required to clone nlohmann/json", file=sys.stderr)
        return 1

    if destination.exists():
        if not (destination / ".git").exists():
            print(f"json path exists but is not a git checkout: {destination}", file=sys.stderr)
            return 1

        subprocess.run(
            [
                "git",
                "-C",
                str(destination),
                "fetch",
                "--depth",
                "1",
                "origin",
                f"refs/tags/{VERSION}:refs/tags/{VERSION}",
            ],
            check=True,
        )
        subprocess.run(["git", "-C", str(destination), "checkout", "--force", VERSION], check=True)
        print(f"updated nlohmann/json in {destination} to {VERSION}")
        return 0

    subprocess.run(["git", "clone", "--branch", VERSION, "--depth", "1", REPO_URL, str(destination)], check=True)
    print(f"cloned nlohmann/json {VERSION} into {destination}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())