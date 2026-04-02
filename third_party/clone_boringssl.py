from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path


REPO_URL = "https://github.com/google/boringssl.git"


def main() -> int:
    third_party_dir = Path(__file__).resolve().parent
    destination = third_party_dir / "boringssl"

    if destination.exists():
        print(f"boringssl already present at {destination}")
        return 0

    if shutil.which("git") is None:
        print("git is required to clone boringssl", file=sys.stderr)
        return 1

    subprocess.run(["git", "clone", REPO_URL, str(destination)], check=True)
    print(f"cloned boringssl into {destination}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())