# talos-rx-888 — Talos system extension for the RX-888 / RX-888 MK2 SDR

REGISTRY      ?= ghcr.io
IMAGE_NAME    ?= ramielrowe/talos-rx-888
VERSION       ?= 0.1.0
IMAGE         := $(REGISTRY)/$(IMAGE_NAME):$(VERSION)

# Talos release whose imager builds the installer. Keep in step with the
# compatibility range declared in manifest.yaml.
TALOS_VERSION ?= v1.10.5
ARCH          ?= amd64

.PHONY: all image firmware loader staging service-rootfs validate push installer test-local watch-local clean help

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

validate: image ## Check the built image against Talos's extension contract
	python3 hack/validate-extension.py $(IMAGE)

push: image ## Push the extension image to the registry
	docker push $(IMAGE)

installer: ## Build a Talos installer image with the extension baked in
	mkdir -p _out
	docker run --rm -t -v $(PWD)/_out:/out \
		ghcr.io/siderolabs/imager:$(TALOS_VERSION) installer \
		--platform=metal --arch $(ARCH) \
		--system-extension-image $(IMAGE)
	@echo
	@echo "Installer written to _out/metal-$(ARCH)-installer.tar"
	@echo "Push it with: crane push _out/metal-$(ARCH)-installer.tar $(REGISTRY)/$(IMAGE_NAME)-installer:$(TALOS_VERSION)"

# Runs the real service rootfs against whatever RX-888 is attached to this
# machine: same binary, same default firmware path as on a Talos node.
#
# --net=host is required. libusb hotplug listens on a netlink uevent socket,
# which is network-namespace scoped -- which is also why this works as a Talos
# extension service, since those run in the host network namespace.
RUN_SERVICE = docker run --rm --privileged --net=host \
	-v /dev/bus/usb:/dev/bus/usb talos-rx-888-service

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
