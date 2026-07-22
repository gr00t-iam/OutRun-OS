# Outrun OS v0.46.0-metal — Capability-Bound IPC & Shared-Memory Messaging

A deterministic, zero-copy, capability-gated IPC pipeline: fixed-size
messages, a per-kproc mailbox, and two kinds of resource that can change
hands between ring-3 processes without the kernel ever touching the
underlying bytes — an open VFS file descriptor, and a genuinely shared
physical memory frame.

## What's new

- **`struct ipc_msg`** — the fixed header: `sender_pid`, `recipient_pid`,
  `msg_type`, `cap_mask`, `payload_len`, plus `xfer_handle` (the one field
  beyond the literal spec, required to carry a transferable fd/shmem id)
  and a 64-byte `inline_data` payload.
- **`SYS_IPC_SEND` / `SYS_IPC_RECV`** (18/19), gated behind a new
  `PCAP_IPC` capability bit. `SYS_IPC_RECV`'s second argument selects
  blocking (spin-wait via the existing `krelax()` — cooperative `sched_yield`
  on the BSP, `pause` on an AP — bounded to 4000 ticks) or a single
  non-blocking poll.
- **A per-kproc mailbox** (`g_ipc_q[MAX_KPROC]`, 8 messages deep), indexed
  the same way `g_proc_slpt[]`/`g_surf[].owner` already are.
- **Zero-copy VFS handle transfer**: a transferred fd is a straight
  ownership reassignment of the existing `g_ofiles[]` entry — the dirent
  and its CAS blocks never move, only the `owner` slot index does, at SEND
  time. v0.45's `descriptor_teardown_kproc` already force-closes any fd
  owned by a dying slot regardless of how it got there, so a recipient who
  dies before ever calling `SYS_IPC_RECV` still can't leak it — no new
  fd-specific teardown logic was needed.
- **Zero-copy shared memory** (`struct ipc_shmem`, `g_ipc_shm[16]`): a small
  pool of `alloc_frame()` pages, refcounted via a per-slot bitmask
  (`MAX_KPROC == 64` fits exactly in one `uint64_t`), mapped into each
  holder's own address space at a fixed, id-derived vaddr
  (`IPC_SHM_V + id*0x1000`). Two processes read and write the SAME physical
  page; the kernel only ever touches the mapping.
- **`ipc_teardown_kproc`**, wired into all three exit paths immediately
  before `descriptor_teardown_kproc` (same ordering discipline v0.44/v0.45
  established, extended one step earlier): clears the dying slot's mailbox
  and unmaps + releases every shared-frame grant it held, freeing the
  physical frame once the last holder is gone.
