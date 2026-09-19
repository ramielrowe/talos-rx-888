/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * rx888-init — RX-888 / RX-888 MK2 FX3 firmware loader and hotplug watcher.
 *
 * The RX-888 has no boot flash for its Cypress FX3: at every power-up it comes
 * up as the bare FX3 ROM bootloader (04b4:00f3) and the host must download
 * firmware into its RAM before it is an SDR at all. Once loaded it detaches and
 * re-enumerates as 04b4:00f1, normally at SuperSpeed.
 *
 * This runs as a Talos extension service. It watches for bootloader devices,
 * loads firmware into each one, and relaxes the mode on the resulting device
 * node so an unprivileged pod can open it. It never exits.
 *
 * This file links ezusb.c (GPL-2.0-or-later), so it carries that licence too.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "libusb.h"
#include "ezusb.h"

#define RX888_VID       0x04b4
#define RX888_PID_BOOT  0x00f3  /* FX3 ROM bootloader, firmware not yet loaded */
#define RX888_PID_APP   0x00f1  /* SDDC firmware running */

#define USBFS_MEMORY_MB_PATH "/sys/module/usbcore/parameters/usbfs_memory_mb"

/* Bounded bookkeeping tables. A host with more than this many RX-888s attached
 * is well outside anything we need to support gracefully. */
#define MAX_TRACKED 16

/* Exponential backoff between retries for a device that fails to load. */
#define BACKOFF_MIN_SEC 2
#define BACKOFF_MAX_SEC 60

/* How long a --once run waits for a freshly programmed device to come back. */
#define REENUMERATE_TIMEOUT_MS 15000

#define FIRMWARE_ENV "RX888_FIRMWARE"

static const char *opt_firmware = "/usr/share/rx888/SDDC_FX3.img";
static long opt_min_usbfs_mb = 256;
static long opt_poll_interval = 5;
static bool opt_once = false;

static volatile sig_atomic_t stop_requested = 0;
static volatile sig_atomic_t rescan_requested = 1;

/* ------------------------------------------------------------------ logging */

static void ts_prefix(char *buf, size_t len)
{
	struct timespec now;
	struct tm tm;

	if (clock_gettime(CLOCK_REALTIME, &now) != 0 ||
	    gmtime_r(&now.tv_sec, &tm) == NULL) {
		snprintf(buf, len, "-");
		return;
	}
	snprintf(buf, len, "%04d-%02d-%02dT%02d:%02d:%02dZ",
		 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
		 tm.tm_hour, tm.tm_min, tm.tm_sec);
}

static void log_msg(const char *level, const char *fmt, va_list ap)
{
	char ts[32];

	ts_prefix(ts, sizeof(ts));
	fprintf(stderr, "%s %-5s rx888-init: ", ts, level);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
}

static void log_info(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	log_msg("info", fmt, ap);
	va_end(ap);
}

static void log_warn(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	log_msg("warn", fmt, ap);
	va_end(ap);
}

static void log_err(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	log_msg("error", fmt, ap);
	va_end(ap);
}

/* ezusb.c declares this extern and calls it for its own diagnostics. It appends
 * its own newlines, so strip a trailing one to keep our log one-line-per-event. */
void logerror(const char *format, ...)
{
	char buf[512];
	size_t len;
	va_list ap;

	va_start(ap, format);
	vsnprintf(buf, sizeof(buf), format, ap);
	va_end(ap);

	len = strlen(buf);
	while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
		buf[--len] = '\0';

	if (len > 0)
		log_info("ezusb: %s", buf);
}

/* ------------------------------------------------------------- usbfs warning */

/*
 * Raising usbcore.usbfs_memory_mb is Talos's job, not ours: the machine config
 * carries it (SysfsConfig on >= 1.14, machine.sysfs on 1.10-1.13) and Talos
 * applies it at runtime. We only read the value so that an operator who skipped
 * that step finds out from the service log rather than from mysterious
 * LIBUSB_ERROR_NO_MEM at high sample rates.
 */
