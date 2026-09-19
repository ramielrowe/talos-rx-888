# Licensing

The extension image is not under a single licence. Read this before
redistributing it.

## What is in the image, and under what terms

| Component | Where it comes from | Licence |
|---|---|---|
| Packaging: Dockerfile, Makefile, manifest, service spec, udev rule, examples | this repo | MIT (see `LICENSE`) |
| `rx888-init` binary | `src/rx888-init.c` + vendored `src/ezusb.c` | **GPL-2.0-or-later** |
| `SDDC_FX3.img` firmware | built from `ik1xpv/ExtIO_sddc` | composite — see below |

## rx888-init is GPL-2.0-or-later

`src/ezusb.c` and `src/ezusb.h` are taken verbatim from libusb's `examples/`
directory. They are the only small, self-contained, libusb-only implementation
of the Cypress FX3 RAM download protocol, and they carry
`SPDX-License-Identifier: GPL-2.0-or-later`. (Note the `fxload` packaged by
Debian and friends is the 2008 linux-hotplug version, which has no FX3 support
at all — this code is not interchangeable with it.)

`rx888-init.c` links them, so the resulting binary is GPL-2.0-or-later. The
complete corresponding source for that binary is in `src/`, and the full licence
text is in `src/LICENSE.GPL-2.0`. Shipping the binary inside the extension image
is a distribution of GPL software: keep this repository available alongside any
image you publish.

## The firmware image is a three-licence composite

`SDDC_FX3.img` is built from `ik1xpv/ExtIO_sddc` at the commit pinned in the
`Dockerfile`. The resulting binary combines:

- **MIT** — the SDDC application code (Copyright (c) 2017-2020 Oscar Steila,
  IK1XPV), including the radio and driver sources.
- **Cypress Software License Agreement** (proprietary) — the vendored EZ-USB FX3
  SDK 1.3.4 under `SDK/`: `fw_lib/`, `fw_build/`, `cyfxtx.c`,
  `cyfx_gcc_startup.S` and the build makefiles. Licence text is in the upstream
  repository at `SDK/license/license.txt`.
- **ThreadX RTOS** (Express Logic, proprietary) — embedded in the SDK's `.a`
  libraries and sublicensed through the Cypress SLA.

The Cypress SLA §1.3 grants a royalty-free licence to reproduce, sublicense and
distribute the firmware **in object code form only, with the applicable Licensee
Product**. Distributing the built `.img` inside a container image intended to
drive Cypress silicon is the case that grant contemplates, and it is what
ka9q-radio, libsddc, rx888_stream and the Debian packages all already do.

### The known conflict, accepted deliberately

`SDDC_FX3/driver/tuner_r82xx.c` — the R828D VHF tuner driver — is **GPL**, and
it is statically linked against the proprietary Cypress SDK and ThreadX. Those
terms cannot both be satisfied. This is a real, unresolved conflict, not a
theoretical one: `ringof/rx888-firmware` deleted that file specifically to
escape it, and ka9q-radio now ships the resulting MIT-licensed build by default.

This project uses the `ik1xpv` lineage anyway, as a deliberate choice, because
removing that file also removes VHF tuner support. If you would rather have the
clean licensing than the VHF coverage, you do not need to rebuild anything —
see below.

## Using a different firmware image

Nothing here is pinned to the `ik1xpv` firmware. The obvious alternative is
`ringof/rx888-firmware` (MIT, HF-only, no GPL/Cypress conflict), published as a
release asset and as `W1EUJ_0.1.0_FX3.img`, which is what ka9q-radio now uses by
default.

There are two ways to get there.

**Rebuild the extension** (the straightforward path). Point the `firmware-build`
stage at the source you want — for `ringof/rx888-firmware` that is a different
repository and build directory, so edit the stage rather than just the
`EXTIO_SDDC_REF` build argument — and rebuild. Everything else is unchanged.

**Override at runtime**, if the replacement image is already present on the host
(for example carried by a second extension). `rx888-init` reads
`$RX888_FIRMWARE`, and an `ExtensionServiceConfig` can inject it:

```yaml
apiVersion: v1alpha1
kind: ExtensionServiceConfig
name: rx888
environment:
  - RX888_FIRMWARE=/usr/local/lib/containers/rx888-alt/custom.img
```

Applying that restarts `ext-rx888`. Note this cannot carry the image itself:
`configFiles.content` is text in the machine config, so it is not a practical
way to deliver a 140 KB binary — the file has to reach the host some other way.

## Upstream references

- libusb `examples/` (`ezusb.c`): https://github.com/libusb/libusb/tree/master/examples
- `ik1xpv/ExtIO_sddc`: https://github.com/ik1xpv/ExtIO_sddc
- Cypress SLA text: https://github.com/ik1xpv/ExtIO_sddc/blob/master/SDK/license/license.txt
- `ringof/rx888-firmware` and its licence analysis:
  https://github.com/ringof/rx888-firmware/blob/main/docs/LICENSE_ANALYSIS.md
