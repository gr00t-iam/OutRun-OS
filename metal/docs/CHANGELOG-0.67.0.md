# OutRun OS v0.67.0-metal — TCP

Milestone 67. A three-way handshake, an ordered byte stream, retransmission of
what was lost, and an end that is announced rather than inferred.

## MAKING A WORD HONEST

v0.65 shipped `SYS_ACCEPT` and said plainly, in its changelog and in the kernel
source, that **there is no TCP in this kernel** — that a "connection" was a
datagram peer and `accept` handed out sessions, with no handshake synthesised
and none claimed. That was the right call then: writing TCP is a milestone, not
a sub-item of an epoll integration.

This is that milestone. `SOCK_STREAM` now means what it says.

Two releases of groundwork made it small rather than enormous. v0.65 made
sockets **descriptors** — so a stream gets `close`, fork inheritance, owner-mask
refcounting and an epoll-watchable name for free — and gave them a real
non-blocking contract. v0.66 exercised the same substrate again. What was left
was the protocol itself.

## THE STATE MACHINE

Eleven states, the POSIX set: `CLOSED`, `LISTEN`, `SYN_SENT`, `SYN_RCVD`,
`ESTAB`, `FIN_WAIT1`, `FIN_WAIT2`, `CLOSE_WAIT`, `CLOSING`, `LAST_ACK`,
`TIME_WAIT`. Sequence and acknowledgement arithmetic is **modular** —
`seq_lt`/`seq_le` on signed differences — because writing `a < b` on raw
`uint32` is correct for four billion bytes and then silently inverts, which is
a bug that appears under sustained load and never in a test.

`struct nsock` gained the stream fields rather than getting a parallel table.
One socket table means one teardown path, one descriptor binding, one place
`net_sock_count_used()` has to be right — and every datagram path keeps working
without knowing streams exist, because `stream` is zero for them.

**A connection is established before `accept`, not by it.** The handshake
completes inside `tcp_input`, and the finished connection joins the listener's
accept queue; `accept` only transfers ownership. Deferring the handshake until
the application calls `accept` would let a busy server time out its peers.

## WHERE THE SEGMENTS GO, AND WHY THAT IS THE HONEST CHOICE

Segments are exchanged over **loopback**, endpoint to endpoint inside this
kernel. That is a real link carrying real segments — the handshake, the
sequence space, the close sequence all happen exactly as they would on a wire —
and it is deterministic, which is the whole point. Gating pass/fail on a SLIRP
round trip would test QEMU's NAT timing, the trap v0.52 avoided for datagrams
and v0.51 for audio.

A loopback link is also lossless, which verifies retransmission **not at all**.
So the suite injects loss: `g_tcp_drop` discards the next N segments on the way
out, and the kernel half asserts both that the loss happened and that
retransmission recovered it. Without that, the retransmit path would be code
that has never once run while every test passed.

## SYSCALLS

No new ones. `SYS_SOCKET` accepts `SOCK_STREAM` (1); `listen`, `accept`,
`connect`, `send`, `recv` and `close` change behaviour by socket type. That is
the payoff for v0.65 making sockets descriptors — a stream needed no new
surface, only a new meaning behind the existing one.

`connect` is **the one call in this system that genuinely blocks**. v0.65
refused to invent `-EINPROGRESS` for a datagram connect that exchanges nothing
and cannot block; a stream connect exchanges a handshake with another endpoint,
so non-blocking `connect` now returns `-EINPROGRESS` and means it.

## THREE DEFECTS THIS FOUND IN ITSELF

**A hang, not a failure.** The first blocking `connect` spun on `g_ticks`
waiting for the handshake. That is a bet that interrupts are enabled underneath
the syscall, and when the bet is wrong the loop never terminates — the boot
stopped mid-suite rather than reporting anything. It now retransmits its own
SYN, bounded by *iterations* and yielding between them, so it cannot wedge
whatever the interrupt state is.

**The retransmit timer could not retransmit a SYN.** It resent the send buffer,
and a SYN is not in the send buffer — there is nothing to resend. A dropped SYN
was therefore unrecoverable, which the loss injection exposed immediately. The
timer now handles `SYN_SENT`, `SYN_RCVD` and a lone FIN as well as data.

**An accepted socket was never marked `connected`.** Every send path gates on
that flag, so the server could receive 1500 bytes and could not reply — which
reads as a transport bug and is really a missing assignment. It cost one test
cycle because the suite checks both directions rather than assuming that a
connection that reads must also write.

## EPOLL

Streams answer the readiness questions a stream has:

- **`EPOLLIN`** — bytes are readable; or a listener has a completed connection;
  or the peer sent FIN. End of stream is readable for the same reason end of
  file is on a pipe: a reader has to be woken to *learn* nothing more is
  coming.
- **`EPOLLOUT`** — established, with room in the send buffer. A half-open or
  unconnected stream reporting writable invites a send that can only fail.
- **`EPOLLHUP`/`EPOLLERR`** — FIN received, or the connection was reset.

## VERIFICATION

42 suites, 0 FAIL on uniprocessor/BIOS, SMP-4/BIOS, and q35 + VT-d IOMMU
(`-smp 4`). Each configuration differs from its v0.66 baseline by exactly one
line — `[tcpstrs]`. Boot logs are in `docs/`.

The assertions are written so that a stack which merely **copied bytes between
two sockets** would fail them:

- The payload is **1500 bytes — three segments at a 512-byte MSS**. A stack
  that preserved message boundaries, or lost anything at a segment boundary,
  produces the wrong bytes. The comparison is byte-for-byte, so recovery from
  the injected loss has to be *correct*, not merely eventual.
- An idle listener answers `-EAGAIN`; a drained stream is not readable; an
  established one is writable.
- `close` produces **end of stream** — `0`, distinct from `-EAGAIN` — and
  `EPOLLHUP`, rather than the peer waiting out a timeout.

The kernel half checks what ring 3 cannot see: that real segments moved in both
directions, that connections reached `ESTAB` through a handshake (both ends
count themselves), that the injected loss actually occurred, and that
retransmission is what recovered it.

### Warnings

46, all pre-existing; this release adds none.

### Not done — and these are the honest limits

- **No wire transmit.** `tcp_output` builds a `struct tseg`, not an on-the-wire
  header with offsets, options and a checksum. Loopback would compute that only
  to discard it. Real TCP on the NIC is the next milestone, not a line in this
  one, and it is marked absent rather than half-written.
- **No reassembly queue.** Out-of-order segments are dropped, not buffered —
  protocol-legal, since the peer retransmits, and adequate on a link that
  cannot reorder. This is the clearest difference between this and a production
  stack.
- **No congestion control.** No slow start, no congestion window, no Nagle, no
  delayed ACK. There is a fixed retransmit timeout and no round-trip estimate.
  On a lossless local link none of it changes an outcome; on a real network all
  of it would.
- **No `TIME_WAIT` timer.** The state exists and is entered; nothing leaves it
  on a clock, so a socket in `TIME_WAIT` is reclaimed with its descriptor
  rather than after 2×MSL.
- **No window advertisement.** Flow control is the receive buffer's size and
  the sender's own send buffer; a full receiver drops and the sender
  retransmits, rather than being told to stop.