static void check_usbfs_memory(void)
{
	char buf[64];
	long value;
	FILE *f;

	f = fopen(USBFS_MEMORY_MB_PATH, "r");
	if (f == NULL) {
		log_warn("cannot read %s: %s", USBFS_MEMORY_MB_PATH, strerror(errno));
		return;
	}
	if (fgets(buf, sizeof(buf), f) == NULL) {
		fclose(f);
		log_warn("cannot read %s: empty", USBFS_MEMORY_MB_PATH);
		return;
	}
	fclose(f);

	value = strtol(buf, NULL, 10);
	if (value == 0) {
		log_info("usbfs_memory_mb is 0 (unlimited)");
		return;
	}
	if (value < opt_min_usbfs_mb) {
		log_warn("usbfs_memory_mb is %ld, below the %ld MB minimum for RX-888 streaming",
			 value, opt_min_usbfs_mb);
		log_warn("high sample rates will fail with LIBUSB_ERROR_NO_MEM until this is raised");
		log_warn("set it in the Talos machine config: SysfsConfig params "
			 "module.usbcore.parameters.usbfs_memory_mb: \"1000\" (Talos >= 1.14),");
		log_warn("or machine.sysfs module.usbcore.parameters.usbfs_memory_mb: \"1000\" (Talos 1.10-1.13)");
		return;
	}
	log_info("usbfs_memory_mb is %ld", value);
}

/* --------------------------------------------------------------- device nodes */

static void usb_node_path(char *buf, size_t len, uint8_t bus, uint8_t addr)
{
	snprintf(buf, len, "/dev/bus/usb/%03u/%03u", bus, addr);
}

/*
 * Belt-and-braces alongside the udev rule the extension ships. If the rule fired
 * this is a no-op; if it did not (rules not yet reloaded, or a build that ignores
 * extension-shipped rules) this still leaves a node a non-root pod can open.
 * Never fatal.
 *
 * Re-asserted on every scan rather than once per arrival, because udev races us:
 * systemd's own 50-udev-default.rules carries
 *   SUBSYSTEM=="usb", ENV{DEVTYPE}=="usb_device", MODE="0664"
 * and if it processes the add event after we have chmod'ed, its mode is the one
 * that sticks. The shipped rule uses MODE:= so that udev itself settles on 0666
 * and this never fires, but when that rule is absent this keeps the node usable.
 *
 * `logged` points at per-device state so a repeatedly-reverted node is reported
 * once per arrival instead of on every pass. Pass NULL to always log.
 */
static void relax_permissions(uint8_t bus, uint8_t addr, bool *logged)
{
	char path[64];
	struct stat st;

	usb_node_path(path, sizeof(path), bus, addr);

	if (stat(path, &st) != 0) {
		log_warn("cannot stat %s: %s", path, strerror(errno));
		return;
	}
	if ((st.st_mode & 0666) == 0666)
		return;

	if (chmod(path, 0666) != 0) {
		log_warn("cannot chmod %s to 0666: %s", path, strerror(errno));
		return;
	}

	if (logged == NULL || !*logged) {
		log_info("relaxed %s from mode %04o to 0666", path, st.st_mode & 07777);
		if (logged != NULL)
			*logged = true;
	}
}

static const char *speed_name(int speed)
{
	switch (speed) {
	case LIBUSB_SPEED_LOW:        return "low (1.5 Mbps)";
	case LIBUSB_SPEED_FULL:       return "full (12 Mbps)";
	case LIBUSB_SPEED_HIGH:       return "high (480 Mbps)";
	case LIBUSB_SPEED_SUPER:      return "SuperSpeed (5 Gbps)";
	case LIBUSB_SPEED_SUPER_PLUS: return "SuperSpeed+ (10 Gbps)";
	default:                      return "unknown";
	}
}

/* ------------------------------------------------------------------- backoff */

struct backoff_entry {
	uint16_t key;       /* bus << 8 | address; 0 means the slot is free */
	unsigned failures;
	time_t next_attempt;
};

static struct backoff_entry backoff[MAX_TRACKED];

static uint16_t dev_key(uint8_t bus, uint8_t addr)
{
	/* Both bus and address are 1-based on Linux, so 0 is never a valid key. */
	return (uint16_t)((uint16_t)bus << 8 | addr);
}

static struct backoff_entry *backoff_find(uint16_t key, bool create)
{
	struct backoff_entry *free_slot = NULL;
	size_t i;

	for (i = 0; i < MAX_TRACKED; i++) {
		if (backoff[i].key == key)
			return &backoff[i];
		if (free_slot == NULL && backoff[i].key == 0)
			free_slot = &backoff[i];
	}
	if (!create || free_slot == NULL)
		return NULL;

	free_slot->key = key;
	free_slot->failures = 0;
	free_slot->next_attempt = 0;
	return free_slot;
}

static bool backoff_ready(uint16_t key, time_t now)
{
	struct backoff_entry *e = backoff_find(key, false);

	return e == NULL || now >= e->next_attempt;
}

