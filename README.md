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
and **udev cannot run a firmware loader** — there is no shell or loader binary in
the host rootfs, and SELinux only permits `udev_t` to execute `udev_exec_t` and
`modprobe_exec_t`. A system extension is the supported way in.

Loading firmware on the host, rather than from the consuming pod, is also what
makes the device plugin work at all. See
[Why the firmware must be loaded before the pod](#why-the-firmware-must-be-loaded-before-the-pod).

## What it does

- Runs `rx888-init` as an extension service (`ext-rx888`), which loads FX3
  firmware at boot and on hotplug, for every RX-888 attached, idempotently.
- Ships a udev rule so the usbfs node is `0666` and an unprivileged pod can open
  it, and relaxes the mode itself as a fallback.
- Warns in its log if `usbcore.usbfs_memory_mb` is still too low for streaming.

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

Image Factory cannot be used: it only serves official Sidero extensions, and its
schematic has no field for a custom image. Build an installer with `imager`
instead.

### 2. Build an installer with the extension baked in

```sh
make installer VERSION=0.1.0 TALOS_VERSION=v1.10.5
crane push _out/metal-amd64-installer.tar ghcr.io/<you>/talos-rx-888-installer:v1.10.5
```

Then upgrade the node, or point `machine.install.image` at it for a new one:

```sh
talosctl upgrade -n <node> --image ghcr.io/<you>/talos-rx-888-installer:v1.10.5
```

### 3. Raise the usbfs buffer limit

**This step is required.** The kernel caps usbfs buffers at 16 MB, which is
about 120 ms of headroom at 64.8 Msps; past that `libusb_submit_transfer()`
returns `LIBUSB_ERROR_NO_MEM`.

```sh
talosctl patch mc -n <node> -p @examples/machineconfig-patch.yaml
```

That sets `machine.sysfs`, which Talos applies at runtime with no reboot and
reapplies on every config change. On Talos ≥ 1.14 the same thing is expressed as
a `SysfsConfig` document — both forms are in the example file.

Do not try to do this with a kernel argument: `machine.install.extraKernelArgs`
is deprecated, only applies at install/upgrade, and is silently ignored on
UKI/systemd-boot installs.

### 4. Check it came up

```sh
talosctl get extensions                 # rx888 listed
talosctl service ext-rx888              # Running
talosctl logs ext-rx888                 # firmware loaded, bus/dev, link speed
talosctl get kernelparamstatuses        # usbfs_memory_mb applied
```

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

## Licensing

The built image is **not** under a single licence: the packaging is MIT, the
`rx888-init` binary is GPL-2.0-or-later, and the FX3 firmware is a composite of
MIT application code, the proprietary Cypress SDK and ThreadX — including a
known, deliberately accepted GPL/Cypress conflict.

Read [docs/LICENSING.md](docs/LICENSING.md) before redistributing it.
