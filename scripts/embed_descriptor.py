#!/usr/bin/env python3
"""Convert a binary file (e.g. protobuf FileDescriptorSet) to a C++ header.

Replaces xxd -i with a portable Python script that works on any platform.

Usage:
    python scripts/embed_descriptor.py input.desc output.hpp [--name SYMBOL]

Generates a header with:
    namespace rsp::schema {
    inline constexpr unsigned char kSymbol[] = { 0x0a, 0x12, ... };
    inline constexpr size_t kSymbolSize = NNN;
    }
"""

import argparse
import os
import sys


def to_cpp_array(data: bytes, symbol: str, indent: str = "    ") -> str:
    lines = []
    lines.append("#pragma once")
    lines.append("")
    lines.append("#include <cstddef>")
    lines.append("")
    lines.append("namespace rsp { namespace schema {")
    lines.append("")
    lines.append(f"inline constexpr unsigned char {symbol}[] = {{")

    # Emit 16 bytes per line
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        hex_values = ", ".join(f"0x{b:02x}" for b in chunk)
        trailing = "," if i + 16 < len(data) else ""
        lines.append(f"{indent}{hex_values}{trailing}")

    lines.append("};")
    lines.append("")
    lines.append(f"inline constexpr size_t {symbol}Size = sizeof({symbol});")
    lines.append("")
    lines.append("}}  // namespace rsp::schema")
    lines.append("")
    return "\n".join(lines)


def symbol_from_filename(filename: str) -> str:
    """Derive a C++ symbol name from a filename: bsd_sockets.desc -> kBsdSocketsDesc"""
    base = os.path.splitext(os.path.basename(filename))[0]
    parts = base.replace("-", "_").split("_")
    camel = "".join(part.capitalize() for part in parts)
    return f"k{camel}"


def main():
    parser = argparse.ArgumentParser(description="Embed a binary file as a C++ header")
    parser.add_argument("input", help="Input binary file (e.g. .desc)")
    parser.add_argument("output", help="Output C++ header file (e.g. .hpp)")
    parser.add_argument("--name", default=None,
                        help="C++ symbol name (default: derived from input filename)")
    args = parser.parse_args()

    with open(args.input, "rb") as f:
        data = f.read()

    if not data:
        print(f"error: {args.input} is empty", file=sys.stderr)
        return 1

    symbol = args.name or symbol_from_filename(args.input)
    header = to_cpp_array(data, symbol)

    os.makedirs(os.path.dirname(args.output) or ".", exist_ok=True)
    with open(args.output, "w", newline="\n") as f:
        f.write(header)

    print(f"Generated {args.output} ({len(data)} bytes, symbol={symbol})")
    return 0


if __name__ == "__main__":
    sys.exit(main() or 0)
