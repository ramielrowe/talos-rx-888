#!/usr/bin/env python3
"""Validate a built Talos system extension image.

Reads the image's own layers (via `docker save`) rather than exporting a
container: a container export additionally contains runtime files such as
/.dockerenv and /etc/resolv.conf that are not part of the image and would make
the layout look wrong.

Checks the contract Talos enforces in pkg/machinery/extensions:
  - the image root holds exactly manifest.yaml and rootfs/
  - manifest.yaml parses, declares version v1alpha1, and carries usable metadata
  - no world-writable files OR DIRECTORIES, no special files
  - every shipped path sits inside Talos's allowed-paths list
  - every extension service spec passes the same rules Talos applies, and has a
    matching container rootfs

Worth knowing: of these, only the image-root rule is enforced by `imager`.
The content rules live behind Builder.ExtensionValidateContents, which nothing
in the Talos tree sets, so they are enforced by Image Factory and by
siderolabs/extensions CI -- and here. That is exactly why this script exists.
"""

import io
import re
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

try:
    import yaml
except ImportError:
    sys.exit("PyYAML is required: pip install pyyaml")

# pkg/machinery/extensions/extensions.go. Union across v1.10 - v1.14: entries
# added in later releases are harmless to accept on an older target, since Talos
# itself is the thing that would reject them and it only gets more permissive.
ALLOWED_PATHS = (
    "etc/cri/conf.d",
    "etc/ld.so.cache",
    "etc/ld.so.conf",
    "etc/vulkan",
    "usr/bin/ldconfig",
    "usr/bin/nvidia-cdi-hook",
    "usr/bin/nvidia-ctk",
    "usr/bin/nvidia-modprobe",
    "usr/bin/nvidia-pcc",
    "usr/bin/nvidia-smi",
    "usr/bin/nvme",
    "usr/lib/firmware",
    "usr/lib/ld-linux-aarch64.so.1",
    "usr/lib/ld-linux-x86-64.so.2",
    "usr/lib/modules",
    "usr/lib/udev/rules.d",
    "usr/local",
    "usr/share/egl",
    "usr/share/glvnd",
)

# pkg/machinery/extensions/services
SERVICE_NAME_RE = re.compile(r"^[-_a-z0-9]+$")
RESTART_KINDS = {"always", "never", "untilSuccess"}
DEPENDS_KEYS = {"service", "path", "network", "time", "configuration"}

failures = []
notes = []


def fail(msg):
    failures.append(msg)


def ok(msg):
    notes.append(msg)


def load_image(image):
    """Return {path: (TarInfo, bytes|None)} for the flattened image filesystem."""
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
                if not entry.isfile() or entry.name.endswith(".json"):
                    continue
                blob = outer.extractfile(entry)
                if blob is None:
                    continue
                data = blob.read()
                try:
                    with tarfile.open(fileobj=io.BytesIO(data)) as layer:
                        for m in layer.getmembers():
                            name = m.name
                            if name.startswith("./"):
                                name = name[2:]
                            name = name.rstrip("/")
                            if not name:
                                continue
                            content = None
                            if m.isfile():
                                f = layer.extractfile(m)
                                if f is not None:
                                    content = f.read()
                            members[name] = (m, content)
                except tarfile.TarError:
                    continue
        return members


def check_manifest(members):
    entry = members.get("manifest.yaml")
    if entry is None:
        fail("manifest.yaml missing")
        return

    try:
        doc = yaml.safe_load(entry[1] or b"")
    except yaml.YAMLError as e:
        fail(f"manifest.yaml is not valid YAML: {e}")
        return
    if not isinstance(doc, dict):
        fail("manifest.yaml is not a mapping")
        return

    # extensions.Load() hard-fails on anything but this literal.
    if doc.get("version") != "v1alpha1":
        fail(f"manifest.yaml version must be 'v1alpha1', got {doc.get('version')!r}")
    else:
        ok("manifest.yaml declares version v1alpha1")

    meta = doc.get("metadata")
    if not isinstance(meta, dict):
        fail("manifest.yaml has no metadata mapping")
        return

    for field in ("name", "version", "author", "description"):
        value = meta.get(field)
        if not isinstance(value, str) or not value.strip():
            # A bare 0.1 parses as a float and fails to decode into a Go string.
            fail(f"manifest.yaml metadata.{field} must be a non-empty string, got {value!r}")

    constraint = (meta.get("compatibility") or {}).get("talos", {}).get("version")
    if constraint is None:
        ok("manifest.yaml declares no Talos constraint (allowed, but not advised)")
    elif not re.match(r"^\s*[<>=!~^]*\s*v?\d+\.\d+\.\d+", str(constraint)):
        fail(f"manifest.yaml Talos constraint {constraint!r} does not look parseable")
    else:
        ok(f"manifest.yaml Talos constraint {constraint!r} looks parseable")


