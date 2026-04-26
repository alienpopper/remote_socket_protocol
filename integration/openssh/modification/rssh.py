#!/usr/bin/env python3
"""rssh - convenience wrapper for SSH over RSP.

Builds a per-connection rsp_ssh config from a shared base config, then
execs ssh with rsp_ssh as the ProxyCommand.

Usage:
  rssh [user@]<name>[%transport] [ssh-options...]
  rssh [user@]{node-id}[%transport]  [ssh-options...]

Target formats:
  house                     resolve 'house' via RSP name service
  user@house                same, log in as 'user'
  house%tcp:10.0.0.1:3939   override RM transport
  {cf605c79-...}            connect by RSP service node ID directly

ssh-options are passed through verbatim to the underlying ssh invocation.

Default config: ~/.ssh/rssh.conf.json
Required field:  rsp_transport   - RM transport, e.g. "tcp:rm-host:3939"
Optional fields: endorsement_node_id - ES node ID for endorsement
                 rsp_ssh_binary      - path to rsp_ssh binary
"""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

DEFAULT_CONFIG_PATHS = [
    os.path.expanduser("~/.ssh/rssh.conf.json"),
    "/etc/rsp-ssh/rssh.conf.json",
]

# SSH options that consume the following argument as their value.
_SSH_VALUE_FLAGS = set("-b -c -D -E -e -F -I -i -J -L -l -m -o -p -Q -R -S -W -w".split())

# Matches {uuid} target syntax.
_NODE_ID_RE = re.compile(r"^\{([0-9a-fA-F-]+)\}$")


def _find_rsp_ssh(config: dict) -> str:
    if "rsp_ssh_binary" in config:
        path = config["rsp_ssh_binary"]
        if not os.path.isfile(path):
            raise RuntimeError(f"rsp_ssh_binary '{path}' not found")
        return path

    # Look relative to this script: script lives at
    # integration/openssh/modification/rssh.py, binary at build/bin/rsp_ssh.
    script_dir = os.path.dirname(os.path.realpath(__file__))
    repo_root = os.path.normpath(os.path.join(script_dir, "..", "..", ".."))
    candidate = os.path.join(repo_root, "build", "bin", "rsp_ssh")
    if os.path.isfile(candidate) and os.access(candidate, os.X_OK):
        return candidate

    # Also check alongside the script (for installed copies).
    candidate = os.path.join(script_dir, "rsp_ssh")
    if os.path.isfile(candidate) and os.access(candidate, os.X_OK):
        return candidate

    found = shutil.which("rsp_ssh")
    if found:
        return found

    raise RuntimeError(
        "rsp_ssh binary not found. "
        "Build it with 'make rsp-ssh', add it to PATH, "
        "or set 'rsp_ssh_binary' in your rssh config."
    )


def _load_config() -> dict:
    for path in DEFAULT_CONFIG_PATHS:
        if os.path.isfile(path):
            with open(path) as f:
                return json.load(f)
    raise RuntimeError(
        f"No rssh config found. Create one at {DEFAULT_CONFIG_PATHS[0]}\n"
        "Required field: rsp_transport (e.g. \"tcp:rm-host:3939\")"
    )


def _parse_target(target: str) -> tuple[str | None, str | None, str | None, str | None]:
    """Parse [user@]<name|{node-id}>[%transport].

    Returns (user, service_name, service_node_id, transport_override).
    Exactly one of service_name / service_node_id will be set.
    """
    user: str | None = None
    transport_override: str | None = None

    if "@" in target:
        user, target = target.split("@", 1)

    if "%" in target:
        target, transport_override = target.split("%", 1)

    m = _NODE_ID_RE.match(target)
    if m:
        return user, None, m.group(1), transport_override

    return user, target, None, transport_override


def _split_args(argv: list[str]) -> tuple[str | None, list[str]]:
    """Separate the rssh target from the remaining ssh passthrough args.

    The target is the first argument that does not start with '-' and is
    not the value consumed by a preceding ssh flag.  Everything else is
    collected into ssh_args in its original order.
    """
    target: str | None = None
    ssh_args: list[str] = []
    skip_next = False

    for arg in argv:
        if skip_next:
            ssh_args.append(arg)
            skip_next = False
            continue

        if arg.startswith("-"):
            ssh_args.append(arg)
            if arg in _SSH_VALUE_FLAGS:
                skip_next = True
            continue

        if target is None:
            target = arg
        else:
            ssh_args.append(arg)

    return target, ssh_args


def main() -> int:
    target, ssh_args = _split_args(sys.argv[1:])

    if target is None:
        print(__doc__, file=sys.stderr)
        return 1

    try:
        config = _load_config()
    except RuntimeError as e:
        print(f"rssh: {e}", file=sys.stderr)
        return 1

    try:
        rsp_ssh = _find_rsp_ssh(config)
    except RuntimeError as e:
        print(f"rssh: {e}", file=sys.stderr)
        return 1

    user, service_name, service_node_id, transport_override = _parse_target(target)

    rsp_transport = transport_override or config.get("rsp_transport")
    if not rsp_transport:
        print(
            "rssh: no rsp_transport configured and none given via %transport",
            file=sys.stderr,
        )
        return 1

    conn_config: dict = {"rsp_transport": rsp_transport}
    if service_node_id:
        conn_config["resource_service_node_id"] = service_node_id
    else:
        conn_config["resource_service_name"] = service_name

    if "endorsement_node_id" in config:
        conn_config["endorsement_node_id"] = config["endorsement_node_id"]

    ssh_host = service_name or service_node_id
    ssh_target = f"{user}@{ssh_host}" if user else ssh_host

    with tempfile.NamedTemporaryFile(mode="w", suffix=".json", delete=False) as tf:
        json.dump(conn_config, tf)
        temp_config = tf.name

    try:
        proxy_cmd = f"{rsp_ssh} {temp_config}"
        cmd = ["ssh", "-o", f"ProxyCommand={proxy_cmd}", ssh_target] + ssh_args
        return subprocess.run(cmd).returncode
    finally:
        os.unlink(temp_config)


if __name__ == "__main__":
    sys.exit(main())
