# syntax=docker/dockerfile:1

# ---------------------------------------------------------------------------
# Stage 1: FX3 firmware, built from source.
#
# ik1xpv/ExtIO_sddc is the VHF-capable lineage: driver/tuner_r82xx.c drives the
# R828D. It vendors the Cypress FX3 SDK 1.3.4 in SDK/, so the build needs only
# arm-none-eabi-gcc and a host gcc for the SDK's elf2img tool.
#
# See docs/LICENSING.md: the resulting image is a three-licence composite and
# carries a known GPL/Cypress-SLA conflict that was accepted deliberately.
# ---------------------------------------------------------------------------
FROM debian:bookworm-slim AS firmware-build

# Pinned by commit SHA; git verifies the checkout against it.
ARG EXTIO_SDDC_REF=331b35cd619bf4af20444d64dc35b13a474493e6

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        git \
        make \
        gcc \
        libc6-dev \
        gcc-arm-none-eabi \
        libnewlib-arm-none-eabi \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
RUN git init -q extio \
    && cd extio \
    && git remote add origin https://github.com/ik1xpv/ExtIO_sddc.git \
    && git fetch -q --depth 1 origin "${EXTIO_SDDC_REF}" \
    && git checkout -q FETCH_HEAD \
    && test "$(git rev-parse HEAD)" = "${EXTIO_SDDC_REF}"

WORKDIR /build/extio/SDDC_FX3
RUN make all && ls -l SDDC_FX3.img

# ---------------------------------------------------------------------------
# Stage 2: rx888-init, a static musl binary.
#
# libusb is built with --disable-udev so hotplug uses the netlink uevent socket
# directly. That works because Talos extension services run in the host network
# namespace; netlink uevents are network-namespace scoped.
# ---------------------------------------------------------------------------
FROM alpine:3.20 AS loader-build

ARG LIBUSB_VERSION=1.0.27
# This code is statically linked into a binary that runs privileged on every
# node, and GitHub release assets are mutable by the repo owner, so pin it.
ARG LIBUSB_SHA256=ffaa41d741a8a3bee244ac8e54a72ea05bf2879663c098c82fc5757853441575

RUN apk add --no-cache build-base curl linux-headers

WORKDIR /build
RUN curl -fsSL -o libusb.tar.bz2 \
        "https://github.com/libusb/libusb/releases/download/v${LIBUSB_VERSION}/libusb-${LIBUSB_VERSION}.tar.bz2" \
    && echo "${LIBUSB_SHA256}  libusb.tar.bz2" | sha256sum -c - \
    && tar xjf libusb.tar.bz2 \
    && cd "libusb-${LIBUSB_VERSION}" \
    && ./configure \
        --prefix=/opt/libusb \
        --enable-static \
        --disable-shared \
        --disable-udev \
    && make -j"$(nproc)" \
    && make install

COPY src/ /build/src/
# Deliberately NOT stripped: this is the one binary that cannot be debugged on a
# Talos node (no shell, no debugger), and a stripped static binary that faults
# yields nothing but a bare address in the kernel log. The symbols cost ~100 KB.
RUN gcc -std=gnu11 -O2 -Wall -Wextra -Werror \
        -Wformat=2 -Wshadow -Wvla -Wpointer-arith \
        -fstack-protector-strong -D_FORTIFY_SOURCE=2 \
        -static \
        -I/build/src \
        -I/opt/libusb/include/libusb-1.0 \
        -o /build/rx888-init \
        /build/src/rx888-init.c \
        /build/src/ezusb.c \
        /opt/libusb/lib/libusb-1.0.a \
    && ! ldd /build/rx888-init 2>/dev/null | grep -q '=>' \
    && /build/rx888-init --help

# ---------------------------------------------------------------------------
# Stage 3: assemble and normalise the extension tree.
#
# The rootfs is overlaid onto the host, so ownership and modes are pinned here
# rather than inherited from whatever the build context happened to have.
#
# Note imager does NOT enforce the world-writable or allowed-path rules: the
# checks exist behind Builder.ExtensionValidateContents, which nothing in the
# Talos tree ever sets. They are enforced by Image Factory and by the
# extensions-validator in siderolabs/extensions CI -- and by `make validate`.
# ---------------------------------------------------------------------------
FROM alpine:3.20 AS staging

COPY manifest.yaml /out/manifest.yaml
COPY extension/rx888.yaml /out/rootfs/usr/local/etc/containers/rx888.yaml
COPY extension/70-rx888.rules /out/rootfs/usr/lib/udev/rules.d/70-rx888.rules
COPY --from=loader-build /build/rx888-init /out/rootfs/usr/local/lib/containers/rx888/usr/bin/rx888-init
COPY --from=firmware-build /build/extio/SDDC_FX3/SDDC_FX3.img /out/rootfs/usr/local/lib/containers/rx888/usr/share/rx888/SDDC_FX3.img

RUN chown -R 0:0 /out \
    && find /out -type d -exec chmod 0755 {} + \
    && find /out -type f -exec chmod 0644 {} + \
    && chmod 0755 /out/rootfs/usr/local/lib/containers/rx888/usr/bin/rx888-init \
    && test -z "$(find /out -perm -o+w)"

# ---------------------------------------------------------------------------
# Stage 4: the extension service's container rootfs on its own.
#
# Not part of the published extension. This is exactly the filesystem Talos
# gives the service — same binary, same firmware path, no shell — so running it
# under docker against real hardware tests what actually gets deployed.
# ---------------------------------------------------------------------------
FROM scratch AS service-rootfs

COPY --from=staging /out/rootfs/usr/local/lib/containers/rx888/ /
ENTRYPOINT ["/usr/bin/rx888-init"]

# ---------------------------------------------------------------------------
# Stage 5: the extension image.
#
# The image root must contain exactly manifest.yaml and rootfs/. This one IS
# enforced unconditionally: extensions.Load() fails with `unexpected file %q`
# for anything else. Every rootfs path here is inside Talos's allowlist
# (/usr/local/** and /usr/lib/udev/rules.d).
#
# Kept last so that a plain `docker build .` builds the extension itself.
# ---------------------------------------------------------------------------
FROM scratch AS extension

COPY --from=staging /out/ /
