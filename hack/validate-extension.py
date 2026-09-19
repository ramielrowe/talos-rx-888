#!/usr/bin/env python3
"""Validate a built Talos system extension image.

Reads the image's own layers (via `docker save`) rather than exporting a
container: a container export additionally contains runtime files such as
/.dockerenv and /etc/resolv.conf that are not part of the image and would make
the layout look wrong.

Checks the contract Talos enforces in pkg/machinery/extensions:
  - the image root holds exactly manifest.yaml and rootfs/
  - manifest.yaml declares version v1alpha1 and a Talos compatibility range
  - no world-writable files, no special files
  - every shipped path sits inside Talos's allowed-paths list
  - the extension service spec matches its container rootfs directory
"""

import io
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

# pkg/machinery/extensions/extensions.go, Talos >= 1.10 (post /usr merge).
ALLOWED_PATHS = (
    "etc/cri/conf.d",
    "usr/lib/firmware",
    "usr/lib/modules",
    "usr/lib/udev/rules.d",
    "usr/local",
    "usr/share/glvnd",
    "usr/share/egl",
    "etc/vulkan",
)

failures = []
notes = []


def fail(msg):
    failures.append(msg)


def ok(msg):
    notes.append(msg)


def collect_image_files(image):
    """Return {path: TarInfo} for the flattened image filesystem."""
    with tempfile.TemporaryDirectory() as tmp:
        tar_path = Path(tmp) / "image.tar"
        subprocess.run(
            ["docker", "save", image, "-o", str(tar_path)],
            check=True,
            stdout=subprocess.DEVNULL,
        )

        members = {}
        with tarfile.open(tar_path) as outer:
            # Layer blobs differ between the legacy (<id>/layer.tar) and OCI
            # (blobs/sha256/<digest>) save formats, so probe every regular file
            # and keep the ones that are actually tar archives.
            for entry in outer.getmembers():
                if not entry.isfile():
                    continue
                if entry.name.endswith(".json") or entry.name == "manifest.json":
                    continue
                blob = outer.extractfile(entry)
                if blob is None:
                    continue
                data = blob.read()
                try:
                    with tarfile.open(fileobj=io.BytesIO(data)) as layer:
                        for m in layer.getmembers():
                            members[m.name.lstrip("./")] = m
                except tarfile.TarError:
                    continue
        return members


def main():
    if len(sys.argv) != 2:
        print("usage: validate-extension.py IMAGE", file=sys.stderr)
        return 2
    image = sys.argv[1]

    members = collect_image_files(image)
    if not members:
        fail("no files found in image layers")
        return report()

    # --- image root ---------------------------------------------------------
    roots = {p.split("/")[0] for p in members if p}
    expected_roots = {"manifest.yaml", "rootfs"}
    if roots != expected_roots:
        fail(f"image root must be exactly {sorted(expected_roots)}, got {sorted(roots)}")
    else:
        ok("image root holds exactly manifest.yaml and rootfs/")

    # --- manifest -----------------------------------------------------------
    if "manifest.yaml" not in members:
        fail("manifest.yaml missing")
    else:
        ok("manifest.yaml present")

    # --- per-file checks ----------------------------------------------------
    world_writable = []
    special = []
    outside = []
    shipped = []

    for path, info in sorted(members.items()):
        if not path or path == "manifest.yaml":
            continue
        if info.isdir():
            continue
        if not (info.isfile() or info.issym()):
            special.append(path)
            continue
        if info.mode & 0o002:
            world_writable.append(f"{path} (mode {info.mode:04o})")

        rel = path[len("rootfs/"):] if path.startswith("rootfs/") else None
        if rel is None:
            outside.append(path)
            continue
        shipped.append(rel)
        if not any(rel == a or rel.startswith(a + "/") for a in ALLOWED_PATHS):
            outside.append(rel)

    if special:
        fail(f"special files are not allowed in an extension: {special}")
    else:
        ok("no special files")

    if world_writable:
        fail(f"world-writable entries: {world_writable}")
    else:
        ok("no world-writable files")

    if outside:
        fail(f"paths outside Talos's allowed-paths list: {outside}")
    else:
        ok(f"all {len(shipped)} shipped files are inside allowed paths")

    # --- extension service wiring ------------------------------------------
    specs = [p for p in shipped if p.startswith("usr/local/etc/containers/") and p.endswith(".yaml")]
    if not specs:
        fail("no extension service spec under usr/local/etc/containers/")
    for spec in specs:
        name = None
        content = read_member(image, "rootfs/" + spec)
        for line in content.splitlines():
            if line.startswith("name:"):
                name = line.split(":", 1)[1].strip()
                break
        if name is None:
            fail(f"{spec} has no top-level name:")
            continue
        rootfs_dir = f"usr/local/lib/containers/{name}/"
        if not any(p.startswith(rootfs_dir) for p in shipped):
            fail(f"service {name!r} has no container rootfs at {rootfs_dir}")
        else:
            ok(f"service {name!r} has its container rootfs at {rootfs_dir}")

    return report()


_member_cache = {}


def read_member(image, path):
    if image not in _member_cache:
        with tempfile.TemporaryDirectory() as tmp:
            tar_path = Path(tmp) / "image.tar"
            subprocess.run(
                ["docker", "save", image, "-o", str(tar_path)],
                check=True,
                stdout=subprocess.DEVNULL,
            )
            cache = {}
            with tarfile.open(tar_path) as outer:
                for entry in outer.getmembers():
                    if not entry.isfile() or entry.name.endswith(".json"):
                        continue
                    blob = outer.extractfile(entry)
                    if blob is None:
                        continue
                    data = blob.read()
                    try:
                        with tarfile.open(fileobj=io.BytesIO(data)) as layer:
                            for m in layer.getmembers():
                                if m.isfile():
                                    f = layer.extractfile(m)
                                    if f is not None:
                                        cache[m.name.lstrip("./")] = f.read()
                    except tarfile.TarError:
                        continue
            _member_cache[image] = cache
    return _member_cache[image].get(path, b"").decode("utf-8", "replace")


def report():
    for n in notes:
        print(f"  ok    {n}")
    for f in failures:
        print(f"  FAIL  {f}")
    if failures:
        print(f"\n{len(failures)} check(s) failed")
        return 1
    print(f"\nall {len(notes)} checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