def check_service_specs(members, shipped):
    specs = [
        p for p in shipped
        if p.startswith("usr/local/etc/containers/") and p.endswith(".yaml")
    ]
    if not specs:
        fail("no extension service spec under usr/local/etc/containers/")
        return

    for spec_path in specs:
        raw = members["rootfs/" + spec_path][1] or b""
        try:
            spec = yaml.safe_load(raw)
        except yaml.YAMLError as e:
            fail(f"{spec_path} is not valid YAML: {e}")
            continue
        if not isinstance(spec, dict):
            fail(f"{spec_path} is not a mapping")
            continue

        name = spec.get("name")
        if not isinstance(name, str) or not SERVICE_NAME_RE.match(name):
            fail(f"{spec_path} name {name!r} must match {SERVICE_NAME_RE.pattern}")
            continue

        container = spec.get("container") or {}
        entrypoint = container.get("entrypoint")
        if not isinstance(entrypoint, str) or not entrypoint:
            fail(f"{spec_path} container.entrypoint must be a non-empty string")

        restart = spec.get("restart")
        if restart not in RESTART_KINDS:
            fail(f"{spec_path} restart must be one of {sorted(RESTART_KINDS)}, got {restart!r}")

        # Talos requires exactly one key per depends entry.
        for i, dep in enumerate(spec.get("depends") or []):
            if not isinstance(dep, dict) or len(dep) != 1:
                fail(f"{spec_path} depends[{i}] must have exactly one key, got {dep!r}")
            elif next(iter(dep)) not in DEPENDS_KEYS:
                fail(f"{spec_path} depends[{i}] key {next(iter(dep))!r} is not one of {sorted(DEPENDS_KEYS)}")

        rootfs_dir = f"usr/local/lib/containers/{name}/"
        if not any(p.startswith(rootfs_dir) for p in shipped):
            fail(f"service {name!r} has no container rootfs at {rootfs_dir}")
            continue

        # The entrypoint is resolved inside the service's container rootfs.
        binary = rootfs_dir + entrypoint.lstrip("/") if isinstance(entrypoint, str) else None
        if binary and binary not in shipped:
            fail(f"service {name!r} entrypoint {entrypoint} is not present at {binary}")
        else:
            ok(f"service {name!r} is valid and its entrypoint is present")


def main():
    if len(sys.argv) != 2:
        print("usage: validate-extension.py IMAGE", file=sys.stderr)
        return 2
    image = sys.argv[1]

    members = load_image(image)
    if not members:
        fail("no files found in image layers")
        return report()

    roots = {p.split("/")[0] for p in members}
    expected_roots = {"manifest.yaml", "rootfs"}
    if roots != expected_roots:
        fail(f"image root must be exactly {sorted(expected_roots)}, got {sorted(roots)}")
    else:
        ok("image root holds exactly manifest.yaml and rootfs/")

    check_manifest(members)

    world_writable = []
    special = []
    outside = []
    shipped = []

    for path, (info, _) in sorted(members.items()):
        # Talos checks the mode of directories too, before it skips them.
        if info.mode & 0o002:
            world_writable.append(f"{path} (mode {info.mode:04o})")
        if info.isdir():
            continue
        # A hard link in a layer tar extracts to a regular file; Talos accepts it.
        if not (info.isfile() or info.issym() or info.islnk()):
            special.append(path)
            continue
        if path == "manifest.yaml":
            continue

        if not path.startswith("rootfs/"):
            outside.append(path)
            continue
        rel = path[len("rootfs/"):]
        shipped.append(rel)
        # Talos uses a plain strings.HasPrefix. This is deliberately stricter
        # (it will not accept "usr/locally/x"); do not "fix" it the other way.
        if not any(rel == a or rel.startswith(a + "/") for a in ALLOWED_PATHS):
            outside.append(rel)

    if special:
        fail(f"special files are not allowed in an extension: {special}")
    else:
        ok("no special files")

    if world_writable:
        fail(f"world-writable entries: {world_writable}")
    else:
        ok("no world-writable files or directories")

    if outside:
        fail(f"paths outside Talos's allowed-paths list: {outside}")
    else:
        ok(f"all {len(shipped)} shipped files are inside allowed paths")

    check_service_specs(members, shipped)

    return report()


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
