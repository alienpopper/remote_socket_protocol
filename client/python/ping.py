#!/usr/bin/env python3
"""RSP ping tool — Python equivalent of client/nodejs/ping.js."""

import argparse
import asyncio
import json
import sys

from rsp_client import RSPClient


async def main() -> None:
    parser = argparse.ArgumentParser(description="Ping an RSP node")
    parser.add_argument("transport_spec", help="tcp:<host>:<port>")
    parser.add_argument("destination_node_id", help="target node UUID")
    args = parser.parse_args()

    async with RSPClient() as client:
        await client.connect(args.transport_spec)
        ok = await client.ping(args.destination_node_id)
        print(json.dumps({
            "local_node_id": client.node_id,
            "resource_manager_node_id": client.peer_node_id,
            "destination_node_id": args.destination_node_id,
            "success": ok,
        }, indent=2))
        if not ok:
            sys.exit(1)


if __name__ == "__main__":
    asyncio.run(main())