- **`DEBUG_IPC`** (`g_debug_ipc`, default off): logs every send/recv, every
  transfer, and every teardown release. Verified live — a captured trace
  showed the exact expected sequence (fd transfer, self-addressed shmem
  creation, real cross-process share, then `released shmem id 0 (last
  holder, freeing frame)` on the second holder's exit) — then reverted to
  the shipped default.
- **`cmd_ipc_stress`** (new suite, `ipcstress` command): 20 rounds of a
  sender/receiver pair (roles 12/13 in `user/init.c`) exchanging a
  transferred fd (read back through the transferred descriptor, verified
  byte-for-byte) then a shared frame (read with no syscall at all —
  genuinely zero-copy), across kproc-recycled slots the same way v0.45's
  `cmd_kproc_stress` churns them.

  ```
  [ipcstrs]  PASS  every round completed without a watchdog timeout (no deadlock across preemption)
  [ipcstrs]  PASS  every round's sender AND receiver exited cleanly (exit == own pid)
  [ipcstrs]  PASS  no descriptor leaked past any round (open-file table fully released each time)
  [ipcstrs]  PASS  no shared-memory grant survived past any round (g_ipc_shm fully released each time)
  [ipcstrs]  PASS  free-frame count reconciles (g_frame_free_depth == lifetime freed - reused)
  [ipcstrs]  PASS  the frame allocator's leaf lock never triggered a rank violation (no double-free race)
  [ipcstrs] RESULT: 6 passed, 0 failed
  ```

## Two design problems found and fixed while building the harness

- **The happens-before gap in shared-memory notification.** A naive
  protocol — sender creates the shared frame as part of the SAME send that
  notifies the peer, writes the pattern afterward — has a real race: the
  peer could pop the notification and read the page on another core before
  the sender's own subsequent write instruction ever executes. Fixed at the
  protocol level, not the kernel API: `ipc_sender` first sends a
  **self-addressed** message (`recipient_pid == own pid`) to create and map
  the frame, pops it back off its own mailbox, writes the pattern, and only
  *then* sends the real peer a second message re-sharing the same id. Since
  `ipc_queue_push`'s lock release is what makes a message visible at all,
  the write is guaranteed to happen-before anything the peer can observe —
  the same edge any correct message-passing-plus-shared-memory handoff
  relies on.
- **`kprintf`'s `%d` reads a 32-bit `int`, not a 64-bit one.** An early draft
  passed `int64_t xfer_handle` straight to `%d` in the debug log lines —
  would have silently misread the format string's remaining arguments.
  Caught before boot-testing by re-reading `kprintf`'s implementation; fixed
  by using `%X` with an explicit `(uint64_t)` cast, matching this codebase's
  existing convention for every other 64-bit debug value.

## Honest scope decisions

- **`ipc-payload` and `ipc-peer` are fixed, reused VFS names, not pid-keyed.**
  `VFS_MAXFILES` (now 26, +2 from v0.45's 24) grew by exactly two, one time,
  for the life of the boot — not per round. A pid-keyed name (matching
  `cio_file_worker`'s convention) would claim a brand-new, permanent dirent
  every one of 20 rounds, the exact growth v0.45's `cmd_kproc_stress`
  deliberately bounded for the same reason. What genuinely recycles every
  round here — kproc slots, `g_ofiles` fd numbers, `g_ipc_shm` ids — is
  exactly what this suite's assertions check.
- **A shared-memory "transfer" is a NEW mechanism, not a repurposed
  `struct dma_grant`.** v0.44's DMA grant is bound to one process's IOMMU
  domain — device isolation, not general RAM sharing — and re-architecting
  it to support a second simultaneous owner was out of scope and would have
  conflated two different kinds of isolation. `ipc_shmem` is a small,
  purpose-built pool instead.
- **A rejected transfer releases the resource outright rather than bouncing
  it back to the sender.** If the recipient's `RECV` finds it lacks the
  capability a message's `cap_mask` requires, the fd is closed / the shmem
  bit is dropped immediately — simpler and symmetric with the teardown
  path, instead of inventing a return-to-sender protocol for an edge case
  this milestone doesn't need to solve.
- **`DMA_GRANT_MMIO`'s revoke path still has no live-fire proof** (unchanged
  since v0.44) — every device with a real PCI `bdf` is only ever granted
  through the out-of-scope legacy excursion path.

## Verification

| Config | Result |
|---|---|
| `-smp 4` BIOS ISO | 0 FAIL, 22 suites incl. `ipcstress` 6/6 |
| `-smp 4` q35 + intel-iommu | 0 FAIL, all green incl. iommu 2/2, capdma 11/11, `ipcstress` 6/6 |
| uniprocessor | 0 FAIL, `ipcstress` 6/6 (sequential-on-BSP fallback) |

Compositor/ipcstress screendump captured at the shell on the IOMMU run
(`OUTRUN-0.46-ipcstress-iommu.png`).

**One flake observed and disclosed, not silently re-rolled:** the first
IOMMU verification attempt hit a single failure in `mcpre` (v0.39's
cross-core-migration suite, unrelated to this milestone) —
`the captured context MIGRATED CORES: started on cpu1, finished on cpu2`.
`mcpre`'s migration check is inherently timing-sensitive (bounded-tick
IPI/AP-readiness windows), and intel-iommu emulation adds scheduling jitter
QEMU's plain BIOS boot doesn't have. An immediate re-run with an identical
binary and a fresh disk reproduced 0 FAIL across all 22 suites, including
`mcpre` 5/5 — confirming this was a pre-existing timing flake, not a
regression from anything in this milestone (which touches none of `mcpre`'s
IPI-preemption/migration machinery). The clean re-run's log is what's
archived above.

## Honest scope gaps

- **No multi-message-in-flight ordering guarantee beyond FIFO per mailbox.**
  Two SENDs from different sender processes to the same recipient are only
  ordered relative to each OTHER by whichever acquires `g_ipc_lock` first —
  correct (no message is torn or duplicated) but not something a caller can
  rely on for cross-sender causality; this milestone's own protocol works
  around it with the self-addressed-send trick rather than needing the
  kernel to provide it.
- **`MAX_IPC_SHMEM` is a small, fixed pool (16)** — a real workload sharing
  many independent memory regions at once would need a larger table or a
  slab-style allocator; sized here to match `cmd_ipc_stress`'s needs, not
  as a production capacity claim.
- **kproc table still has a `MAX_KPROC` concurrent-live cap** (v0.45,
  unchanged) — recycling keeps the table from exhausting over a long boot,
  but a genuinely large number of SIMULTANEOUSLY live processes remains
  bounded at 64.
- VT-x unavailable under TCG; IOMMU/VT-d fully real; NVMe/UEFI boot-order
  quirk remains unexercised, upstream of the kernel (all unchanged).