static void backoff_record_failure(uint16_t key, time_t now)
{
	struct backoff_entry *e = backoff_find(key, true);
	long delay = BACKOFF_MIN_SEC;
	unsigned i;

	if (e == NULL)
		return;

	e->failures++;
	for (i = 1; i < e->failures && delay < BACKOFF_MAX_SEC; i++)
		delay *= 2;
	if (delay > BACKOFF_MAX_SEC)
		delay = BACKOFF_MAX_SEC;

	e->next_attempt = now + delay;
	log_warn("retrying that device in %lds (attempt %u failed)", delay, e->failures);
}

static void backoff_clear(uint16_t key)
{
	struct backoff_entry *e = backoff_find(key, false);

	if (e != NULL)
		e->key = 0;
}

/* --------------------------------------------------------------- app tracking */

/* Devices already seen running firmware, so we log and chmod once per arrival
 * rather than on every poll. */
static uint16_t app_seen[MAX_TRACKED];
static bool app_relax_logged[MAX_TRACKED];

/*
 * Find (or claim) the slot tracking a device running firmware. Returns its index
 * and sets *is_new on the first sighting, or -1 if the table is full.
 */
static int app_slot(uint16_t key, bool *is_new)
{
	size_t i;

	for (i = 0; i < MAX_TRACKED; i++) {
		if (app_seen[i] == key) {
			*is_new = false;
			return (int)i;
		}
	}

	for (i = 0; i < MAX_TRACKED; i++) {
		if (app_seen[i] == 0) {
			app_seen[i] = key;
			app_relax_logged[i] = false;
			*is_new = true;
			return (int)i;
		}
	}

	*is_new = true;
	return -1;
}

static unsigned app_count(void)
{
	unsigned n = 0;
	size_t i;

	for (i = 0; i < MAX_TRACKED; i++)
		if (app_seen[i] != 0)
			n++;
	return n;
}

static void app_forget_absent(const uint16_t *present, size_t n_present)
{
	size_t i, j;

	for (i = 0; i < MAX_TRACKED; i++) {
		bool still_here = false;

		if (app_seen[i] == 0)
			continue;
		for (j = 0; j < n_present; j++) {
			if (present[j] == app_seen[i]) {
				still_here = true;
				break;
			}
		}
		if (!still_here) {
			app_seen[i] = 0;
			app_relax_logged[i] = false;
		}
	}
}

/* ------------------------------------------------------------ firmware upload */

/*
 * Returns true if firmware was handed to the device. The device then detaches
 * and re-enumerates as RX888_PID_APP, which the next scan picks up.
 */
static bool load_firmware(libusb_device *dev, uint8_t bus, uint8_t addr)
{
	libusb_device_handle *handle = NULL;
	int status;
	int attempt;

	log_info("bootloader at bus %u device %u, loading %s", bus, addr, opt_firmware);

	status = libusb_open(dev, &handle);
	if (status != LIBUSB_SUCCESS) {
		log_err("libusb_open on bus %u device %u failed: %s",
			bus, addr, libusb_error_name(status));
		return false;
	}

	libusb_set_auto_detach_kernel_driver(handle, 1);

	/*
	 * The kernel tears down a previous usbfs claim asynchronously, so a claim
	 * right after another process exited can spuriously fail with EBUSY. One
	 * retry covers it.
	 */
	for (attempt = 0; attempt < 2; attempt++) {
		status = libusb_claim_interface(handle, 0);
		if (status != LIBUSB_ERROR_BUSY)
			break;
		log_warn("interface busy on bus %u device %u, retrying", bus, addr);
		usleep(500000);
	}
	if (status != LIBUSB_SUCCESS) {
		log_err("libusb_claim_interface on bus %u device %u failed: %s",
			bus, addr, libusb_error_name(status));
		libusb_close(handle);
		return false;
	}

	status = ezusb_load_ram(handle, opt_firmware, FX_TYPE_FX3, IMG_TYPE_IMG, 0);

	/*
	 * The final step of the FX3 RAM load is a jump to the entry point, during
	 * which the device disconnects mid-transfer. An I/O error there is the
	 * expected outcome, not a failure.
	 */
	if (status != 0 && status != LIBUSB_ERROR_IO && status != LIBUSB_ERROR_NO_DEVICE) {
		log_err("firmware load on bus %u device %u failed: %s",
			bus, addr, libusb_error_name(status));
		libusb_release_interface(handle, 0);
		libusb_close(handle);
		return false;
	}

	libusb_release_interface(handle, 0);
	libusb_close(handle);

	log_info("firmware handed to bus %u device %u, waiting for it to re-enumerate as %04x:%04x",
		 bus, addr, RX888_VID, RX888_PID_APP);
	return true;
}

