#!/usr/bin/env python3
"""Reject forbidden undefined references from the v8host engine persona."""

import argparse
import os
import pathlib
import re
import subprocess
import sys


FORBIDDEN = (
    ("client implementation", "v8host::client"),
    ("coordinator namespace", "v8host::coordinator"),
    ("coordinator entry", "v8hostbrokerrun"),
    ("coordinator implementation", "brokerservice"),
    ("client rendezvous", "brokerrendezvous"),
    ("client pipe transport", "pipeclienttransport"),
    ("peer authentication", "peerauth"),
    ("pipe authentication", "authenticatepipepeer"),
    ("pipe authentication", "authenticateserver"),
    ("pipe authentication", "authenticateclient"),
    ("named-pipe helper", "namedpipe"),
    ("broker ABI/core", "sbox_broker_"),
    ("Chromium sandbox core", "sandbox::"),
    ("Chromium sandbox core", "brokerservices"),
    ("Chromium sandbox core", "targetservices"),
    ("Chromium sandbox core", "policybase"),
    ("Chromium sandbox core", "sbox_core"),
)

# The public client API (src/public/v8host_client.h): extern "C" functions, so
# their undefined symbols are plain names, possibly decorated (_name, _name@N)
# or imported (__imp_name).
CLIENT_API = re.compile(
    r"^(?:__imp_)?_?v8host_(?:initialize|(?:broker|session|run|runtime|worker|config)_\w+)(?:@\d+)?$"
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--stamp", required=True)
    parser.add_argument("objects", nargs="+")
    args = parser.parse_args()

    vswhere = pathlib.Path(
        os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
    ) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    found = subprocess.run(
        [
            str(vswhere),
            "-latest",
            "-products",
            "*",
            "-find",
            r"VC\Tools\Llvm\x64\bin\llvm-nm.exe",
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    nm = found.stdout.splitlines()[0] if found.returncode == 0 and found.stdout else ""
    if not nm:
        print("llvm-nm.exe not found through vswhere", file=sys.stderr)
        return 1

    failures = []
    for obj in sorted(args.objects):
        result = subprocess.run(
            [nm, "--undefined-only", "--demangle", obj],
            check=False,
            capture_output=True,
            text=True,
        )
        if result.returncode:
            print(result.stderr, file=sys.stderr, end="")
            return result.returncode
        for line in result.stdout.splitlines():
            lowered = line.lower()
            for category, needle in FORBIDDEN:
                if needle in lowered:
                    failures.append((obj, category, line.strip()))
            fields = line.split()
            if len(fields) == 2 and CLIENT_API.match(fields[1]):
                failures.append((obj, "client API", line.strip()))

    if failures:
        print("v8host engine layering check FAILED:", file=sys.stderr)
        for obj, category, symbol in failures:
            print(f"  {obj}: {category}: {symbol}", file=sys.stderr)
        return 1

    pathlib.Path(args.stamp).write_text("PASS\n", encoding="ascii")
    print(f"v8host engine layering check PASS ({len(args.objects)} objects)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
