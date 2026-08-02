# OutRun OS v0.65.0-metal — a socket is a descriptor

Milestone 65. Non-blocking sockets, datagram sessions, and network readiness
folded into the v0.64 epoll engine.

## THE THING THAT WAS IN THE WAY

The task asked to "wire socket rx/tx ring readiness into `ep_poll_fd`". That
sentence was not expressible against the v0.52 socket layer, and the reason is
worth stating plainly because it shaped the whole release:

**Sockets were not descriptors.** `SYS_SOCKET` returned an index into
`g_sock[16]`, a namespace disjoint from the `g_ofiles[16]` every other I/O
object lives in. Socket 3 and file 3 were different objects. epoll watches
descriptors, so it could not *name* a socket, let alone poll one. `SYS_CLOSE`
could not release one either — the only thing that ever reclaimed a socket was
its owner dying.

So the first and largest part of this milestone is not epoll work at all. A
socket now has an `ofile` entry (`VOL_SOCK`), and inherits from machinery that
was already tested: fork inheritance, owner-mask refcounting, `SYS_CLOSE`,
force-close-on-exit, and — the point — a name epoll can watch. This is the same
argument v0.64 made for epoll instances and eventfds, applied to the one I/O
object that had been left out.

Two consequences fell out for free:

- **`SYS_CLOSE` now works on a socket**, and a closed socket is EBADF to every
  socket call.
- **A thread can use its process's sockets.** `g_sock[i].owner` was compared
  against `current_proc_idx`, the same per-thread-slot bug v0.64 fixed for
  `g_ofiles`. Ownership now resolves through `fd_owner()` to the thread-group
  leader, so a worker thread can serve a socket its leader opened — the shape
  of every threaded server, and impossible before.

`net_teardown_kproc` is demoted to a safety net. It runs *before*
`descriptor_teardown_kproc`, so freeing by owner there would yank a socket out
from under a forked sibling still holding an inherited descriptor. It now
reclaims only sockets no descriptor names — the window between claiming a
`g_sock` slot and claiming its `ofile`, which a fault in between could strand.

## WHAT "CONNECTION" MEANS HERE, AND WHAT IT DOES NOT

The task named `SYS_ACCEPT`, listening sockets, and `EPOLLHUP` on "remote
connection reset". **There is no TCP in this kernel** — no handshake, no
sequence numbers, no retransmit, no RST. Writing one is a milestone of its own,
not a sub-item of an epoll integration, so none was written and none is
pretended.

What is implemented instead is honest and is what the milestone actually
tests. A **session** is a peer: the first datagram from an `(addr,port)` a
listener has not heard from is a session request, and `accept()` turns it into
a socket of its own, already connected to that peer, **carrying that first
datagram in its receive ring**. A server therefore reads the client's opening
message from the accepted socket rather than the listener, exactly as a stream
server would — and unlike a bare `recvfrom` loop, it can attribute it.

`EPOLLHUP` means the listener behind a session closed, which is locally
observable and therefore testable. It does not mean a peer reset a connection,
because no connection exists to reset.

## SYSCALLS

| # | call | notes |
| --- | --- | --- |
| 35 | `SYS_SOCKET(domain, type, proto)` | **now returns a descriptor**, not a `g_sock` index. `type` may carry `SOCK_NONBLOCK`. |
| 80 | `SYS_FCNTL(fd, cmd, arg)` | `F_GETFL` / `F_SETFL`, `O_NONBLOCK` the only settable bit |
| 81 | `SYS_LISTEN(fd, backlog)` | marks a bound socket as one that hands out sessions |
| 82 | `SYS_ACCEPT(fd, peer_out, flags)` | a new descriptor, or `-EAGAIN`. `peer_out` is two 32-bit words so neither address nor port is truncated. |

`O_NONBLOCK` lives on the **descriptor**, not the socket, because POSIX puts it
on the open file description — after a fork two processes name one socket
through two descriptions and are entitled to disagree about blocking. Putting
the flag on the socket would make one process's `fcntl` silently retune the
other's.

