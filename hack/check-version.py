#!/usr/bin/env python3
"""Check that manifest.yaml's metadata.version matches an expected version.

The manifest version is what a node reports in `talosctl get extensions`, and it
is a completely separate string from the git tag and from the Makefile's
VERSION. Nothing makes them agree, so a release cut without bumping the manifest
ships an extension that reports the previous version forever.
"""

import sys

try:
    import yaml
except ImportError:
    sys.exit("PyYAML is required: pip install pyyaml")


def main():
    if len(sys.argv) != 2:
        print("usage: check-version.py EXPECTED_VERSION", file=sys.stderr)
        return 2
    expected = sys.argv[1].lstrip("v")

    with open("manifest.yaml") as f:
        manifest = yaml.safe_load(f)

    actual = manifest.get("metadata", {}).get("version")

    if not isinstance(actual, str):
        # An unquoted 0.1 parses as a float, which Talos cannot decode either.
        print(f"manifest.yaml metadata.version must be a quoted string, got {actual!r}")
        return 1

    if actual != expected:
        print(f"version mismatch: manifest.yaml says {actual!r}, expected {expected!r}")
        print()
        print("Bump metadata.version in manifest.yaml (and the VERSION default in")
        print("the Makefile) so the extension reports the version it ships as.")
        return 1

    print(f"manifest.yaml metadata.version is {actual!r}, as expected")
    return 0


if __name__ == "__main__":
    sys.exit(main())
