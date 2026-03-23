#pragma once

#include <stdbool.h>
#include <stdint.h>

/**
 * Process a potential EAPOL frame (EtherType 0x888E).
 * Call from raw_frame_recv() when EtherType matches.
 */
void eapol_process(const uint8_t *data, uint32_t length);

/**
 * Returns true if an EAPOL frame has been detected on this link,
 * indicating the port requires 802.1X authentication.
 */
bool eapol_detected(void);

/**
 * Get a human-readable description of the EAPOL state detected.
 * Returns NULL if no EAPOL frames have been seen.
 */
const char *eapol_get_status(void);

/**
 * Reset EAPOL detection state (call on link down).
 */
void eapol_reset(void);
