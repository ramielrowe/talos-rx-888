/* SPDX-License-Identifier: MIT
 *
 * Minimal stand-in for libusb's autoconf-generated <config.h>.
 *
 * The vendored ezusb.c/ezusb.h are lifted verbatim from libusb's examples/,
 * which are built inside libusb's own autotools tree and therefore include
 * <config.h>. PRINTF_FORMAT is the only thing they actually need from it.
 */
#ifndef RX888_CONFIG_H
#define RX888_CONFIG_H

#if defined(__GNUC__)
#define PRINTF_FORMAT(a, b) __attribute__((__format__(__printf__, a, b)))
#else
#define PRINTF_FORMAT(a, b)
#endif

#endif /* RX888_CONFIG_H */
