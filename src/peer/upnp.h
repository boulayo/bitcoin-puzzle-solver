/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* upnp.h - asking the router of the home to let the port in (UPnP IGD, the protocol by which programs open a port on home routers), so
 * that a node behind it can be dialled and be a custodian (docs/en/NETWORK.md 7.9). Only when the user asks (--upnp). IPv4. */
#ifndef PZPEER_UPNP_H
#define PZPEER_UPNP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Finds the router and asks it to send the TCP port `port` to this machine, the same port outside, for lease_s seconds (asked again
 * before they pass; a router that only takes mappings without end gets one). ssdp: where the search goes, NULL for the multicast address
 * of the protocol (tests give the address of their own router). False with the reason in why. */
bool upnp_map(uint16_t port, unsigned lease_s, const char *ssdp, char *why, size_t cap);

#endif
