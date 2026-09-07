/* OutRun Net — link counters, bandwidth, and the socket table.
 *
 * Source: SYS_HW_INFO's HW_NET domain, which reports the virtio-net device's
 * lifetime frame and BYTE counters, the guest address, and every live socket
 * in the kernel's table with its TCP state.
 *
 * BANDWIDTH IS A DELTA, and this program computes it from bytes rather than
 * frames. A frame count is not bandwidth — sixty ARP frames and sixty full
 * segments are the same number and two orders of magnitude apart — which is
 * why the byte counters exist at all.
 *
 * THIS PROGRAM DOES NOT CONFIGURE THE LINK, and it says so on screen. The
 * address, netmask and MTU are fixed in the kernel's network stack: there is
 * no syscall that changes them, so there is no control here that pretends to.
 * A settings pane whose values were read back from a constant would be a
 * screen that could never be wrong and could never do anything. */
#include "../include/outrun_abi.h"

/* The kernel's TCP state machine, named here so this program's labels and the
 * kernel's `enum { TCPS_CLOSED, ... }` are the same list in the same order. */
enum {
    NET_TCPS_CLOSED = 0, NET_TCPS_LISTEN, NET_TCPS_SYN_SENT, NET_TCPS_SYN_RCVD,
    NET_TCPS_ESTABLISHED, NET_TCPS_FIN_WAIT1, NET_TCPS_FIN_WAIT2,
    NET_TCPS_CLOSE_WAIT, NET_TCPS_CLOSING, NET_TCPS_LAST_ACK, NET_TCPS_TIME_WAIT
};
enum { NET_TAB_LINK = 0, NET_TAB_SOCKETS = 1, NET_TAB_COUNTERS = 2, NET_NTAB = 3 };
#define NET_HIST 100

struct net_state {
    struct outrun_net_info prev;
    unsigned long long prev_ns;
    unsigned long long tx_bps[HW_MAX_NETIF], rx_bps[HW_MAX_NETIF];
    unsigned long long tx_fps[HW_MAX_NETIF], rx_fps[HW_MAX_NETIF];
    unsigned samples;
    int tab, have_prev;
    unsigned char tx_hist[NET_HIST], rx_hist[NET_HIST];
};