Syscalls 1–79 are unchanged and the pre-compiled VFS binaries run unmodified.

## THE NON-BLOCKING CONTRACT

- **`SYS_RECV`** returns `-EAGAIN` the instant the ring is empty when
  `O_NONBLOCK` is set. Blocking recv keeps the v0.52 bounded poll returning 0
  on timeout, unchanged, because `netstrs` and every existing caller are
  written against it.
- **`SYS_ACCEPT`** returns `-EAGAIN` when no peer is waiting. It claims the
  child's socket slot *before* dequeuing the peer, so a full socket table
  leaves the request queued for a later accept rather than consuming and
  discarding a client's first datagram.
- **`SYS_CONNECT` never returns `-EAGAIN`**, whatever `O_NONBLOCK` says. A
  datagram connect records a default peer and exchanges nothing, so it cannot
  block; reporting "in progress" for something already finished would be a lie
  a caller would then wait on.
- **`SYS_SEND`** is where the one piece of real backpressure in this system
  lives. A receiver's ring is four deep and `net_sock_enqueue` drops silently
  past that. A silent drop is legal for UDP and useless as a non-blocking
  contract, so a full destination is now reported as `-EAGAIN`. It is reported
  whether or not `O_NONBLOCK` is set, because there is nothing to block *on* —
  no transmit queue drains in the background here, and on a uniprocessor a
  blocking sender would be waiting for a receiver it is itself preventing from
  running.

## READINESS, AND THE DIRECTION THE LOCKS ALLOW

`ep_poll_fd_locked` gains a socket branch that asks the socket layer. Readiness
is computed from the ring counters a following `recv` or `accept` would
consult, so it cannot disagree with them — the same rule as pipes and eventfds.

- **`EPOLLIN`**: a datagram is queued; or, on a listener, a peer is waiting to
  be accepted; or the session's listener is gone (end of conversation is
  readable for the same reason end of file is on a pipe — a reader must be
  woken to *learn* nothing more is coming).
- **`EPOLLOUT`**: the socket has a destination. An unconnected socket reporting
  writable would invite a send that can only fail. A listener is never writable;
  it has nothing to send.
- **`EPOLLERR` / `EPOLLHUP`**: sticky error, or the listener behind a session
  closed.

The lock ranks decided the shape. `g_ofile_lock` is rank 1 and `g_net_lock` is
rank 9, so **asking** (descriptor → socket) climbs and is fine, while
**answering** — waking an epoll waiter from the enqueue path — would descend
and is a rank inversion. So the send path identifies which descriptor became
ready under the net lock and calls `ep_notify_fd` after releasing it. Same
discipline as v0.64's pipe and eventfd notifies, forced here by the ranking
rather than chosen.

### One routing bug this surfaced

After an accept, the listener **and** its session child are both bound to the
same local port, so `net_find_bound`'s "first socket bound to this port" stopped
being a usable answer — a second client's packets would land in the first
client's socket. Delivery now resolves by peer identity first (established
session), then listener, then plain bound socket. All of it went into one
resolver that both delivery and the send-side full check call, so the two
cannot drift; the previous duplicated "is it full" test was a second copy of
routing logic waiting to disagree with the first.

## VERIFICATION

| config | suites | FAIL |
| --- | --- | --- |
| SMP-4 / BIOS | 40 | 0 |
| q35 + VT-d IOMMU (`-smp 4`) | 42 | 0 |
| uniprocessor / BIOS | 40 | **intermittent — see below** |

Each configuration differs from its v0.64 baseline by exactly one line —
`[netepollstrs]`. Boot logs are in `docs/`.

### The uniprocessor result is NOT clean, and this release is not tagged

One UP boot in three fails `posixstrs` round `'std fd table'` with driver exit
964: `oread()` on the VFS file `motd` returned no bytes, in all three rounds of
that boot. The other two UP boots pass 11/11, and SMP-4 and IOMMU — which
exercise every path this milestone touched harder, on four cores — are clean.

