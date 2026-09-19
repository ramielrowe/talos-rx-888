# talos-rx-888

A [Talos Linux](https://www.talos.dev/) system extension that makes an
**RX-888 / RX-888 MK2** SDR usable from a Kubernetes pod.

## Why this exists

The RX-888 has no boot flash for its Cypress FX3. At every power-up it
enumerates as the bare FX3 ROM bootloader (`04b4:00f3`, USB 2.0 high speed), and
a host has to download firmware into its RAM before it is a radio at all. Only
then does it re-enumerate as `04b4:00f1`, normally at SuperSpeed.

On an ordinary distribution that is a udev rule plus a systemd unit. On Talos it
is not: there is no package manager, no systemd units, the rootfs is immutable,
and **udev cannot run a firmware loader** — the host rootfs has no shell and no
loader binary, and an extension can only place executables under `/usr/local`,
which nothing in udev's path references. A system extension is the supported way
in.

Loading firmware on the host, rather than from the consuming pod, is also what
makes the device plugin work at all. See
[Why the firmware must be loaded before the pod](#why-the-firmware-must-be-loaded-before-the-pod).

## What it does

- Runs `rx888-init` as an extension service (`ext-rx888`), which loads FX3
  firmware at boot and on hotplug, for every RX-888 attached, idempotently.
- Ships a udev rule so the usbfs node is `0666` and an unprivileged pod can open
  it, and relaxes the mode itself as a fallback.
- Warns in its log if `usbcore.usbfs_memory_mb` is too low for streaming — though
  the installer bakes a working value into the kernel command line, so this is a
  backstop rather than a step you have to remember.

It does **not** run any streaming or DSP software: that stays in your pods.

## Requirements

- Talos **≥ v1.10**, amd64.
- A USB 3 port. The MK2 needs SuperSpeed to stream; upstream guidance is a
  direct port rather than a hub.
- [generic-device-plugin](https://github.com/squat/generic-device-plugin), if you
  want to hand the device to pods by resource request. Installing it is out of
  scope here; `examples/generic-device-plugin.yaml` shows the configuration this
  extension expects.

## Install

### 1. Build and push the extension

```sh
make image VERSION=0.1.0
make push  VERSION=0.1.0
```

The hosted Image Factory (`factory.talos.dev`) cannot be used: its schematic only
has `customization.systemExtensions.officialExtensions`, with no field for a
custom image. You need an installer built by `imager`. (A self-hosted
image-factory can be pointed at your own extensions source, which is a larger
undertaking than this.)

Note `make installer` depends on `push`. imager pulls the extension over the
network with crane and never reads your local docker daemon, so an image that
exists only locally is invisible to it.

### 2. Get an installer image

The installer bakes a specific Talos release together with this extension.
It is **not** forward-compatible: a new Talos release needs a new installer.

**Use a published one.** Tagged releases publish
`ghcr.io/<you>/talos-rx-888-installer:<talos>-<ext>`, e.g.
`:v1.10.5-0.1.0`, for every Talos release in the `default` list in the `plan`
job of `.github/workflows/build.yml` (currently v1.10.5 and v1.12.11).

**Or cut one on demand.** To cover a new Talos release without tagging a new
extension version, run the `build` workflow manually — Actions → build → Run
workflow — with:

| input | example | meaning |
|---|---|---|
| `talos_version` | `v1.12.11` | build only this release; blank builds them all |
| `extension_version` | `0.1.0` | the already-published extension to bake in |

The extension version must already exist in the registry: the installer build
resolves it to a digest before imager runs, rather than rebuilding it.

**Or build and publish your own:**

```sh
make push-installer VERSION=0.1.0 TALOS_VERSION=v1.10.5
```

That resolves the extension to a digest — so the installer records exactly which
build went into it, and re-tagging cannot change it afterwards — runs imager, and
pushes the result. It also passes `--base-installer-image`, which imager has no
default for: omit it and the build fails at the last step with
`parsing reference ""`. The image was renamed `siderolabs/installer` →
`siderolabs/installer-base` in Talos 1.10; override `BASE_INSTALLER` for
anything older. For a local build with no push, use
`make installer` and find the artifact at `_out/installer-amd64.tar`. (imager
names it `installer-<arch>.tar`; it does not include the platform, despite what
the `--platform` flag suggests.)

**Package visibility is separate from repository visibility.** Making the repo
public does *not* publish its GHCR packages: a package first pushed by CI's
`GITHUB_TOKEN` is created private regardless. Flip both
`talos-rx-888` and `talos-rx-888-installer` to public under the repository's
Packages settings after the first publish.

This matters far more for the installer than for the extension:

- **The extension** can stay private. imager's keychain is
  `MultiKeychain(DefaultKeychain, github.Keychain, google.Keychain)`, and
  `github.Keychain` authenticates ghcr.io from `$GITHUB_TOKEN`, which the
  `installer-remote` target forwards into the container. CI works
  unattended; locally, `export GITHUB_TOKEN=<PAT with read:packages>` or
  `docker login ghcr.io` (the Makefile picks up either).
- **The installer cannot**, in practice. A Talos node pulls
  `machine.install.image` *itself*, during install and upgrade, with no
  GitHub credentials anywhere. A private installer package means configuring
  `machine.registries` with a pull secret on every node, before it can ever
  reach the image. Publish this one.

Then upgrade the node, or point `machine.install.image` at it for a new one:

```sh
talosctl upgrade -n <node> --image ghcr.io/<you>/talos-rx-888-installer:v1.10.5-0.1.0
```

### 3. usbfs buffer limit — already handled

Nothing to do. The installer bakes `usbcore.usbfs_memory_mb=1000` into the
kernel command line, so a node boots ready to stream.

This matters because the kernel default is 16 MB, which is roughly 120 ms of
headroom at 64.8 Msps; past that `libusb_submit_transfer()` returns
`LIBUSB_ERROR_NO_MEM`. `rx888-init` logs a warning at startup if the value is
ever below its threshold, so a node that somehow missed it says so in
`talosctl logs ext-rx888`.

Build with a different value, or none at all:

```sh
make push-installer USBFS_MEMORY_MB=2000    # or USBFS_MEMORY_MB= to omit it
```

To change it on a node you already installed, without rebuilding, patch the
machine config — `/sys/module/usbcore/parameters/usbfs_memory_mb` is writable at
runtime and takes precedence over the boot-time value:

```sh
talosctl patch mc -n <node> -p @examples/machineconfig-patch.yaml
```

That example sets `machine.sysfs`, which works on every Talos release this
extension supports. On Talos >= 1.14 the same thing is expressed as a
`SysfsConfig` document; both forms are in the file.

Note this only works because we build the installer ourselves.
`machine.install.extraKernelArgs` is deprecated, applies only at
install/upgrade, and is silently ignored on UKI/systemd-boot — which is exactly
what an imager-built installer produces.

### 4. Check it came up

```sh
talosctl get extensions                 # rx888 listed
talosctl service ext-rx888              # Running
talosctl logs ext-rx888                 # firmware loaded, bus/dev, link speed
talosctl read /proc/cmdline             # usbcore.usbfs_memory_mb=1000 present
```

(`talosctl get kernelparamstatuses` is the place to look instead if you set the
value through machine config rather than taking the baked-in one.)

A healthy log looks like:

```
info  rx888-init: usbfs_memory_mb is 1000
info  rx888-init: watching for hotplug events
info  rx888-init: bootloader at bus 10 device 3, loading /usr/share/rx888/SDDC_FX3.img
info  rx888-init: firmware handed to bus 10 device 3, waiting for it to re-enumerate as 04b4:00f1
info  rx888-init: RX-888 running firmware at bus 10 device 3, link SuperSpeed (5 Gbps)
```

### 5. Expose it to pods

```sh
kubectl apply -f examples/generic-device-plugin.yaml
kubectl apply -f examples/test-pod.yaml
kubectl logs rx888-smoke-test
```

Pods then request the device by resource name:

```yaml
resources:
  limits:
    devic.es/rx888: "1"
```

**One radio per node**, as configured. generic-device-plugin collects every
device matching a group into a *single* advertised resource, so with two radios
attached the node still advertises `devic.es/rx888: 1` and the one pod that gets
it receives both device nodes. `count: 2` does not help — it creates two units
that each contain both radios. To schedule two radios independently, give each
its own group keyed on `serial:` (the SDDC firmware reports one). `rx888-init`
itself loads firmware into every attached radio regardless.

## Why the firmware must be loaded before the pod

generic-device-plugin derives a device's identity from a SHA-1 of its
`/dev/bus/usb/BBB/DDD` path. A device that loads firmware detaches and comes back
with a different product ID and device number, which means:

- the running pod keeps a stale char-device node pointing at a dead minor;
- the device cgroup still only permits the old minor;
- on container restart the kubelet refuses outright — *previously allocated
  devices are no longer healthy* — so the pod must be **deleted and recreated**,
  not restarted.

So the plugin is configured to match `04b4:00f1` **only**, never the `00f3`
bootloader, and this extension makes sure the device has already reached `00f1`
before the plugin ever advertises it.

The same limitation applies whenever the radio is power-cycled: the FX3 loses
its RAM-resident firmware, the extension reloads it within a few seconds, and
any pod that was holding the old device node needs recreating.

## Behaviour notes

- **Firmware does not persist.** The FX3 has no flash, so unplugging or
  power-cycling the radio always returns it to the bootloader. A warm host
  reboot usually leaves it running, since port power typically survives; the
  service handles both cases.
- **Already-programmed devices are left alone.** `rx888-init` only ever acts on
  `04b4:00f3`. Re-flashing a running FX3 would require a `RESETFX3` or
  `USBDEVFS_RESET` first, and there is no reason to go there.
- **The udev rule is what really sets permissions.** It uses `MODE:="0666"` —
  the `:=` locks the value, and the `70-` prefix sorts after systemd's own
  `50-udev-default.rules`, which would otherwise set `0664`. `rx888-init` also
  re-asserts `0666` on each pass as a fallback, because a plain chmod races udev
  and loses.
  - A consequence of `:=`: if you later try to tighten the mode with
    `machine.udev.rules`, it will be **silently ignored**. Talos writes those to
    `99-talos.rules`, which sorts after ours, and `:=` forbids later changes.
    Change `extension/70-rx888.rules` and rebuild instead.
  - Extension squashfs layers are built with `mksquashfs -all-root` and carry no
    SELinux labels. This is fine today because Talos boots SELinux **permissive**
    unless you pass `enforcing=1`. If you run enforcing, verify the rule is still
    being applied — `talosctl logs ext-rx888` will report relaxing the node mode
    on every arrival if udev has stopped doing it.
- **One consumer at a time.** The RX-888 has a single bulk interface; a second
  process gets `LIBUSB_ERROR_BUSY`.
- **No usbmon.** Talos kernels are built without `CONFIG_USB_MON`, so on-node USB
  capture is not available. Diagnose from `talosctl logs ext-rx888`.

## Development

```sh
make loader        # build just rx888-init
make firmware      # build just the FX3 firmware
make image         # build the extension image
make validate      # check the image against Talos's extension contract
make test-local    # load firmware into an RX-888 attached to THIS machine
```

`make test-local` runs the real loader against real hardware over
`/dev/bus/usb`, with `--net=host` because libusb hotplug listens on a netlink
uevent socket, which is network-namespace scoped. (That is also why the service
works on Talos: extension services run in the host network namespace.)

## Cutting a release

The extension version lives in three places, and only two of them are checked:
`metadata.version` in `manifest.yaml` (what a node reports in
`talosctl get extensions`), the `VERSION` default in the `Makefile`, and the git
tag. Bump the first two, then tag:

```sh
make check-version VERSION=0.2.0    # fails until the manifest is bumped
git tag -a v0.2.0 -m "..." && git push origin v0.2.0
```

CI runs the same check on every build — against the Makefile default always,
and against the tag on a tag push — before the firmware build, so a release cut
without bumping the manifest fails in seconds rather than shipping an extension
that reports the previous version forever.

## Licensing

The built image is **not** under a single licence: the packaging is MIT, the
`rx888-init` binary is GPL-2.0-or-later, and the FX3 firmware is a composite of
MIT application code, the proprietary Cypress SDK and ThreadX — including a
known, deliberately accepted GPL/Cypress conflict.

Read [docs/LICENSING.md](docs/LICENSING.md) before redistributing it.
