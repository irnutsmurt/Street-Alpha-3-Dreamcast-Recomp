/* UDP transport for netplay, with an optional network simulator. */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t ip;    /* network byte order */
    uint16_t port;  /* host byte order */
} NetAddr;

int net_open(uint16_t port);                 /* bind a non-blocking UDP socket (0 = any port) */
void net_close(void);
int net_resolve(const char *host, uint16_t port, NetAddr *out);
void net_send(const NetAddr *to, const void *buf, size_t len);
int net_recv(void *buf, size_t cap, NetAddr *from);  /* bytes, 0 if nothing */
int net_broadcast(uint16_t port, const void *buf, size_t len);
const char *net_addr_str(const NetAddr *a);
int net_local_addrs(char out[][16], int max);  /* this machine's IPv4 addresses */
int net_is_open(void);

/* Network simulator for testing (SFA3_NET_SIM="delay_ms,jitter_ms,loss_percent"):
 * outgoing packets are delayed and randomly dropped. */
void net_sim_config(int delay_ms, int jitter_ms, int loss_pct);
