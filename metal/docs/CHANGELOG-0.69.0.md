# OutRun OS v0.69.0-metal — ARP: learning who is actually at an address

Milestone 69. The frames v0.68 put on the wire now carry the peer's real
hardware address, and this host answers when somebody asks for it.

## THE ADDRESS THAT WAS ALWAYS BROADCAST

v0.68 listed among its limits:

> **The Ethernet destination is broadcast** and there is no ARP resolution for
> TCP — inherited from `net_tx_udp`, which SLIRP accepts. A real switched
> network needs the real MAC.

SLIRP accepts a broadcast destination. A switch is under no obligation to, and
a real network is entitled to drop it. Worse, broadcasting every segment of
every connection is precisely the behaviour switching exists to eliminate — it
is not merely impolite, it does not scale past one conversation.

An eight-entry cache now maps IPv4 addresses to hardware addresses, and
`tcp_wire_build` addresses each frame to the peer it is actually for.

## THE THREE DECISIONS WORTH RECORDING

**Learn from any ARP frame, request or reply.** A host that is asking for us is
about to talk to us, so its mapping is the one we are most likely to need next;
learning it from the request saves the round trip we would otherwise spend
asking back. This is standard behaviour and it is not laziness.

**A changed MAC replaces the old one.** Machines move, interfaces fail over,
addresses get reassigned. A cache that refused to update would keep talking
confidently to somewhere nothing is listening — which is worse than not knowing
at all, because nothing reports an error.

**Broadcast is a fallback, not a failure.** A first segment to an unresolved
address still goes out broadcast, and fires an ARP request so the *next* one is
addressed properly. Dropping the segment instead would stall every new
connection for a full resolution round trip, and TCP would recover only by
retransmission timeout — correct, and needlessly slow.

## ANSWERING IS WHAT MAKES A HOST REACHABLE

Requests for this host's address are answered. Without that a peer can never
address a frame to us, and every inbound connection dies before it starts —
this is the half of ARP that has nothing to do with sending and everything to
do with being findable.

Requests for **anybody else** are not answered. Replying to every request on
the segment is how one host poisons a whole network, and the suite asserts the
negative case explicitly rather than trusting that it never happens.

## VERIFICATION

42 suites, 0 FAIL on uniprocessor/BIOS, SMP-4/BIOS, and q35 + VT-d IOMMU
(`-smp 4`). `tcpstrs` grows from 16 assertions to 21; no suite is added, so
every configuration's suite set is identical to its v0.68 baseline. Boot logs
are in `docs/`.

Every ARP assertion is driven by a frame the suite **constructs and hands to
the real handler**, so none of it depends on SLIRP replying:

- the request this kernel builds is a well-formed Ethernet ARP request, checked
  field by field rather than by length alone;
- a synthetic reply is **learned**, with the MAC intact;
- a TCP frame to that peer is then addressed **to the learned MAC, not
  broadcast** — which is the entire point, and the one assertion that would
  still pass if the cache were written but never consulted;
- a request for this host **is** answered;
- a request for somebody else is **not**.

That last pair matters together. A stack that answered everything passes the
positive test and fails the negative one; a stack that answered nothing passes
the negative and fails the positive. Only one behaviour passes both.

### Warnings

46, all pre-existing; this release adds none.

### Not done

- **No cache expiry.** Entries live until evicted by pressure (least recently
  seen, eight slots). Real ARP ages entries out so a stale mapping cannot
  outlive the machine that taught it; here a moved host is corrected only when
  it speaks again.
- **No gratuitous ARP on startup**, so nothing on the segment learns about this
  host until it either speaks or is asked for.
- **Still nothing received from a real peer.** The decoder is verified against
  frames this kernel builds and against deliberate corruption; it has not yet
  parsed a segment produced by another implementation. That needs a peer this
  environment can depend on, which SLIRP is not.
- Still no reassembly queue, no congestion control, no `TIME_WAIT` timer, and
  the receive window is emitted truthfully but ignored on receipt.
