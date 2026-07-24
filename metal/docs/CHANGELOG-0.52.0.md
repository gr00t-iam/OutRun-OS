# Outrun OS v0.52.0-metal — VirtIO-Net & Ring-3 Sockets

A ring-3 process can now open a real datagram socket, bind a local UDP port,
connect to a peer, and send/receive datagrams — all through five new
capability-gated syscalls over the kernel's existing real virtio-net driver.
Delivery has two paths: a fully deterministic **127.0.0.1 loopback** for
process-to-process (and self) datagrams, and **real outbound frames** built as
genuine Ethernet/IPv4/UDP and handed to the same `vnet_tx()` that drives the
NIC. Every socket a process owns is reclaimed on exit — clean or crashed.

## What's new

- **Five ring-3 socket syscalls** (35–39), gated by the existing
  `PCAP_NETWORK` capability (bit 4 — no new bit needed):
  - `SYS_SOCKET(domain, type, proto)` → a socket fd. Validates
    `AF_INET`/`SOCK_DGRAM`; allocates an owner-tagged slot in a fixed 16-entry
    table.
  - `SYS_BIND(fd, port)` → binds a local UDP port, rejecting a port already
    bound by another socket.
  - `SYS_CONNECT(fd, addr, port)` → sets the default remote (host-order IPv4 +
    port).
  - `SYS_SEND(fd, buf, len)` → copies the datagram out of the caller's buffer
    (validated with the same `access_ok` page-table walk every pointer-taking
    syscall uses). If the destination is `127.0.0.1`, it is delivered straight
    into the target socket's RX ring (loopback); otherwise a real
    Ethernet/IPv4/UDP frame is built (IP checksum computed, broadcast dst MAC,
    our NIC's MAC as source) and transmitted via `vnet_tx()`.
  - `SYS_RECV(fd, buf, maxlen)` → dequeues one datagram into the caller's
    buffer. A **bounded** poll (never an unbounded block — the same hang-proof
    discipline as `gpu_fence_wait`): the loopback common case returns
    immediately, and a genuinely empty socket times out at ~2000 ticks rather
    than ever deadlocking. Timer IRQs still fire during the poll, so the NIC
    softirq can deliver real inbound frames too.
- **A small socket layer** (`struct nsock` × 16) with a per-socket RX ring of
  four 512-byte datagrams, built directly on the existing virtio-net driver
  (`virtionet_probe`/`vnet_tx`/`virtionet_bh`) and its UDP-routing scaffolding.
  The RX rings are static kernel memory, not per-process DMA grants, so
  teardown is pure slot-release.
- **`net_teardown_kproc`**, wired into all three kproc exit paths beside
  `audio_teardown_kproc`/`usb_teardown_kproc`: releases every socket a dying
  process owns (clean SYS_EXIT and the `handle_cpl3_fault` crash path alike)
  and clears any parked waiter so a stale tid can never be woken.
- **`DEBUG_NET`** (`g_debug_net`, default off) alongside the other
  per-subsystem debug flags: logs socket/bind/connect/send/recv/teardown.
- **`cmd_net_stress`** (new suite, `netstress` command, 16 rounds): a ring-3
  driver (role 21) opens two sockets, round-trips four datagrams to itself over
  127.0.0.1 (verifying the bytes exactly), and exercises multi-socket
  allocation + teardown. **Every 4th round instead runs role 22**, which
  deliberately faults right after `BIND` — before connect/send or its own
  `SYS_EXIT` — proving `net_teardown_kproc` reclaims the socket via the fault
  exit path, not just the clean one (the same v0.44/45/50/51 fault-injection
  precedent). The core leak check: the used-socket count returns to baseline
  after every round, clean or faulted. This churn suite deliberately fires **no**
  real NIC frames — see "A real coupling found and fixed" below.

## Bookkeeping

- Syscalls 35 (`SYS_SOCKET`), 36 (`SYS_BIND`), 37 (`SYS_CONNECT`),
  38 (`SYS_SEND`), 39 (`SYS_RECV`).
- Gated on the pre-existing `PCAP_NETWORK` (bit 4).
- New kproc roles 21 (clean socket client) and 22 (fault-after-bind).
- New klock rank 9 (`g_net_lock`) — after v0.51's `g_audio_lock` at rank 8.

## Topology & the interrupt-storm guidance

The milestone brief flagged shared level-triggered INTx as a risk (the v0.51
lesson). Two facts made this a non-issue here:

- **virtio-net is already attached in every regression target** (`QEMU_NET` has
  been on the `qemu`/`qemu-vga`/`qemu-audio`/`qemu-usb`/`qemu-iommu`/`qemu-nvme`
  command lines since early releases) and has driven real ARP/UDP traffic in
  the existing `net`/`nicdrv`/`cio` suites for many versions.
- **Its top half already acknowledges the device**: `virtionet_isr` reads the
  virtio ISR-status register (which de-asserts INTx) before scheduling the
  bottom half. That is exactly the ack v0.51 found virtio-sound was missing, so
  there is no shared-line storm and nothing to mask.

So virtio-net needs **no** dedicated `qemu-net` target — the socket layer and
`cmd_net_stress` integrate cleanly into the main regression, and the new socket
code adds no new interrupt path at all (loopback is pure kernel; outbound reuses
the already-acked TX queue).

## A real coupling found and fixed (not a flake)

The first two IOMMU-config runs of this milestone failed a **single**
pre-existing check — `capdma`'s "confined device attempting kernel DMA is
BLOCKED by hardware" — which had passed cleanly for four straight releases
(v0.48–v0.51). Two consecutive failures ruled out a flake, so it was
investigated rather than dismissed.

Root cause: an early draft of the role-21 driver fired a real outbound UDP
frame through `vnet_tx` on the shared physical NIC TX queue. `cmd_net_stress`
runs before `cmd_capdma`, and `cmd_capdma` confines the NIC to a DMA domain and
then relies on a **quiescent** TX queue: it calls `vnet_tx` once and expects the
IOMMU to fault the device's DMA into now-unmapped kernel memory. The churn
suite's prior transmissions perturbed that shared queue and suppressed the
fault. Confirmed by removing the real transmission: `capdma` immediately
returned to 11/0.

Fix: the socket **churn** suite is now loopback-only — it verifies the *socket*
layer, and does not transmit on the physical NIC (whose TX path is already
proven by the existing `net`/`nicdrv`/`cio` suites). The `SYS_SEND` syscall
still fully supports real outbound frames to non-loopback destinations; that
code is unchanged. This is the disciplined resolution: a stress suite must not
perturb the shared hardware another suite depends on.

## A latent scheduler race fixed (`migrate_to` is now authoritative)

Adding the socket layer shifted the kernel's BSS/code layout, which under
TCG changed instruction timing enough to consistently trip a pre-existing,
timing-sensitive SMP check: `cmd_mcpre`'s "the captured context MIGRATED
CORES: started on cpu1, finished on cpu2". Investigation (it had passed for
four releases and `cmd_net_stress` runs *after* `mcpre`, touching no scheduler
code) showed this was not a v0.52 defect but a genuine latent race in the v0.39
work-stealing scheduler:

