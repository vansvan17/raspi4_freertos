/* ramlog.h
 *
 * Hands results to Linux through RAM instead of the UART. Linux reserves each
 * region with a no-map reserved-memory node, arms it, and reboots into this
 * image with tryboot. The image fills the region and resets through the PM
 * watchdog, and Linux reads the region back after it boots again.
 *
 * The MMU is off in these images, so every access below goes straight to DRAM
 * without cache maintenance. Keep this layout in sync with tools/pi_ramlog.py.
 */
#ifndef RAMLOG_H
#define RAMLOG_H

#include <stdint.h>

#define RAMLOG_PRIMARY_BASE   (0x60000000UL)
#define RAMLOG_SECONDARY_BASE (0x30000000UL)
#define RAMLOG_REGION_SIZE    (0x00100000UL)
#define RAMLOG_HEADER_SIZE    (64U)
#define RAMLOG_CAPACITY       (RAMLOG_REGION_SIZE - RAMLOG_HEADER_SIZE)

#define RAMLOG_HOST_ARMED     (0x484F5354U) /* written by Linux before reboot */
#define RAMLOG_FW_MAGIC       (0x52544F53U) /* written by this image */

#define RAMLOG_STATE_RUNNING  (1U)
#define RAMLOG_STATE_COMPLETE (2U)

#define RAMLOG_KIND_LOG       (1U) /* data is the text the UART would carry */
#define RAMLOG_KIND_PATTERN   (2U) /* data is the persistence-test pattern */

struct ramlog_header {
    uint32_t host_magic;
    uint32_t fw_magic;
    uint64_t host_nonce;
    uint64_t seen_nonce;  /* host_nonce as found at boot, 0 if not armed */
    uint32_t state;
    uint32_t length;
    uint32_t capacity;
    uint32_t checksum;    /* FNV-1a over data[0, length) */
    uint32_t kind;
    uint32_t reports;
    uint32_t dropped;
    uint32_t reserved[3];
};

void ramlog_begin(uint32_t kind);
void ramlog_putc(uint8_t c);
void ramlog_note_report(void);
void ramlog_finish(void);

uint32_t ramlog_fnv1a(volatile const uint8_t *data, uint32_t length);

void pm_watchdog_start(uint32_t seconds);
void pm_watchdog_reset_now(void) __attribute__((noreturn));

#endif /* RAMLOG_H */