static void net_dec(char *out, int *at, unsigned long long v) {
    char d[24];
    int n = 0;
    do { d[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) out[(*at)++] = d[--n];
}
/* Host byte order, most significant octet first — the order the ABI carries
 * and the order the kernel's NET_GUEST_IP constant is written in. */
static void net_ip_str(char *out, unsigned ip) {
    int at = 0;
    for (int i = 3; i >= 0; --i) {
        net_dec(out, &at, (ip >> (i * 8)) & 0xFFu);
        if (i) out[at++] = '.';
    }
    out[at] = 0;
}
static void net_mac_str(char *out, const unsigned char *mac) {
    static const char hex[] = "0123456789ABCDEF";
    int at = 0;
    for (int i = 0; i < 6; i++) {
        out[at++] = hex[(mac[i] >> 4) & 0xF];
        out[at++] = hex[mac[i] & 0xF];
        if (i < 5) out[at++] = ':';
    }
    out[at] = 0;
}
static unsigned long long net_delta(unsigned long long now, unsigned long long was) {
    return now > was ? now - was : 0ull;
}
/* Per second, from a nanosecond interval. Two branches, not a shift loop: the
 * loop this replaced halved BOTH terms until the product fitted, which drove
 * `ns` to zero for a large enough delta and then returned 0 — turning an
 * absurd input into "no traffic", the one answer that could not be
 * distinguished from a quiet link. Dividing first loses sub-nanosecond
 * precision at a scale where there is none to lose, and cannot vanish. */
static unsigned long long net_rate(unsigned long long delta, unsigned long long ns) {
    if (!ns) return 0;
    if (delta <= (unsigned long long)~0ull / 1000000000ull)
        return delta * 1000000000ull / ns;
    return (delta / ns) * 1000000000ull;
}
static void net_reset(struct net_state *t) {
    unsigned char *z = (unsigned char *)t;
    for (unsigned long i = 0; i < sizeof *t; i++) z[i] = 0;
}
static void net_sample(struct net_state *t, const struct outrun_net_info *s,
                       unsigned long long now_ns) {
    unsigned long long elapsed = net_delta(now_ns, t->prev_ns);
    if (t->have_prev && elapsed) {
        for (unsigned i = 0; i < HW_MAX_NETIF; i++) {
            t->tx_bps[i] = net_rate(net_delta(s->iface[i].tx_bytes, t->prev.iface[i].tx_bytes), elapsed);
            t->rx_bps[i] = net_rate(net_delta(s->iface[i].rx_bytes, t->prev.iface[i].rx_bytes), elapsed);
            t->tx_fps[i] = net_rate(net_delta(s->iface[i].tx_frames, t->prev.iface[i].tx_frames), elapsed);
            t->rx_fps[i] = net_rate(net_delta(s->iface[i].rx_frames, t->prev.iface[i].rx_frames), elapsed);
        }
        t->prev_ns = now_ns;
    } else if (!t->have_prev) {
        t->prev_ns = now_ns;
    }
    t->prev = *s;
    t->have_prev = 1;
    /* The graph is scaled against a fixed 100 KiB/s reference rather than
     * against the observed peak: a link that has only ever carried ARP would
     * otherwise draw those few frames as a full-height spike. */
    unsigned tx = (unsigned)(t->tx_bps[0] / 1024ull);
    unsigned rx = (unsigned)(t->rx_bps[0] / 1024ull);
    t->tx_hist[t->samples % NET_HIST] = (unsigned char)(tx > 100 ? 100 : tx);
    t->rx_hist[t->samples % NET_HIST] = (unsigned char)(rx > 100 ? 100 : rx);
    t->samples++;
}
static void net_set_tab(struct net_state *t, int tab) {
    if (tab < 0 || tab >= NET_NTAB) return;
    t->tab = tab;
}
static const char *net_sock_kind(const struct outrun_socket *s) {
    return (s->flags & HW_SOCK_STREAM) ? "TCP" : "UDP";
}
static char g_state_buf[16];
/* Streams report the TCP state machine's own name. Collapsing them into
 * "CONNECTED" would hide FIN-WAIT and TIME-WAIT, which are precisely the
 * states a socket gets stuck in and therefore the ones worth seeing. */
static const char *net_sock_state(const struct outrun_socket *s) {
    if (s->flags & HW_SOCK_STREAM) {
        if (s->flags & HW_SOCK_LISTENING) return "LISTEN";
        switch (s->state) {
        case NET_TCPS_CLOSED:      return "CLOSED";
        case NET_TCPS_LISTEN:      return "LISTEN";
        case NET_TCPS_SYN_SENT:    return "SYN-SENT";
        case NET_TCPS_SYN_RCVD:    return "SYN-RCVD";
        case NET_TCPS_ESTABLISHED: return "ESTABLISHED";
        case NET_TCPS_FIN_WAIT1:   return "FIN-WAIT1";
        case NET_TCPS_FIN_WAIT2:   return "FIN-WAIT2";
        case NET_TCPS_CLOSE_WAIT:  return "CLOSE-WAIT";
        case NET_TCPS_CLOSING:     return "CLOSING";
        case NET_TCPS_LAST_ACK:    return "LAST-ACK";
        case NET_TCPS_TIME_WAIT:   return "TIME-WAIT";
        default: break;
        }
        int at = 0;
        const char *p = "STATE ";
        while (*p) g_state_buf[at++] = *p++;
        net_dec(g_state_buf, &at, s->state);
        g_state_buf[at] = 0;
        return g_state_buf;
    }
    /* Datagram sockets have no session. "PEERED" is a default destination,
     * not a connection, and calling it CONNECTED would imply a handshake that
     * never happened. */
    if (s->flags & HW_SOCK_CONNECTED) return "PEERED";
    if (s->lport) return "BOUND";
    return "IDLE";
}
static unsigned long long net_queued_rx(const struct outrun_net_info *s) {
    unsigned long long n = 0;
    for (unsigned i = 0; i < s->nsock && i < HW_MAX_SOCK; i++) n += s->sock[i].rx_queued;
    return n;
}
static unsigned long long net_queued_tx(const struct outrun_net_info *s) {
    unsigned long long n = 0;
    for (unsigned i = 0; i < s->nsock && i < HW_MAX_SOCK; i++) n += s->sock[i].tx_queued;
    return n;
}
static const char *net_link_note(const struct outrun_net_info *s) {
    if (!s->nif) return "NO VIRTIO-NET DEVICE: BOOT WITH -device virtio-net-pci";
    return s->iface[0].ready ? "LINK UP" : "DEVICE PRESENT BUT NOT READY";
}

#ifndef APP_HOST_TEST
#include "gui.h"

static struct net_state state;
static struct outrun_net_info sample;

static void net_rate_str(char *out, unsigned long long bps) {
    int at = 0;
    if (bps >= 1024ull * 1024ull) {
        net_dec(out, &at, bps / (1024ull * 1024ull));
        out[at++] = '.';
        out[at++] = (char)('0' + (bps % (1024ull * 1024ull)) * 10ull / (1024ull * 1024ull));
        const char *u = " MIB/S";
        while (*u) out[at++] = *u++;
    } else if (bps >= 1024ull) {
        net_dec(out, &at, bps / 1024ull);
        out[at++] = '.';
        out[at++] = (char)('0' + (bps % 1024ull) * 10ull / 1024ull);
        const char *u = " KIB/S";
        while (*u) out[at++] = *u++;
    } else {
        net_dec(out, &at, bps);
        const char *u = " B/S";
        while (*u) out[at++] = *u++;
    }
    out[at] = 0;
}
static void net_graph(struct app_win *w, const unsigned char *v, unsigned count,
                      int x, int y, int h, u32 color) {
    app_rect(w, x, y, NET_HIST * 4, h, 0x121722);
    unsigned n = count < NET_HIST ? count : NET_HIST;
    unsigned first = count - n;
    for (unsigned i = 0; i < n; ++i) {
        int bar = (int)((unsigned)v[(first + i) % NET_HIST] * (unsigned)(h - 2) / 100u);
        app_rect(w, x + (int)i * 4, y + h - 1 - bar, 3, bar + 1, color);
    }
}
static void net_render(struct app_win *w, struct net_state *t) {
    static const char *tabs[NET_NTAB] = { "LINK", "SOCKETS", "COUNTERS" };
    const struct outrun_net_info *s = &t->prev;
    char b[40];
    app_fill(w, w->bg);
    for (int i = 0; i < NET_NTAB; i++) {
        app_rect(w, i * 104, 0, 100, 22, t->tab == i ? 0x28495d : 0x1c2636);
        app_str(w, i * 104 + 10, 7, tabs[i], t->tab == i ? 0x22e4ff : 0x7c8ca0);
    }
    int y = 32;
    if (t->tab == NET_TAB_LINK) {
        app_str(w, 8, y, net_link_note(s), s->nif && s->iface[0].ready ? 0x3df5c4 : 0xffb020);
        y += 18;
        if (s->nif) {
            const struct outrun_netif *n = &s->iface[0];
            app_str(w, 8, y, n->name, 0x22e4ff); y += 16;
            app_str(w, 8, y, "MAC", 0x7c8ca0);
            net_mac_str(b, n->mac); app_str(w, 120, y, b, w->fg); y += 14;
            app_str(w, 8, y, "IPV4", 0x7c8ca0);
            net_ip_str(b, n->ipv4); app_str(w, 120, y, b, w->fg); y += 14;
            app_str(w, 8, y, "MTU", 0x7c8ca0);
            app_u32(w, 120, y, n->mtu, w->fg); y += 14;
            app_str(w, 8, y, "IRQ LINE", 0x7c8ca0);
            app_u32(w, 120, y, n->irq_line, w->fg); y += 14;
            app_str(w, 8, y, "IRQS", 0x7c8ca0);
            app_u32(w, 120, y, (u32)n->irqs, 0x3df5c4); y += 18;
            app_str(w, 8, y, "TX", 0x22e4ff);
            net_rate_str(b, t->tx_bps[0]); app_str(w, 40, y, b, w->fg);
            app_u32(w, 200, y, (u32)t->tx_fps[0], 0x7c8ca0);
            app_str(w, 240, y, "FRAMES/S", 0x7c8ca0); y += 14;
            net_graph(w, t->tx_hist, t->samples, 8, y, 40, 0x22e4ff); y += 46;
            app_str(w, 8, y, "RX", 0x3df5c4);
            net_rate_str(b, t->rx_bps[0]); app_str(w, 40, y, b, w->fg);
            app_u32(w, 200, y, (u32)t->rx_fps[0], 0x7c8ca0);
            app_str(w, 240, y, "FRAMES/S", 0x7c8ca0); y += 14;
            net_graph(w, t->rx_hist, t->samples, 8, y, 40, 0x3df5c4); y += 48;
            app_str(w, 8, y, "GRAPHS SCALE TO A FIXED 100 KIB/S", 0x7c8ca0);
        }
    } else if (t->tab == NET_TAB_SOCKETS) {
        app_str(w, 8, y, "KIND", 0x7c8ca0);
        app_str(w, 48, y, "LOCAL", 0x7c8ca0);
        app_str(w, 112, y, "REMOTE", 0x7c8ca0);
        app_str(w, 248, y, "STATE", 0x7c8ca0);
        app_str(w, 360, y, "RX/TX", 0x7c8ca0);
        y += 16;
        for (unsigned i = 0; i < s->nsock && i < HW_MAX_SOCK; i++) {
            const struct outrun_socket *sk = &s->sock[i];
            u32 c = (sk->flags & HW_SOCK_LISTENING) ? 0xffb020
                  : (sk->flags & HW_SOCK_CONNECTED) ? 0x3df5c4 : w->fg;
            app_str(w, 8, y, net_sock_kind(sk), c);
            app_u32(w, 48, y, sk->lport, w->fg);
            if (sk->raddr) {
                net_ip_str(b, sk->raddr);
                app_str(w, 112, y, b, w->fg);
                app_str(w, 208, y, ":", 0x7c8ca0);
                app_u32(w, 216, y, sk->rport, w->fg);
            } else {
                app_str(w, 112, y, "-", 0x7c8ca0);
            }
            app_str(w, 248, y, net_sock_state(sk), c);
            app_u32(w, 360, y, sk->rx_queued, 0x3df5c4);
            app_str(w, 392, y, "/", 0x7c8ca0);
            app_u32(w, 400, y, sk->tx_queued, 0x22e4ff);
            y += 12;
        }
        if (!s->nsock) app_str(w, 8, y, "NO SOCKETS OPEN", 0x7c8ca0);
        y += 16;
        app_str(w, 8, y, "QUEUED RX BYTES", 0x7c8ca0);
        app_u32(w, 200, y, (u32)net_queued_rx(s), 0x3df5c4); y += 14;
        app_str(w, 8, y, "QUEUED TX BYTES", 0x7c8ca0);
        app_u32(w, 200, y, (u32)net_queued_tx(s), 0x22e4ff);
    } else {
        app_str(w, 8, y, "LIFETIME STACK COUNTERS", 0x22e4ff); y += 18;
        app_str(w, 8, y, "FRAMES TX", 0x7c8ca0);
        app_u32(w, 208, y, (u32)s->tx_frames, w->fg); y += 14;
        app_str(w, 8, y, "FRAMES RX", 0x7c8ca0);
        app_u32(w, 208, y, (u32)(s->nif ? s->iface[0].rx_frames : 0), w->fg); y += 14;
        app_str(w, 8, y, "BYTES TX", 0x7c8ca0);
        app_u32(w, 208, y, (u32)(s->nif ? s->iface[0].tx_bytes : 0), w->fg); y += 14;
        app_str(w, 8, y, "BYTES RX", 0x7c8ca0);
        app_u32(w, 208, y, (u32)(s->nif ? s->iface[0].rx_bytes : 0), w->fg); y += 18;
        app_str(w, 8, y, "LOOPBACK DELIVERIES", 0x7c8ca0);
        app_u32(w, 208, y, (u32)s->loop_deliveries, w->fg); y += 14;
        app_str(w, 8, y, "ACCEPTS", 0x7c8ca0);
        app_u32(w, 208, y, (u32)s->accepts, w->fg); y += 14;
        app_str(w, 8, y, "SESSIONS", 0x7c8ca0);
        app_u32(w, 208, y, (u32)s->sessions, w->fg); y += 14;
        app_str(w, 8, y, "EAGAIN", 0x7c8ca0);
        app_u32(w, 208, y, (u32)s->eagain, 0xffb020); y += 14;
        app_str(w, 8, y, "SOCKETS OPEN", 0x7c8ca0);
        app_u32(w, 208, y, s->nsock, w->fg);
    }
    app_str(w, 8, w->ch - 26, t->samples < 2 ? "SAMPLING: RATES APPEAR AFTER TWO SAMPLES"
                                             : "READ-ONLY MONITOR", 0xffb020);
    app_str(w, 8, w->ch - 12, "NO LINK CONFIGURATION: THE STACK'S ADDRESS IS FIXED", 0x7c8ca0);
    app_present(w);
}

void _start(void) {
    struct app_win w;
    if (app_create(&w, 430, 440, 0x3df5c4)) app_exit(1);
    app_title(&w, "OUTRUN NET");
    net_reset(&state);
    u64 last = 0;
    for (;;) {
        struct outrun_desktop_info clock;
        int dirty = 0;
        if ((i64)sysc(SYS_DESKTOP_INFO, (u64)&clock, sizeof clock, 0) >= 0 &&
            (!last || clock.wall_ns - last >= 500000000ull)) {
            last = clock.wall_ns;
            i64 rc = (i64)sysc(SYS_HW_INFO, HW_NET, (u64)&sample, sizeof sample);
            if (rc < 0 || sample.version != OUTRUN_HW_ABI_VERSION) app_exit(2);
            net_sample(&state, &sample, clock.wall_ns);
            dirty = 1;
        }
        struct outrun_event e;
        int prc;
        while ((prc = app_poll(&w, &e)) > 0) {
            if (e.type == EVENT_MOUSE_DOWN && e.y < 22) { net_set_tab(&state, e.x / 104); dirty = 1; }
            if (e.type == EVENT_KEY_PRESS) {
                if (e.code == '\t') net_set_tab(&state, (state.tab + 1) % NET_NTAB);
                if (e.code >= '1' && e.code <= '3') net_set_tab(&state, e.code - '1');
                dirty = 1;
            }
        }
        if (prc < 0) app_exit(0);
        if (dirty) net_render(&w, &state);
        app_idle();
    }
}
#endif
