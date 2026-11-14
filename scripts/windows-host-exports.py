#!/usr/bin/env python3
"""Generate the filtered Windows export .def for the ER-301 package-host ABI."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

NM_ROW = re.compile(r"^(?:[0-9A-Fa-f]+)?\s+([A-Za-z?])\s+(.+)$")
CODE_TYPES = {"T", "t", "W", "w"}
OD_MANGLED_PATTERNS = (
    re.compile(r"^_ZN[KVR]*2od"),
    re.compile(r"^_ZT(?:V|I|S|T)N2od"),
    re.compile(r"^_ZTh.*_N2od"),
    re.compile(r"^_ZTv.*_N2od"),
    re.compile(r"^_ZGVN2od"),
    re.compile(r"^_ZZN2od"),
)
RUNTIME_PREFIXES = (
    ".", "_Unwind_", "_GCC_", "__gxx_", "__cxa_",
    "__imp_", "__mingw_", "_mingw_", "__chkstk", "___chkstk",
)


def is_package_host_symbol(name: str) -> bool:
    # Third-party package C++ imports are part of the ER-301 od::* ABI. Keep
    # compiler/libstdc++ implementation symbols private to each package.
    if name.startswith("_Z"):
        return any(pattern.search(name) for pattern in OD_MANGLED_PATTERNS)
    if name.startswith(RUNTIME_PREFIXES):
        return False
    # Preserve ER-301/Lua C APIs and data exports such as globalConfig.
    return True


def run_text(command: list[str]) -> str:
    try:
        return subprocess.check_output(
            command, stderr=subprocess.STDOUT, text=True, errors="replace"
        )
    except subprocess.CalledProcessError as exc:
        sys.stderr.write(exc.output)
        raise


def collect_symbols(nm: str, archives: list[Path]) -> dict[str, str]:
    result: dict[str, str] = {}
    for archive in archives:
        output = run_text([nm, "-g", "--defined-only", str(archive)])
        for raw in output.splitlines():
            match = NM_ROW.match(raw.strip())
            if not match:
                continue
            symbol_type, name = match.groups()
            if not is_package_host_symbol(name):
                continue
            previous = result.get(name)
            # Prefer a code definition if duplicate archive members advertise
            # the same weak symbol with different nm classifications.
            if previous is None or (previous not in CODE_TYPES and symbol_type in CODE_TYPES):
                result[name] = symbol_type
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument("archives", type=Path, nargs="+")
    parser.add_argument("--nm", default="nm")
    parser.add_argument("--library", default="plugin.dll")
    args = parser.parse_args()

    symbols = collect_symbols(args.nm, args.archives)
    if not symbols:
        raise RuntimeError("No ER-301 package-host exports found")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", newline="\n") as handle:
        handle.write(f"LIBRARY {args.library}\n")
        handle.write("EXPORTS\n")
        for name in sorted(symbols):
            if symbols[name] not in CODE_TYPES:
                handle.write(f"  {name} DATA\n")
            else:
                handle.write(f"  {name}\n")

    print(f"Generated {args.output} with {len(symbols)} ER-301 package-host exports.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