/* ----------------------------------------------------------------- scan cycle */

static unsigned scan_and_act(libusb_context *ctx)
{
	uint16_t present_apps[MAX_TRACKED];
	unsigned loaded = 0;
	size_t n_present = 0;
	libusb_device **list;
	ssize_t count, i;
	time_t now = time(NULL);

	count = libusb_get_device_list(ctx, &list);
	if (count < 0) {
		log_err("libusb_get_device_list failed: %s", libusb_error_name((int)count));
		return 0;
	}

	for (i = 0; i < count; i++) {
		struct libusb_device_descriptor desc;
		libusb_device *dev = list[i];
		uint8_t bus, addr;
		uint16_t key;

		if (libusb_get_device_descriptor(dev, &desc) != LIBUSB_SUCCESS)
			continue;
		if (desc.idVendor != RX888_VID)
			continue;

		bus = libusb_get_bus_number(dev);
		addr = libusb_get_device_address(dev);
		key = dev_key(bus, addr);

		if (desc.idProduct == RX888_PID_APP) {
			bool is_new;
			int slot;

			if (n_present < MAX_TRACKED)
				present_apps[n_present++] = key;

			slot = app_slot(key, &is_new);
			if (is_new) {
				int speed = libusb_get_device_speed(dev);

				log_info("RX-888 running firmware at bus %u device %u, link %s",
					 bus, addr, speed_name(speed));
				if (speed < LIBUSB_SPEED_SUPER)
					log_warn("link is below SuperSpeed; streaming will be "
						 "unreliable. Use a USB 3 port and cable");
			}
			relax_permissions(bus, addr,
					  slot >= 0 ? &app_relax_logged[slot] : NULL);
			continue;
		}

		if (desc.idProduct != RX888_PID_BOOT)
			continue;

		/*
		 * Only ever act on a bootloader device. Re-flashing a device that is
		 * already running firmware would need a RESETFX3 or USBDEVFS_RESET
		 * first, and there is no reason to go there.
		 */
		if (!backoff_ready(key, now))
			continue;

		relax_permissions(bus, addr, NULL);

		if (load_firmware(dev, bus, addr)) {
			backoff_clear(key);
			loaded++;
		} else {
			backoff_record_failure(key, now);
		}
	}

	app_forget_absent(present_apps, n_present);
	libusb_free_device_list(list, 1);
	return loaded;
}

/* -------------------------------------------------------------- hotplug/signals */

static int LIBUSB_CALL hotplug_cb(libusb_context *ctx, libusb_device *dev,
				  libusb_hotplug_event event, void *user_data)
{
	(void)ctx;
	(void)dev;
	(void)event;
	(void)user_data;

	/* Do no real work in the callback: just ask the main loop to rescan. */
	rescan_requested = 1;
	return 0;
}

static void on_signal(int signum)
{
	(void)signum;
	stop_requested = 1;
}

static void install_signal_handlers(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
}

/* ------------------------------------------------------------------------ main */

static void usage(const char *argv0)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"\n"
		"  --firmware PATH       FX3 firmware image (default: %s,\n"
		"                        overridden by $" FIRMWARE_ENV ")\n"
		"  --min-usbfs-mb N      warn if usbfs_memory_mb is below N (default: %ld)\n"
		"  --poll-interval SEC   fallback rescan interval (default: %ld)\n"
		"  --once                load firmware, then exit instead of watching\n"
		"  --verbose             raise ezusb verbosity\n"
		"  --quiet               lower ezusb verbosity\n"
		"  --help                this text\n",
		argv0, opt_firmware, opt_min_usbfs_mb, opt_poll_interval);
}

static bool parse_args(int argc, char **argv)
{
	int i;

	for (i = 1; i < argc; i++) {
		const char *arg = argv[i];

		if (strcmp(arg, "--firmware") == 0 && i + 1 < argc) {
			opt_firmware = argv[++i];
		} else if (strcmp(arg, "--min-usbfs-mb") == 0 && i + 1 < argc) {
			opt_min_usbfs_mb = strtol(argv[++i], NULL, 10);
		} else if (strcmp(arg, "--poll-interval") == 0 && i + 1 < argc) {
			opt_poll_interval = strtol(argv[++i], NULL, 10);
			if (opt_poll_interval < 1)
				opt_poll_interval = 1;
		} else if (strcmp(arg, "--once") == 0) {
			opt_once = true;
		} else if (strcmp(arg, "--verbose") == 0) {
			verbose++;
		} else if (strcmp(arg, "--quiet") == 0) {
			verbose--;
		} else if (strcmp(arg, "--help") == 0) {
			usage(argv[0]);
			exit(0);
		} else {
			log_err("unrecognised argument: %s", arg);
			usage(argv[0]);
			return false;
		}
	}
	return true;
}

