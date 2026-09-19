# talos-rx-888 — Talos system extension for the RX-888 / RX-888 MK2 SDR

REGISTRY      ?= ghcr.io
IMAGE_NAME    ?= ramielrowe/talos-rx-888
VERSION       ?= 0.1.0
IMAGE_REPO    := $(REGISTRY)/$(IMAGE_NAME)
IMAGE         := $(IMAGE_REPO):$(VERSION)

# The installer bakes in BOTH a Talos release and this extension, so the tag
# names both. An installer is not forward-compatible with other Talos releases.
INSTALLER_REPO ?= $(IMAGE_REPO)-installer
# Deferred (=, not :=) on purpose: TALOS_VERSION is defined below this line.
INSTALLER_IMAGE = $(INSTALLER_REPO):$(TALOS_VERSION)-$(VERSION)

# Talos release whose imager builds the installer. Keep in step with the
# compatibility range declared in manifest.yaml.
TALOS_VERSION ?= v1.10.5
ARCH          ?= amd64

# imager has no default for this and fails with `parsing reference ""` if it is
# omitted. Renamed from siderolabs/installer to siderolabs/installer-base as of
# Talos 1.10; override if you target something older.
BASE_INSTALLER ?= ghcr.io/siderolabs/installer-base:$(TALOS_VERSION)

.PHONY: all image firmware loader staging service-rootfs validate push \
        check-version installer installer-remote push-installer test-local \
        watch-local clean help

# crane, run via docker so there is nothing to install. Override with
# CRANE=crane CRANE_OUT=_out to use a local binary instead.
#
# --user 0 and DOCKER_CONFIG are both load-bearing. The crane image runs as uid
# 65532 with no HOME set, so it never looks in /root/.docker, and a docker
# config written by `docker login` is mode 0600 owned by someone else -- so
# mounting the file alone yields "No matching credentials were found".
# Mounting the directory, naming it with DOCKER_CONFIG, and running as root
# fixes the path and the permissions together.
DOCKER_CFG_DIR := $(HOME)/.docker
CRANE ?= docker run --rm --user 0 \
	$(if $(wildcard $(DOCKER_CFG_DIR)/config.json),-v $(DOCKER_CFG_DIR):/dockercfg:ro -e DOCKER_CONFIG=/dockercfg,) \
	-v $(PWD)/_out:/out \
	gcr.io/go-containerregistry/crane:latest
CRANE_OUT ?= /out

# Extra flags for imager, e.g. IMAGER_ARGS='--extra-kernel-arg console=ttyS0'
# or --insecure when pulling the extension from a plain-HTTP registry.
IMAGER_ARGS ?=

all: image ## Build the extension image (default)

image: ## Build the extension image
	docker build --target extension -t $(IMAGE) .

firmware: ## Build only the FX3 firmware stage
	docker build --target firmware-build -t talos-rx-888-firmware .

loader: ## Build only the rx888-init stage
	docker build --target loader-build -t talos-rx-888-loader .

staging: ## Build the staging stage (extension tree plus a shell, for testing)
	docker build --target staging -t talos-rx-888-staging .

service-rootfs: ## Build the extension service's container rootfs on its own
	docker build --target service-rootfs -t talos-rx-888-service .

check-version: ## Verify manifest.yaml metadata.version matches VERSION
	python3 hack/check-version.py $(VERSION)

validate: image ## Check the built image against Talos's extension contract
	python3 hack/validate-extension.py $(IMAGE)

push: image ## Push the extension image to the registry
	docker push $(IMAGE)

# Depends on `push`: imager pulls the extension over the network with crane, it
# never reads the local docker daemon, so an image that has only been built
# locally is invisible to it.
installer: push ## Build a Talos installer locally (publishes the extension first)
	@$(MAKE) --no-print-directory installer-remote

# Builds the installer from an already-published extension. This is what CI
# uses, where an earlier job has already pushed it.
#
# The extension is resolved to a digest first, so the installer records exactly
# which build went into it and a moving tag cannot change that later.
#
# GITHUB_TOKEN is forwarded when set: imager's keychain is
# MultiKeychain(DefaultKeychain, github.Keychain, google.Keychain), and
# github.Keychain authenticates ghcr.io from that variable -- which is how a
# PRIVATE extension package can be pulled without making it public.
installer-remote: ## Build a Talos installer from the published extension
	mkdir -p _out
	@set -eu; \
	digest=$$($(CRANE) digest $(IMAGE)); \
	echo "extension $(IMAGE) -> $$digest"; \
	docker run --rm -v $(PWD)/_out:/out \
		$(if $(GITHUB_TOKEN),-e GITHUB_TOKEN,) \
		ghcr.io/siderolabs/imager:$(TALOS_VERSION) installer \
		--platform=metal --arch $(ARCH) \
		--base-installer-image $(BASE_INSTALLER) \
		$(IMAGER_ARGS) \
		--system-extension-image $(IMAGE_REPO)@$$digest
	@echo
	@echo "Installer written to _out/installer-$(ARCH).tar"

push-installer: installer-remote ## Push the installer image to the registry
	$(CRANE) push $(CRANE_OUT)/installer-$(ARCH).tar $(INSTALLER_IMAGE)
	@echo
	@echo "Published $(INSTALLER_IMAGE)"
	@echo "Use it with: talosctl upgrade -n <node> --image $(INSTALLER_IMAGE)"

# Runs the real service rootfs against whatever RX-888 is attached to this
# machine: same binary, same default firmware path as on a Talos node.
#
# --net=host is required. libusb hotplug listens on a netlink uevent socket,
# which is network-namespace scoped -- which is also why this works as a Talos
# extension service, since those run in the host network namespace.
RUN_SERVICE = docker run --rm --privileged --net=host \
	-v /dev/bus/usb:/dev/bus/usb talos-rx-888-service --min-usbfs-mb 256

test-local: service-rootfs ## Load firmware into an RX-888 attached to this machine
	$(RUN_SERVICE) --once
	@echo
	@lsusb -d 04b4:00f1 || (echo "no RX-888 running firmware found"; exit 1)

watch-local: service-rootfs ## Run the watcher in the foreground against local hardware
	$(RUN_SERVICE)

clean: ## Remove build outputs
	rm -rf _out

help: ## List targets
	@grep -hE '^[a-zA-Z_-]+:.*?## ' $(MAKEFILE_LIST) \
		| awk 'BEGIN {FS = ":.*?## "}; {printf "  \033[36m%-12s\033[0m %s\n", $$1, $$2}'