**It is not root-caused, so `v0.65.0-metal` is deliberately not tagged.** What
is known:

- The failing assertion is a VFS file read. Nothing in this milestone touches
  the VFS, CAS or block layer.
- `posixstrs` runs at position 13; `netepollstrs` at 24. The new suite cannot
  reach it.
- `motd` is byte-identical in the failing and passing boots (same hash, same
  copy-on-write repoint at the same log line).
- No descriptor was force-closed in either boot, and `netstrs` reports 7/7 in
  both.
- The plausible new coupling is that sockets now consume `ofile` slots — the
  table is 16 entries and global, and `netstrs` (position 10) allocates
  descriptors where it previously allocated none. That would be a real
  regression introduced here. It does not obviously explain *this* symptom,
  because a failed `open` makes the driver skip the read rather than fail it,
  but it is the first thing to rule out.
- `posixstrs` has shown load-sensitive failures before (v0.63, SMP, exit 702).
  That precedent is a reason to suspect the environment, not a reason to record
  this as a flake — v0.61 recorded a genuine `g_execbuf` race as a "pipestrs
  flake" and it cost a milestone to find.

Next step is to instrument the `ofile` table occupancy at the point `posixstrs`
runs and compare a failing boot against a passing one, then re-run the UP
config enough times to establish whether the rate changes with v0.64's build.

All of `netepollstrs` is loopback, deliberately: the loopback path is
synchronous, so a failing assertion means the mechanism is wrong rather than
that SLIRP was slow. The same discipline v0.52 used for `netstrs` and v0.51 for
audio. A round trip through QEMU's NAT would test QEMU's timing.

The checks ring 3 cannot make about itself are the ones that matter, and they
live in the kernel half:

- **The EAGAIN path was genuinely taken** (`g_net_eagain` moved). A driver that
  never reached a would-block condition would pass every assertion it makes
  about itself while testing a blocking socket.
- **A waiter parked rather than spun** (`g_epoll_parks` moved) **and was woken
  by the datagram, not its deadline** (`g_futex_timeouts` did not move). This is
  the headline: a spinning waiter satisfies every ring-3 assertion and is still
  wrong.
- **Every socket was reclaimed with its descriptor** — the socket table and the
  descriptor table must agree. Those two numbers disagreeing is precisely the
  bug a side table keyed by process would have made permanent.

Ring 3 proves the semantics: an idle listener is not readable (a listener that
claimed readiness with no peer sends a server into an accept loop that spins on
EAGAIN forever), the peer's first datagram arrives *with* the accepted socket,
an edge is reported once and not twice, send refuses a full receiver rather than
dropping, and closing a listener leaves its session reporting
end-of-conversation rather than dangling.

`netstrs` passes unchanged on the refactored layer, which turns the existing
suite into a live test of the replacement rather than a museum piece.

### Warnings

46, all pre-existing; this release adds none. It removes one dead helper
(`net_find_session`, made redundant by the unified resolver) rather than
silencing it.

### Not done

- **No TCP.** No handshake, no reliability, no ordering, no reset. `accept`
  hands out datagram sessions and the changelog says so rather than letting the
  syscall name imply otherwise.
- **Real inbound frames still do not reach sockets.** `net_deliver_locked` is
  driven only by the loopback path; `net_dispatch` handles ARP/ICMP and does not
  feed `g_sock`. That was already true in v0.52 and is unchanged here — wiring
  it up is a NIC-side change with its own verification problem (SLIRP timing),
  not a readiness change.
- **`EPOLLOUT` carries no queue depth**, because the virtio TX path takes the
  frame synchronously. It says "your send will be accepted", which is true,
  rather than inventing a watermark the hardware does not have.
- `SOCK_BACKLOG` is 4 and `SYS_LISTEN`'s `backlog` argument is accepted and
  clamped rather than honoured exactly; sizing the queue per socket would be a
  parameter with no effect behind it.
- `VFS_MAXFILES` is still 64 and still nearly spent — unchanged since v0.60.
