/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* net.h - TCP and the encrypted channel between two nodes (docs/en/NETWORK.md 7.1). One TCP connection per exchange: connect, the Noise XX
 * handshake with the prologue "pznet1" || F, one request frame, one answer frame, close. A session keeps its connection and goes on with
 * frames. This is the only file of pzpeer that calls the sockets of the system (POSIX; Windows gets a shim of its own), and every wait has
 * a deadline: a peer that stops talking costs a timeout, never a thread.
 *
 * Both sides check that the node ID of the other's static key has the leading zero bits of the network; a dialler that knows whom it calls
 * also checks the identity before it shows its own (the initiator of XX sends its static key last). The initiator tells, in its last
 * handshake message, the port it listens on, so the responder can put it in its routing table with the address it sees. */
#ifndef PZPEER_NET_H
#define PZPEER_NET_H

#include "keys.h"
#include "noise.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NET_TIMEOUT_MS 10000       /* default limit of a connect, a handshake message, a frame */
#define NET_HS1 33                 /* -> e                                      (the handshake messages have fixed lengths) */
#define NET_HS2 98                 /* <- e, ee, s, es: e, the encrypted static key, the tag of an empty payload */
#define NET_HS3 67                 /* -> s, se: the encrypted static key and the encrypted listening port (2 bytes, big-endian) */

typedef struct {
    uint8_t ip[16];                /* IPv6; an IPv4 address as ::ffff:a.b.c.d */
    uint16_t port;
} net_addr;

/* What a node brings to every connection; read only once set. */
typedef struct {
    const secp256k1_context *ctx;
    const home_identity *self;
    uint8_t prologue[38];          /* "pznet1" || F */
    unsigned id_bits;              /* leading zero bits a node ID must have: NODE_ID_ZERO_BITS on a real network */
    uint16_t port;                 /* the port this node listens on, told to the nodes it dials; 0 when it does not listen */
    int timeout_ms;
} net_cfg;

/* A connection after its handshake. The fields are read only. Sending and receiving share the frame buffer: one thread at a time uses a
 * connection. */
typedef struct {
    int fd;
    int timeout_ms;                /* of each frame from now on (net_set_timeout) */
    noise_cs tx, rx;
    uint8_t remote[32];            /* x-only identity of the other side */
    uint8_t remote_id[32];         /* its node ID */
    net_addr addr;                 /* dialled: the address; accepted: the address seen, with the port the other side listens on (0: none) */
    uint8_t hash[32];              /* the hash of the handshake, the same on both sides (a session can sign it) */
    uint8_t buf[NOISE_HEAD + NOISE_FRAME_MAX + NOISE_TAG];
} net_conn;

void net_cfg_init(net_cfg *cfg, const secp256k1_context *ctx, const home_identity *self, const uint8_t fp[32], unsigned id_bits,
                  uint16_t port, int timeout_ms);
/* n bytes from the random source of the system (ephemeral keys, nonces); false when it cannot be read. */
bool net_random(uint8_t *out, size_t n);

/* "host:port", "[ipv6]:port"; a host name is resolved (the first address). False for a port outside 1..65535 or a name that does not resolve. */
bool net_parse_addr(const char *hostport, net_addr *out);
/* "a.b.c.d:port" or "[ipv6]:port" (at least 48 bytes). */
void net_addr_str(const net_addr *a, char *out, size_t cap);

/* A listening socket on a numeric address (NULL or "": every address, IPv6 and IPv4, or IPv4 alone where there is no IPv6), port 0 for a
 * free one; the port it got in *bound. -1 on failure. */
int net_listen(const char *host, uint16_t port, uint16_t *bound);
/* The next connection on a listening socket within timeout_ms: its socket, and the address it comes from. -1 when none came, -2 on an error
 * of the system (too many open files: the caller waits a little). */
int net_accept(int lfd, int timeout_ms, net_addr *from);
void net_close_fd(int fd);

/* Dials and runs the handshake as initiator. expect: the x-only identity the other side must have (NULL: any valid one). NULL on failure. */
net_conn *net_dial(const net_cfg *cfg, const net_addr *to, const uint8_t expect[32]);
/* The handshake as responder on an accepted socket, which the connection takes (it is closed on failure). NULL on failure. */
net_conn *net_handshake(const net_cfg *cfg, int fd, const net_addr *from);
/* One frame of at most NOISE_FRAME_MAX bytes. */
bool net_send(net_conn *c, const uint8_t *body, size_t len);
/* The next frame into body; false on a timeout, a closed connection, a frame that does not authenticate or one longer than cap. After a
 * false the channel is out of step: close it. */
bool net_recv(net_conn *c, uint8_t *body, size_t cap, size_t *len);
void net_set_timeout(net_conn *c, int timeout_ms);
/* Closes the socket and wipes the keys of the channel. NULL is ignored. */
void net_close(net_conn *c);

/* One exchange: dial, the request, the answer (at most cap bytes), close. remote (may be NULL): the identity that answered. */
bool net_exchange(const net_cfg *cfg, const net_addr *to, const uint8_t expect[32], const uint8_t *req, size_t rlen, uint8_t *ans,
                  size_t cap, size_t *alen, uint8_t remote[32]);

#endif
