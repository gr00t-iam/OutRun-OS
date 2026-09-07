/* Host tests for the network deck's core.
 *
 * Two things carry the weight here. The first is bandwidth, which is a rate
 * and therefore has the same failure modes as every other rate in this tree:
 * no previous sample, no elapsed time, and a counter that appears to move
 * backwards. The second is the socket table, whose rows are decoded from a
 * flag word and a TCP state number — and where a listening socket, a connected
 * stream and a bound datagram socket must not all render as "OPEN".
 *
 * The address formatter is tested against the loopback and SLIRP addresses the
 * kernel actually uses, in the host byte order the ABI actually carries. Byte
 * order is the classic place for a display to be confidently backwards. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define APP_HOST_TEST
#include "net_deck.c"

static struct net_state st;
static struct outrun_net_info s;

static void base(void) {
    memset(&s, 0, sizeof s);
    s.version = OUTRUN_HW_ABI_VERSION;
    s.size = (unsigned)sizeof s;
    s.nif = 1;
    s.iface[0].ready = 1;
    s.iface[0].mtu = 1500;
    s.iface[0].irq_line = 11;
    s.iface[0].ipv4 = 0x0A000210u;
    s.iface[0].mac[0] = 0x52; s.iface[0].mac[1] = 0x54; s.iface[0].mac[2] = 0x00;
    s.iface[0].mac[3] = 0x12; s.iface[0].mac[4] = 0x34; s.iface[0].mac[5] = 0x56;
    strcpy(s.iface[0].name, "virtio-net0");
}

int main(void) {
    char b[32];

    /* ---- address and MAC formatting -------------------------------------- */
    net_ip_str(b, 0x0A000210u);   assert(!strcmp(b, "10.0.2.16"));
    net_ip_str(b, 0x7F000001u);   assert(!strcmp(b, "127.0.0.1"));
    net_ip_str(b, 0u);            assert(!strcmp(b, "0.0.0.0"));
    net_ip_str(b, 0xFFFFFFFFu);   assert(!strcmp(b, "255.255.255.255"));
    net_mac_str(b, s.iface[0].mac);
    /* base() has not run yet: the MAC is all zeroes, and that must format. */
    assert(!strcmp(b, "00:00:00:00:00:00"));
    base();
    net_mac_str(b, s.iface[0].mac);
    assert(!strcmp(b, "52:54:00:12:34:56"));

    /* ---- the delta and rate primitives, directly -------------------------
     * Asserted here as well as through net_sample because the sampled path
     * cannot discriminate: a backwards counter produces a near-2^64 delta, and
     * every plausible rate calculation on a number that size lands back near
     * zero by one route or another. Only the primitive shows the difference. */
    assert(net_delta(10, 4) == 6);
    assert(net_delta(4, 10) == 0);
    assert(net_delta(5, 5) == 0);
    assert(net_rate(2000, 1000000000ull) == 2000);
    assert(net_rate(2000, 500000000ull) == 4000);
    assert(net_rate(1, 0) == 0);
    /* A delta too large to multiply by 1e9 must still come back large. The
     * guard that used to sit here answered 0, which reads as an idle link. */
    assert(net_rate(~0ull - 9998ull, 1000000000ull) > 1000000000ull);

    /* ---- rates ----------------------------------------------------------- */
    net_reset(&st);
    s.iface[0].tx_frames = 10; s.iface[0].rx_frames = 20;
    s.iface[0].tx_bytes = 1000; s.iface[0].rx_bytes = 2000;
    net_sample(&st, &s, 1000000000ull);
    /* The first sample has nothing to subtract from. Showing the lifetime
     * total as a rate would report a machine that has been up for an hour as
     * saturating its link. */
    assert(st.tx_bps[0] == 0 && st.rx_bps[0] == 0);
    assert(st.samples == 1);

    s.iface[0].tx_bytes = 3000; s.iface[0].rx_bytes = 2500;
    s.iface[0].tx_frames = 20; s.iface[0].rx_frames = 25;
    net_sample(&st, &s, 2000000000ull);        /* exactly one second later     */
    assert(st.tx_bps[0] == 2000 && st.rx_bps[0] == 500);
    assert(st.tx_fps[0] == 10 && st.rx_fps[0] == 5);

    /* Half a second: the same delta is twice the rate. */
    s.iface[0].tx_bytes = 4000;
    net_sample(&st, &s, 2500000000ull);
    assert(st.tx_bps[0] == 2000);

    /* No elapsed time holds the last rate rather than dividing by zero. */
    unsigned long long held = st.tx_bps[0];
    s.iface[0].tx_bytes = 9999;
    net_sample(&st, &s, 2500000000ull);
    assert(st.tx_bps[0] == held);

    /* A counter that appears to go backwards floors at zero. */
    s.iface[0].tx_bytes = 0;
    net_sample(&st, &s, 3500000000ull);
    assert(st.tx_bps[0] == 0);

    /* ---- socket decoding ------------------------------------------------- */
    /* A datagram socket, bound and idle. */
    struct outrun_socket sk;
    memset(&sk, 0, sizeof sk);
    sk.lport = 7777;
    assert(!strcmp(net_sock_kind(&sk), "UDP"));
    assert(!strcmp(net_sock_state(&sk), "BOUND"));

    sk.flags = HW_SOCK_STREAM;
    sk.state = 0;
    assert(!strcmp(net_sock_kind(&sk), "TCP"));
    assert(!strcmp(net_sock_state(&sk), "CLOSED"));

    sk.flags = HW_SOCK_STREAM | HW_SOCK_LISTENING;
    assert(!strcmp(net_sock_state(&sk), "LISTEN"));

    /* A connected stream reports the TCP state machine's own name, because
     * "CONNECTED" would collapse ESTABLISHED, FIN-WAIT and TIME-WAIT into one
     * row and hide exactly the states a socket gets stuck in. */
    sk.flags = HW_SOCK_STREAM | HW_SOCK_CONNECTED;
    sk.state = NET_TCPS_ESTABLISHED;
    assert(!strcmp(net_sock_state(&sk), "ESTABLISHED"));
    sk.state = NET_TCPS_SYN_SENT;      assert(!strcmp(net_sock_state(&sk), "SYN-SENT"));
    sk.state = NET_TCPS_TIME_WAIT;     assert(!strcmp(net_sock_state(&sk), "TIME-WAIT"));
    sk.state = 99;                     assert(!strcmp(net_sock_state(&sk), "STATE 99"));

    /* A UDP socket that is connected is still UDP, and "CONNECTED" for it
     * means a default peer, not a session. */
    memset(&sk, 0, sizeof sk);
    sk.flags = HW_SOCK_CONNECTED;
    sk.lport = 1234; sk.rport = 5678; sk.raddr = 0x7F000001u;
    assert(!strcmp(net_sock_kind(&sk), "UDP"));
    assert(!strcmp(net_sock_state(&sk), "PEERED"));

    /* An unbound, unconnected socket exists but is doing nothing, and saying
     * so is not the same as omitting the row. */
    memset(&sk, 0, sizeof sk);
    assert(!strcmp(net_sock_state(&sk), "IDLE"));

    /* ---- queue totals across the table ----------------------------------- */
    base();
    s.nsock = 3;
    s.sock[0].rx_queued = 5;  s.sock[0].tx_queued = 1;
    s.sock[1].rx_queued = 0;  s.sock[1].tx_queued = 7;
    s.sock[2].rx_queued = 11; s.sock[2].tx_queued = 0;
    net_reset(&st);
    net_sample(&st, &s, 1000000000ull);
    assert(net_queued_rx(&st.prev) == 16);
    assert(net_queued_tx(&st.prev) == 8);

    /* ---- an absent interface is reported as absent ----------------------- */
    base();
    s.nif = 0;
    net_reset(&st);
    net_sample(&st, &s, 1000000000ull);
    assert(!strcmp(net_link_note(&st.prev),
                   "NO VIRTIO-NET DEVICE: BOOT WITH -device virtio-net-pci"));
    base();
    net_sample(&st, &s, 1000000000ull);
    assert(!strcmp(net_link_note(&st.prev), "LINK UP"));
    s.iface[0].ready = 0;
    net_sample(&st, &s, 2000000000ull);
    assert(!strcmp(net_link_note(&st.prev), "DEVICE PRESENT BUT NOT READY"));

    /* ---- tabs ------------------------------------------------------------ */
    net_set_tab(&st, NET_TAB_SOCKETS);
    assert(st.tab == NET_TAB_SOCKETS);
    net_set_tab(&st, 42);
    assert(st.tab == NET_TAB_SOCKETS);

    printf("net_deck: address and MAC formatting, bandwidth deltas with zero and "
           "backwards intervals, socket decoding, queue totals PASS; state=%zu bytes\n",
           sizeof st);
    return 0;
}