/* For --once: success means at least one RX-888 is running firmware. */
static int status_exit_code(void)
{
	if (app_count() > 0)
		return 0;
	log_err("no RX-888 is running firmware");
	return 1;
}

int main(int argc, char **argv)
{
	libusb_hotplug_callback_handle cb_handle;
	libusb_context *ctx = NULL;
	bool hotplug = false;
	unsigned loaded;
	time_t last_poll;
	int status;

	const char *env_firmware;

	setvbuf(stderr, NULL, _IOLBF, 0);

	/*
	 * Precedence: built-in default, then RX888_FIRMWARE, then --firmware.
	 * The environment sits in the middle because an ExtensionServiceConfig can
	 * add environment entries to a running service but cannot rewrite its args,
	 * so this is the only way an operator can point the service at a different
	 * firmware image without rebuilding the extension.
	 */
	env_firmware = getenv(FIRMWARE_ENV);
	if (env_firmware != NULL && env_firmware[0] != '\0')
		opt_firmware = env_firmware;

	if (!parse_args(argc, argv))
		return 2;

	log_info("starting, firmware %s", opt_firmware);

	if (access(opt_firmware, R_OK) != 0) {
		log_err("firmware image %s is not readable: %s",
			opt_firmware, strerror(errno));
		return 1;
	}

	check_usbfs_memory();

	status = libusb_init(&ctx);
	if (status != LIBUSB_SUCCESS) {
		log_err("libusb_init failed: %s", libusb_error_name(status));
		return 1;
	}

	install_signal_handlers();

	/*
	 * LIBUSB_HOTPLUG_ENUMERATE is the important flag: it fires the callback for
	 * devices that are already attached at startup as well as for later arrivals.
	 * Without it, a node that booted with the SDR already plugged in would sit
	 * there in bootloader mode forever waiting for an event that never comes.
	 */
	if (!opt_once && libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG)) {
		status = libusb_hotplug_register_callback(
			ctx,
			LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED | LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT,
			LIBUSB_HOTPLUG_ENUMERATE,
			RX888_VID, LIBUSB_HOTPLUG_MATCH_ANY, LIBUSB_HOTPLUG_MATCH_ANY,
			hotplug_cb, NULL, &cb_handle);
		if (status == LIBUSB_SUCCESS) {
			hotplug = true;
			log_info("watching for hotplug events");
		} else {
			log_warn("hotplug registration failed (%s), falling back to polling",
				 libusb_error_name(status));
		}
	} else if (!opt_once) {
		log_warn("libusb reports no hotplug support, falling back to polling");
	}

	last_poll = time(NULL);
	loaded = scan_and_act(ctx);
	rescan_requested = 0;

	if (opt_once) {
		/*
		 * A device we just programmed takes a moment to drop off the bus and
		 * come back as 04b4:00f1. Wait for that so a one-shot run reports the
		 * real outcome, and so the new node gets its mode relaxed before we go.
		 */
		if (loaded > 0) {
			int waited;

			for (waited = 0; waited < REENUMERATE_TIMEOUT_MS; waited += 250) {
				usleep(250000);
				scan_and_act(ctx);
				if (app_count() > 0)
					break;
			}
			if (app_count() == 0)
				log_warn("device did not re-enumerate as %04x:%04x within %d ms",
					 RX888_VID, RX888_PID_APP, REENUMERATE_TIMEOUT_MS);
		}
		log_info("--once given, exiting");
		libusb_exit(ctx);
		return status_exit_code();
	}

	while (!stop_requested) {
		struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
		time_t now;

		if (hotplug)
			libusb_handle_events_timeout_completed(ctx, &tv, NULL);
		else
			usleep(250000);

		if (stop_requested)
			break;

		now = time(NULL);
		if (rescan_requested || now - last_poll >= opt_poll_interval) {
			rescan_requested = 0;
			last_poll = now;
			scan_and_act(ctx);
		}
	}

	log_info("shutting down");
	if (hotplug)
		libusb_hotplug_deregister_callback(ctx, cb_handle);
	libusb_exit(ctx);
	return 0;
}
