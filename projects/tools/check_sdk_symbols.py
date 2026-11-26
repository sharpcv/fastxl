#!/usr/bin/env python3
"""Fail when a Linux SDK binary needs a newer libc/libstdc++ than allowed.

The prebuilt ``libfastxl.so`` ships to every distribution the README lists, so
it must not pick up symbol-version floors from the build host. The SDK is built
inside ``manylinux_2_28`` (glibc 2.28) and links libstdc++/libgcc statically;
this check is the gate for both properties.

    python3 projects/tools/check_sdk_symbols.py build/lib/libfastxl.so
    python3 projects/tools/check_sdk_symbols.py build/lib/libfastxl.so \
        --max-glibc 2.28
"""

import argparse
import re
import subprocess
import sys

# libstdc++/libgcc are static in the SDK, so any versioned reference means the
# link flags regressed and the binary would need a matching host runtime.
CXX_RUNTIME_VERSION = re.compile(r"\b(?:GLIBCXX|CXXABI|GCC)_[0-9]+(?:\.[0-9]+)*\b")
GLIBC_VERSION = re.compile(r"\bGLIBC_([0-9]+(?:\.[0-9]+)+)\b")
DYNAMIC_CXX = re.compile(r"lib(?:stdc\+\+|gcc_s|supc\+\+)\.so")


def objdump(arguments):
    result = subprocess.run(
        ["objdump", *arguments], capture_output=True, text=True
    )
    if result.returncode != 0:
        raise SystemExit("objdump %s failed: %s" % (" ".join(arguments), result.stderr))
    return result.stdout


def as_version(text):
    return tuple(int(part) for part in text.split("."))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary")
    parser.add_argument("--max-glibc", default="2.28")
    arguments = parser.parse_args()
    limit = as_version(arguments.max_glibc)

    symbols = objdump(["-T", arguments.binary])
    glibc = {}
    for line in symbols.splitlines():
        match = re.search(r"\(GLIBC_([0-9.]+)\)\s+(\S+)\s*$", line)
        if match:
            glibc.setdefault(as_version(match.group(1)), set()).add(match.group(2))
    too_new = sorted(version for version in glibc if version > limit)
    cxx = sorted(set(CXX_RUNTIME_VERSION.findall(symbols)))
    dynamics = []
    for line in objdump(["-p", arguments.binary]).splitlines():
        fields = line.split()
        if fields and fields[0] == "NEEDED" and fields[-1] and DYNAMIC_CXX.search(fields[-1]):
            dynamics.append(fields[-1])

    failures = []
    if too_new:
        details = []
        for version in too_new:
            names = ", ".join(sorted(glibc[version])[:4])
            details.append("GLIBC_%s (%s)" % (".".join(map(str, version)), names))
        failures.append(
            "glibc symbols newer than %s: %s"
            % (arguments.max_glibc, "; ".join(details))
        )
    if cxx:
        failures.append(
            "dynamic libstdc++/libgcc symbol versions: %s" % ", ".join(cxx)
        )
    if dynamics:
        failures.append("dynamic C++ runtime dependencies: %s" % ", ".join(dynamics))

    if failures:
        for failure in failures:
            print("FAIL: %s" % failure, file=sys.stderr)
        return 1
    newest = max(glibc) if glibc else (0,)
    print(
        "sdk symbols ok: max GLIBC_%s, no dynamic libstdc++/libgcc"
        % ".".join(map(str, newest))
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
