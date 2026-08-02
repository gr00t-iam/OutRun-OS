# OutRun OS v0.68.0-metal — TCP on the wire

Milestone 68. The segments v0.67 exchanged between endpoints inside this kernel
are now real frames that another machine could receive.

## WHAT WAS MISSING, STATED AS v0.67 STATED IT

v0.67's changelog listed, first among its limits:

> **No wire transmit.** `tcp_output` builds a `struct tseg`, not an on-the-wire
> header with offsets, options and a checksum. Loopback would compute that only
> to discard it. Real TCP on the NIC is the next milestone, not a line in this
> one, and it is marked absent rather than half-written.

This is that milestone. `tcp_output` now builds Ethernet + IPv4 + TCP and hands
it to `vnet_tx` for any peer that is not loopback, and inbound TCP frames are
decoded and fed to the same state machine.

## THE CHECKSUM IS THE WHOLE DIFFICULTY

This could not copy what `net_tx_udp` does, and the reason is worth recording.
IPv4 UDP may leave its checksum **zero** to mean "not computed", and the
existing datagram path does exactly that. TCP has no such escape: a zero
checksum is simply a *wrong* checksum, and every receiver on earth discards the
segment.

It also covers a **pseudo-header** — source address, destination address,
protocol, TCP length — not just the segment. That is what binds a segment to
the addresses it travelled between, and what stops a perfectly well-formed
segment being accepted for the wrong connection.

This is precisely the class of bug loopback cannot find. A checksum that is
wrong in every direction is invisible to a test where both endpoints are this
kernel and neither one checks. It would have appeared for the first time
against real hardware, as silence.

## VERIFYING A WIRE FORMAT WITHOUT A WIRE

The trap is obvious once named: gate the test on a reply from QEMU's SLIRP
gateway and you are testing QEMU's NAT timing, which is the trap v0.52 avoided
for datagrams and v0.51 for audio. Gate it on nothing and the encoder is
verified only against its own decoder.

So the suite does three separable things:

1. **Round trip through the real codec.** A segment is encoded by
   `tcp_wire_build` and decoded by `tcp_wire_parse`; every header field, both
   addresses and 300 bytes of payload must survive byte for byte. Both halves
   are the ones the NIC path actually uses.
2. **The IPv4 header checksum verifies over itself** — `net_cksum16` of the
   header must be zero, which is the standard property and independent of the
   encoder's own arithmetic.
3. **Corruption is rejected.** One flipped bit in the payload must fail the
   checksum, and the same frame must parse again once the bit is restored.
   Without this pair the checksum could be decoration: a parser that ignored it
   would pass every other assertion here.

And separately, **a frame is genuinely transmitted**: the driver makes one
non-blocking `connect` to a non-loopback address, which forces the real encoder
and `vnet_tx`, and the kernel half asserts the transmit counter moved. Whether
anything answers is deliberately not asserted.

### One frame — and it still broke something

`cmd_capdma` proves a confined device is **blocked by the IOMMU** when it
attempts kernel DMA, and it needs a quiescent NIC transmit queue to produce
that fault. That is exactly why `netstrs` has never sent a real frame and says
so in v0.52's changelog.

Giving `tcpstrs` one genuine transmit was enough. **A single SYN made capdma's
confined-DMA assertion fail under VT-d**, while both BIOS configurations stayed
clean — the failure was invisible on two thirds of the matrix, which is the
argument for running all three.

The fix is ordering, not a weaker assertion: the suite that needs silence runs
first. `tcpstrs` now runs after `capdma` and `nicdriver`. Weakening capdma to
tolerate traffic would have discarded the only test that proves the IOMMU
confines a device at all.

## RECEIVE

`net_dispatch` routed every IPv4 frame to `net_route`, which only understands
UDP. A TCP segment handed to it was silently ignored — inbound connections
would have disappeared without a trace. TCP is now dispatched first, by
protocol number, and a frame that fails to decode is counted and dropped: there
is no useful reply to a segment whose checksum failed, because its addresses
cannot be trusted either.

`net_rx_tcp` runs in the NIC bottom half, takes `g_net_lock` itself, and looks
the connection up by the same `(dport, saddr, sport)` precedence the loopback
path uses — established endpoint first, listener second.

## VERIFICATION

42 suites, 0 FAIL on uniprocessor/BIOS, SMP-4/BIOS, and q35 + VT-d IOMMU
(`-smp 4`). `tcpstrs` grows from 8 assertions to 16; no suite is added, so
every configuration's suite list is **identical** to its v0.67 baseline, with
`tcpstrs` moved to the end of the battery for the reason above. Boot
logs are in `docs/`.

### Warnings

46, all pre-existing; this release adds none.

### Not done

- **Nothing has been received from a real peer.** The decoder is verified
  against the encoder and against deliberate corruption; it has not yet parsed
  a segment produced by another implementation. That needs a peer this test
  environment can depend on, which SLIRP is not.
- **The Ethernet destination is broadcast** and there is no ARP resolution for
  TCP — inherited from `net_tx_udp`, which SLIRP accepts. A real switched
  network needs the real MAC.
- **No IP fragmentation or reassembly**, and no path MTU discovery. A segment
  is capped at a 512-byte MSS, well under any real MTU, so nothing fragments.
- Still no reassembly queue, no congestion control, no `TIME_WAIT` timer and no
  window advertisement — the window field is *emitted* (the receive buffer's
  free space) but not yet honoured on receipt. Emitting a truthful window and
  ignoring the peer's is the honest half-step; enforcing it is flow control,
  which belongs with congestion control.