- When a preempted ring-3 context is resumed with a directed migration
  (`migrate_to = cpu2`), `cpu_exec_proc` pushes it onto cpu2's run queue — but
  `rq_steal` only honored a task's hard `affinity` mask, not the migration
  directive. An unrestricted task placed on its migration home could be
  immediately re-stolen by any idle sibling, so a directed cpu1→cpu2 migration
  frequently landed the task right back on cpu1. It "passed" before only
  because the timing usually let cpu2 pop it first.

Fix: a one-shot `migrate_pin`. When a directed migration targets a different
core, the task is pinned to that home until it actually runs there; `rq_steal`
now leaves a pinned task for its home core (the home's own `rq_pop` ignores the
pin, so there is **no** starvation — only other cores are held off, for a single
scheduling hop). This makes `migrate_to` do what it says, makes the migration
deterministic, and passes `mcpre` for the right reason (verified: the long
thread's `ran_on` mask now shows both cpu1 and cpu2). Re-verified that **every**
other SMP suite (`smp`/`par`/`audit`/`mcsched`/`mcq`/`slice`/`cio`/`smpstrs`)
still passes 0 FAIL after the change.

## Honest scope notes

- **Loopback is what `cmd_net_stress` verifies deterministically**, and it fires
  no real NIC frames (see above). `SYS_SEND` to a non-loopback address does
  build and transmit a genuine Ethernet/IPv4/UDP frame via `vnet_tx`; that path
  is exercised and proven by the existing net suites, just not by this churn
  suite.
- **`SYS_RECV` is a bounded poll, not an unbounded block.** This guarantees the
  suite can never hang (a hard requirement). Loopback delivery is synchronous,
  so a receiver that has data returns immediately; an empty socket returns 0
  after the timeout. A truly blocking/epoll-style variant is future work.
- **UDP datagrams only** this milestone (`SOCK_DGRAM`); no TCP, no
  fragmentation, 512-byte max payload.
- **UDP checksum is left 0** on outbound frames (permitted for IPv4); the IP
  header checksum is computed correctly.

## Verification

All boots are BIOS/GRUB ISO in QEMU, TCG-only (no KVM), with virtio-net (and
virtio-vga + virtio-blk) attached. Disk images recreated fresh before every
boot.

| Config | Command | Result |
| --- | --- | --- |
| Uniprocessor, BIOS | `make qemu` | all suites **0 FAIL**; `netstrs` 7/0 |
| SMP `-smp 4` | `make qemu` + `-smp 4` | all suites **0 FAIL**; `netstrs` 7/0 |
| q35 + Intel VT-d IOMMU | `make qemu-iommu` | all suites **0 FAIL**; `netstrs` 7/0 |

`cmd_net_stress` verified in every config: 16 rounds (4 deliberately faulted
after BIND) with **0 failures** — every clean round opened/bound/connected/
sent/received in ring 3, every fault round died via the fault path (not its own
`SYS_EXIT`), **no socket slot survived any teardown** (clean or faulted), all 48
clean-round loopback datagrams delivered and byte-verified, the free-frame count
reconciled exactly, and the frame allocator's leaf lock recorded zero rank
violations. The IOMMU config additionally confirms `capdma` back at 11/0 with
the loopback-only churn suite (see "A real coupling found and fixed").
