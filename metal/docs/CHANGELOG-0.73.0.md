# OutRun OS v0.73.0-metal — the three things TCP was missing

Milestone 73. v0.67 and v0.68 each ended with a list of what they had not
done. This release takes three items off it: a `TIME_WAIT` that ends on a
clock, a receive window that is honoured rather than merely advertised, and
out-of-order segments that are kept rather than discarded.

Nothing here is new capability in the sense of a new syscall. It is the
difference between a stack that works on a link that never loses or reorders
anything, and one that behaves correctly when the link misbehaves.

## TIME_WAIT ENDS ON A CLOCK

v0.67 shipped the state and said so plainly:

> **No `TIME_WAIT` timer.** The state exists and is entered; nothing leaves it
> on a clock, so a socket in `TIME_WAIT` is reclaimed with its descriptor
> rather than after 2×MSL.

A socket entering `TIME_WAIT` now arms `tw_ticks`, and `tcp_timer_scan`
releases the connection state when it expires.

**The placement is the whole trick.** The scan's loop began with
`if (!s->rexmit_ticks) continue;` — a socket in `TIME_WAIT` has no
retransmission pending, so its `rexmit_ticks` is zero and the loop skipped it.
A timer added *after* that guard would have compiled, read correctly, and never
once fired. The check goes first, and the suite asserts the expiry rather than
the arming alone, so moving it back would fail rather than silently regress.

### The constant is not 2×MSL, and that is deliberate

`TCP_TIMEWAIT_TICKS` is 60 — 0.6 s at 100 Hz — against a real 2×MSL of 60 s.
MSL bounds how long a duplicate segment can wander a **wide-area** network.
The only links this kernel has are loopback and one emulated hop to its peer,
where a segment cannot outlive its connection by anything close to a minute.
A real 2×MSL here would test the scheduler's patience, not the protocol.

The number is wrong for the internet and right for this machine, and it is
written next to the constant rather than left for someone to discover.

## THE WINDOW IS HONOURED

v0.68 put the window on the wire and admitted in the same paragraph that it was
*"emitted but not honoured on receipt"*. Tracing it turned up why, and it was
further back than "we didn't get to it": **`tcp_wire_parse` never decoded the
field, and `struct tseg` had nowhere to keep it.** The encoder wrote a truthful
window into every segment and the decoder threw it away, so `tcp_input` had
never once seen a peer's window. There was nothing to honour.

`struct tseg` now carries `win`, both paths fill it, and `tcp_send_data` caps
what it sends by the window minus what is already in flight.

**Filling it on the loopback path mattered more than on the wire.** Loopback is
what the suite exercises; a version that decoded the wire field and left
loopback at zero would have passed every assertion against the NIC path and
done nothing at all where the tests actually run.

**Capping, not refusing.** A short `send()` is the POSIX contract and the
caller retries — by which time an ACK has usually reopened the window.
Refusing outright would convert flow control into a spurious error.

## OUT-OF-ORDER SEGMENTS ARE KEPT

v0.67 called this the clearest difference between this and a production stack:

> **No reassembly queue.** Out-of-order segments are dropped, not buffered —
> protocol-legal, since the peer retransmits.

Legal, but it turns one lost segment into a retransmission of everything after
it, because the sender learns nothing about what did arrive.

Four bounded slots per socket. A segment past the hole is stored and answered
with a **duplicate ACK naming `rcv_nxt`** — which is what tells the peer *which*
segment to resend. When the hole fills, the queue drains in a loop, because one
arrival can release several behind it.

A full queue drops exactly as v0.67 always did. The fallback is the old
behaviour, not a new failure mode — four slots is a bound on how much better
this gets, never a way for it to get worse.

## VERIFICATION

43 suites (45 on VT-d), 0 FAIL on uniprocessor/BIOS, SMP-4/BIOS and
q35 + VT-d IOMMU (`-smp 4`), each against a freshly formatted volume. No suite
is added; `tcpstrs` grows by 7 assertions. Boot logs are in `docs/`.

Each assertion drives the **real state machine** on a socket the block sets up
and releases itself, rather than reading back a field the test just wrote:

- a FIN arriving in `FIN_WAIT2` enters `TIME_WAIT` **with its clock armed** —
  the state transition is the protocol's, not the fixture's;
- and the state **ends on that clock**, which is the assertion that fails if
  the `tw_ticks` check is ever moved back below the retransmit guard;
- a 100-byte window caps a 300-byte send at exactly 100, and the stall counter
  moves — a cap that happened to equal the buffer limit would pass the first
  half and fail the second;
- a **zero** window stops the sender completely, which is what distinguishes
  flow control from a size limit;
- a segment past a hole is queued, `rcv_nxt` does **not** advance, and nothing
  is delivered;
- filling the hole releases what was behind it;
- and the reassembled bytes are **in the right order**, checked byte for byte —
  a stack that merely stopped dropping would pass the two assertions above and
  fail this one.

Five counters were added for what ring 3 cannot see: a `TIME_WAIT` that
expired, a send the window held back, and segments queued, merged and dropped.
Every one of these paths can compile and never execute; a counter that stays
zero is the only way the suite can tell the difference between working and
absent.

### The fixture returns what it takes

`tcpstrs_grab_sock` takes a socket from a shared 16-entry table and every
caller releases it in the same block. v0.72's `usersstrs` leaked five
descriptors and broke nine assertions across three unrelated suites twenty
minutes later; that lesson is applied here rather than relearned.

### Warnings

46, all pre-existing; this release adds none.

### Not done

- **No congestion control.** Still no slow start, no congestion window, no
  Nagle, no delayed ACK, and a fixed retransmit timeout with no round-trip
  estimate. Flow control is what the *receiver* can absorb; congestion control
  is what the *network* can carry, and this release adds only the first.
- **Duplicate ACKs are sent but not counted.** A real stack uses three of them
  as a fast-retransmit trigger; here they inform the peer and nothing acts on
  them at the sender.
- **The reassembly queue holds four segments** and does not coalesce adjacent
  ones or trim overlaps — a segment that partially overlaps what is queued is
  stored whole or dropped whole.
- **`TIME_WAIT` releases connection state, not the socket object**, which stays
  until its descriptor closes, because a descriptor must never dangle.
- **Still nothing received from a real peer.** Every assertion here is driven
  by segments this kernel constructs. That remains true until there is a peer
  this environment can depend on, which SLIRP is not.
