#pragma once

// Single mutex guarding all shared protocol/result state (LLDP, CDP, DHCP,
// EAPOL and connectivity results). These structs are written from the Ethernet
// RX callback and the connectivity task, and read from the HTTP handlers and
// the UI loop. Writers hold the lock while mutating; readers hold it while
// taking a consistent snapshot.

void proto_lock_init(void);
void proto_lock(void);
void proto_unlock(void);
