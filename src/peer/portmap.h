/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* portmap.h - asking the router of the home to let the port in, so that a node behind it can be dialled and be a custodian
 * (docs/en/NETWORK.md 7.9). As Bitcoin Core does, by PCP (RFC 6887) or, for a router that only speaks that, by the NAT-PMP before it
 * (RFC 6886); then, for a router that only speaks that, by UPnP IGD. Asked unless the user says not to (--no-portmap). IPv4. */
#ifndef PZPEER_PORTMAP_H
#define PZPEER_PORTMAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Asks the router to send the TCP port `port` to this machine, the same port outside, for lease_s seconds. nonce: 12 bytes that stay the
 * same for this node (PCP renews a mapping only for the nonce that made it). router: NULL for the router of the default route and the
 * multicast search of UPnP; tests give the address of a router of theirs, which takes all three. The seconds the router keeps the port
 * open (to be asked again before half of them pass), 0 when it does not; what happened in why. */
unsigned portmap(uint16_t port, unsigned lease_s, const uint8_t nonce[12], const char *router, char *why, size_t cap);

#endif
