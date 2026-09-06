# v0.95.0 Objective 1 — PCI passthrough and device claim

Architecture specification. **No kernel or user-space code has been modified.**

Scope chosen from `Blueprints.md`'s "IOMMU & PCI Passthrough Initialization"
workstream. Written against `main` at `d4eac7a` (v0.94.0).

---

## 0. A note on where this scope came from

There is no `ROADMAP-0.95` in this tree and `Blueprints.md` carries no version
numbers, so nothing in the repository scheduled this milestone. The objective
was selected deliberately, from four evidence-backed candidates, and that is
recorded here so a later reader does not mistake it for a plan that already
existed. The other three — POSIX-1b `timer_create`, standing-debt closure
(the 100-iteration soak, the `append-oversub` deadline, `capdma`'s unexplained
used-ring signal, `smp2`/`smp8` tiers), and hardirq time accounting — remain
open and are not superseded by this.

---

## 1. Audit — what exists, and what the objective actually costs

Blueprints asks for a syscall that "detaches a high-performance GPU and its
audio controller from the host kernel, isolating it into a protected DMA
domain". Measured against the tree, that sentence spans four subsystems of very
different maturity.

### 1a. What is already built, and is genuinely reusable

| capability | where | state |
|---|---|---|
| IOMMU / VT-d bring-up | `iommu_init` (`4947`), `dmar_r32/w32/r64/w64` (`4723-4726`) | real; DMAR parsed, registers driven |
| Per-process DMA domains | `iommu_build_proc_domain` (`4804`), `iommu_attach_proc_domain(bdf, proc)` (`4842`), `iommu_detach_to_kernel(bdf)` (`4860`), `iommu_domain_add_page` (`4835`) | real, and already keyed by **BDF** — the exact granularity a device claim needs |
| Second-level page tables | `iommu_build_slpt(limit, levels, use_2m)` (`4730`) | real |
| Fault reporting | `iommu_read_fault` (`34396`), `iommu_clear_faults` (`34388`), `iommu_invalidate_all` (`34374`) | real — this is what makes a negative control observable |
| BAR mapping to ring 3 | `SYS_VFIO_MAP_BAR` (case 20, `18900`) | real, capability-gated on `PCAP_VFIO` **and** the device's own `d->req` |
| DMA grant table | `dma_grant_create/revoke` (`4881`/`2901`), `MAX_DMA_GRANTS 8` per process | real |
| Per-process teardown | `vfio_teardown_kproc` (`2859`) | real; releases IRQ lines on exit |
| CPU affinity mask | `kprocs[].affinity` (`1953`), `SYS_SETAFFINITY` (`19038`) | real; `0` = unrestricted |
| `smp4-iommu` gate tier | `gate-matrix.sh`, q35 + VT-d, `intremap=on` | real, and green at v0.94.0 |

The IOMMU half of this objective is largely **already present**. That is the
single most important audit finding, and it is why this objective is tractable
at all.

### 1b. What does not exist, stated plainly

- **`sys_claim_pci_device` — absent.** `grep -c claim_pci` is 0.
- **General PCI enumeration — absent.** The only bus walker is
  `pci_probe_virtio` (`3045`). There is no generic config-space read/write, no
  capability-list walker, and no way to address a device by
  `(domain, bus, slot, func)` at all. `struct kdev` (`MAX_KDEV 8`) is a
  hand-registered table populated at boot by `kdev_register`, and it stores
  `bdf` as `0xFFFF` for anything not explicitly wired.
- **MSI and MSI-X — entirely absent.** `grep -c "ioapic\|msi_"` is **0**. This
  is the finding that changes the shape of the objective and it deserves to be
  spelled out rather than discovered in week two:
  - Interrupts are 8259 PIC, remapped to vectors 32-47 (`pic_remap`, `436`).
  - **The IDT is 52 entries** (`idt[52]`, `408`): 0-31 exceptions, 32-47 PIC
    IRQs, 48-50 IPIs, 51 the LAPIC slice tick. There is no room for a single
    MSI-X vector without enlarging it.
  - There is no interrupt-vector allocator, no IOAPIC driver, and no MSI
    capability parser.
  - `SYS_VFIO_WAIT_IRQ` (case 21) is built on `g_vfio_irq_owner[line]` /
    `g_vfio_irq_seq[line]` with `MAX_VFIO_LINES 24` — a **legacy line** model,
    indexed by PIC IRQ, with no notion of a per-device message vector.

  MSI-X is therefore not an addition to an interrupt subsystem. It is the
  interrupt subsystem, and it is the majority of this objective's cost.
- **`struct kdev` has no owner field.** `{ name, base, len, req, used, bdf }`
  (`2810`). Ownership today lives out-of-band in `g_vfio_irq_owner[]`, keyed by
  IRQ line rather than by device — so two processes mapping two BARs of the
  same device are indistinguishable to the kernel.
- **No lock protects `kdevs[]`.** It is written once at boot and read
  thereafter, which is safe only because nothing mutates it at runtime. A claim
  syscall makes it mutable from any core.
- **Core isolation — absent.** `core_pin` is 0. `affinity` restricts where a
  task *may* run; it does not stop the scheduler putting *other* work on a core,
  which is what Blueprints' "prevent the host scheduler from interrupting those
  cores" asks for.

### 1c. One interaction v0.94 just created

`SYS_VFIO_WAIT_IRQ` waits by **spinning** — `while (seq == seen && ticks - t0 <
timeout) krelax();` — inside the syscall. As of v0.94 that loop is inside the
`syscall_trap` bracket, so every microsecond of it is now billed as **system
time**. A passthrough driver that spends its life waiting on device interrupts
will report close to 100% stime and almost no utime, and `ITIMER_VIRTUAL` will
barely advance for it.

That is *arguably correct* — the task is in the kernel — but it is new
behaviour that nobody has looked at, and it makes the spin visible in a way it
was not before. It is also an argument for parking rather than spinning, which
this objective should take up: `block_ring3_restart` already exists and is used
by `epoll_wait` for exactly this shape of wait.

---

## 2. Recommended phasing, and an honest cost statement

The four deliverables named for this objective are **not** of comparable size:

| deliverable | rests on | estimated cost |
|---|---|---|
| `sys_claim_pci_device` / release | PCI config-space access + owner field + device lock | moderate |
| Per-device IOMMU domain isolation | `iommu_attach_proc_domain` — **already exists** | small |
| Core pinning / isolation | scheduler `rq_push_any`, `rq_steal`, `g_slice_on` | moderate |
| **MSI-X vector routing** | IDT enlargement, vector allocator, MSI capability parser, per-device vector tables | **large — bigger than the other three combined** |

Recommendation: **split the milestone.** Objective 1 delivers claim/release,
per-device IOMMU isolation and core pinning, on the existing legacy-IRQ
delivery path. Objective 2 delivers MSI-X on top of a real vector allocator.
Attempting all four as one change means the first three cannot be gated until
the fourth compiles, and this tree's whole evidence discipline depends on being
able to run a green gate between increments.

The specification below covers all four so the ABI is designed once and does
not have to be broken later; the MSI-X sections are marked **[Obj 2]**.

---

## 3. System call surface

### 3a. `SYS_CLAIM_PCI_DEVICE` (111)

111 is the next free number (110 is `SYS_GETRUSAGE`, added in v0.94).

```c
/* SYS_CLAIM_PCI_DEVICE(bdf, flags, out) -> device_id (>= 0), or -errno
 *
 * bdf packs (bus << 8) | (slot << 3) | func, matching struct kdev's existing
 * field and the SID the IOMMU already uses. DOMAIN IS NOT AN ARGUMENT: this
 * kernel's DMAR handling addresses one segment, and taking a domain parameter
 * it cannot honour would be an ABI that lies. It is reserved in `flags`.
 */
```

- `flags` bit 0: `CLAIM_EXCLUSIVE` — refuse if any other process holds it.
  Bit 1: `CLAIM_DMA` — build and attach a per-process IOMMU domain. Bits 2-15
  reserved, **must be zero**; a non-zero reserved bit returns `-EINVAL` rather
  than being ignored, so a future meaning cannot be silently consumed by an old
  kernel.
- `out` is a user pointer to a 32-byte descriptor: `{ u64 bar0_phys, bar0_len,
  bar1_phys, bar1_len }`. Sizes are reported so ring 3 can size its mapping
  without a second syscall.
- Returns a **device_id**, which is an index into `kdevs[]` and is exactly what
  `SYS_VFIO_MAP_BAR` already takes as `a0`. The claim path therefore composes
  with the existing BAR mapping rather than replacing it.

Errors, each distinct so a failure names its rule: `-EPERM(-13)` no `PCAP_VFIO`
or the device's own `d->req`; `-EBUSY(-16)` already claimed; `-ENODEV(-19)` no
such BDF; `-ENOSPC(-28)` `kdevs[]` full; `-EINVAL(-22)` reserved flag set.

### 3b. `SYS_RELEASE_PCI_DEVICE` (112)

`(device_id) -> 0 or -errno`. Detaches the IOMMU domain back to the kernel
identity domain via the existing `iommu_detach_to_kernel(bdf)`, revokes the
process's DMA grants for that device, unmaps its BAR windows, and clears the
owner.

**Release must be idempotent and must not be the only path.** Process exit and
a CPL3 fault both have to do the same work, or a crashed driver leaves a device
attached to a dead process's page tables — see §5c.

### 3c. `SYS_CORE_PIN` (113)

```c
/* SYS_CORE_PIN(cpu_mask, flags) -> 0 or -errno
 *
 * DISTINCT FROM SYS_SETAFFINITY, which this kernel already has. Affinity says
 * where THIS task may run. Pinning says what may run on THOSE CORES: it asks
 * the scheduler to stop placing other work there, which affinity cannot
 * express because it is a property of a task and this is a property of a core.
 */
```

Requires `PCAP_SMP_ADMIN` (already defined, `user/init.c:326`). `flags` bit 0
`PIN_NO_SLICE` also gates off the LAPIC slice tick for those cores by clearing
their participation in `g_slice_on`.

**Refuses to isolate the last unisolated core**, returning `-EBUSY`. A kernel
that lets a process strand the scheduler with nowhere to run is a kernel with a
one-syscall hang, and the check has to be in the syscall rather than in the
caller.

### 3d. `SYS_MSIX_ENABLE` (114) — **[Obj 2]**

`(device_id, n_vectors, out_vector_base) -> vectors allocated, or -errno`.
Deferred with the rest of the MSI-X work; the number is reserved here so the
ABI does not shuffle when it lands.

---

## 4. IOMMU and memory security

### 4a. Per-device domains

The mechanism exists and is keyed by BDF. What changes is *when* it is invoked:
today `iommu_attach_proc_domain` is driven by the kernel's own device wiring;
under this objective a claim drives it, and the domain's lifetime becomes the
claim's lifetime.

    claim(bdf, CLAIM_DMA)
      -> iommu_build_proc_domain(proc)        existing
      -> iommu_attach_proc_domain(bdf, proc)  existing
      -> dma_grant_create(..., DMA_GRANT_PAGE, bdf) per mapped region

    release / exit / fault
      -> iommu_detach_to_kernel(bdf)          existing
      -> dma_grant_revoke(...) for each grant
      -> iommu_invalidate_all()               existing

### 4b. Mapping bounds

Two rules, both of which the negative control in §6 exists to prove:

1. **A device's DMA domain contains only pages that process owns.**
   `iommu_domain_add_page` is the only way in, and it must be called only for
   frames already mapped into that process's CR3. A device that can reach a
   page its owner cannot is a DMA escape.
2. **A BAR is never a raw physical address from userland.** Case 20 already
   enforces this — "SYS_VFIO_MAP_BAR never accepts a raw physaddr from
   userspace" — and the claim path must not weaken it. `out` reports the
   physical base for information; it is not accepted back as an input.

`MAX_DMA_GRANTS` is **8 per process** (`1872`). A GPU with two BARs plus ring
buffers can plausibly exceed that. Raising it is in scope; leaving it at 8 and
returning `-ENOSPC(-12)` from a grant exhaustion that a caller reads as "out of
memory" is not.

---

## 5. Concurrency and lock safety

### 5a. A new lock, and its rank

`kdevs[]` becomes runtime-mutable, so it needs `g_dev_lock`.

**CORRECTED — this section originally proposed rank 7, which is taken.** The
first draft read CLAUDE.md's rank list as the complete set and proposed slotting
in at 7 "between frame (6) and net (9)". Enumerating the actual declarations
shows that wrong twice over, and it is recorded rather than silently fixed
because it is the same class of mistake CLAUDE.md documents for role numbers:
*check for a free number before adding one, and grep both halves.*

The live set, from `grep 'struct klock g_'`:

    0 redir   1 ofile   2 vfs    3 cas     4 vblk   5 surf   6 ipc
    7 gpu     8 audio   9 net   10 wm     12 vmfile        13 udb

plus two deliberately **UNRANKED** raw spinlocks that are not klocks at all —
`g_frame_lock` (the frame free-list, which must work before the scheduler
exists and on APs with no rank tracking) and `g_conlock` (console, IRQ-safe
leaf inside `kprintf`). The parenthesised "(6)" and "(7)" beside them in the
`kernel64.c:3349` comment are descriptive labels, not ranks.

So the second error: **CLAUDE.md is not stale about `g_frame_lock`** — it
correctly omits it, because an unranked leaf has no rank to list. What CLAUDE.md
actually lacks is `ipc` 6, `gpu` 7, `audio` 8, `wm` 10, `vmfile` 12 and
`redir` 0, and that omission is what made rank 7 look free.

Adopted: **`g_dev_lock` rank 11**, the only free rank in the range.

The reasoning is the ordering it must survive. The original draft also had the
direction backwards: acquiring UPWARD is legal, so a NIC passthrough path
holding `net` (9) and then claiming a device needs the device lock **above** 9,
not below it. Rank 11 satisfies that, and equally lets a driver holding
`gpu` (7) or `audio` (8) claim its own device — which is the whole point for a
GPU passthrough path. The discipline that follows: take the driver's lock
first, then `g_dev_lock`, never the reverse. Frame allocation is unranked, so
it constrains nothing here.

### 5b. What the lock protects, and what it must not span

Under `g_dev_lock`: the owner field, the `used` flag, BDF lookup, and the
claim/release transition. **Not** under it: `iommu_invalidate_all()`, which
touches hardware registers and can be slow, and any user-memory access
(`access_ok`, the `out` copy) — a page fault taken while holding a lock is a
deadlock this tree has the rank machinery to detect but no reason to invite.
Validate against state the held lock protects rather than re-deriving it
through a lower rank.

### 5c. Teardown invariants

Three exit paths, and all three must converge:

| path | site | today |
|---|---|---|
| `SYS_EXIT` | `kproc_reap` region | calls `vfio_teardown_kproc` |
| CPL3 fault | `handle_cpl3_fault` (`21826`) | must reach the same teardown |
| explicit release | new case 112 | new |

The invariant to state and then test: **after any of the three, no device's
IOMMU domain points at a dead process's page tables, and `kdevs[].owner` names
no unused slot.** The v0.75 `ppid_slot` lesson applies directly — a slot index
is not proof of identity, because slots are recycled. `owner` must be validated
with the same `(slot, gen)` pairing `ppid_slot`/`ppid_gen` use, or a claim can
be inherited by whichever process next occupies the slot.

Teardown ordering is fixed: **detach the IOMMU first, then unmap, then clear the
owner.** Unmapping before detaching leaves a window in which the device's domain
references freed frames, and a device does not stop issuing DMA because its
driver died.

---

## 6. Probe and gate strategy

### 6a. Role 65 — the negative control

Roles are a shared namespace matched by nothing but the integer, so this was
checked before claiming it: `grep '\.role = ' kernel/kernel64.c` and
`grep 'role == ' user/init.c` agree that **64 is the highest assigned** (v0.94)
and 65/66 are free.

Role 65 asserts that the isolation **refuses** things, which is the half a
green suite cannot demonstrate. Exit codes 1920-1929, success sentinel **1920**,
deadline expiry on its own code (**1929**).

| # | check | must produce |
|---|---|---|
| 1 | `SYS_VFIO_MAP_BAR` on a device this process has **not** claimed | `-EPERM`, not a mapping |
| 2 | claim a device already claimed by another process | `-EBUSY` |
| 3 | claim with a **reserved flag bit set** | `-EINVAL` — proves reserved bits are enforced, not ignored |
| 4 | DMA to a physical page outside the claimed domain | an **IOMMU fault**, readable via `iommu_read_fault` |
| 5 | `SYS_VFIO_WAIT_IRQ` on a line the process does not own | `-EPERM` |
| 6 | `SYS_CORE_PIN` isolating every online core | `-EBUSY` |

**Check 4 is the one that matters, and it is the one that can silently test
nothing.** If the DMA never actually reaches the device, no fault is raised and
the check passes having proven nothing — which is exactly the shape of the
Carryover-3 failure CLAUDE.md records, where a harness reported 12/12 against a
deliberately broken kernel because the workload could not reach the defect.

So role 65 must **count detections separately from failures**: `iommu_faults`
raised by the probe's own out-of-domain access must be **> 0** for the check to
count as run at all. A boot where check 4 reports zero faults and zero failures
is reported as `NOT EXERCISED`, not as a pass.

### 6b. Role 66 — the positive path, under `-smp 4`

Exit codes 1930-1939, sentinel **1930**, deadline on **1939**.

1. Claim a device with `CLAIM_DMA`, map BAR 0, read back a known register.
2. Drive an interrupt and receive it — via `SYS_VFIO_WAIT_IRQ` for Objective 1,
   via MSI-X for Objective 2.
3. DMA throughput inside the domain, measured as **bytes per unit of a
   deadline**, never as an iteration count — at 1 vCPU and at 4 an iteration
   budget means different durations, which is the standing rule here.
4. Claim, release, and **re-claim** the same device, twice. This is the check
   that catches a teardown leak, and it is the one a single-shot test misses.
5. Under `-smp 4`, a **sibling** process attempts the role-65 check-1 access to
   the claimed device concurrently and must still be refused — isolation has to
   hold against another core, not just against the same one.

### 6c. The negative control on the control

Per the standing rule that a test which cannot fail has not passed: before
believing role 65, build with the ownership check in case 20 removed, and
confirm check 1 **fails**. Before believing role 66's step 4, build with
`iommu_detach_to_kernel` stubbed out and confirm the re-claim fails. Both were
what exposed v0.94's split as real; the same discipline applies here.

### 6d. Harness changes

- **`tools/probe-timebench.sh` needs none.** It is specific to `timebench` and
  roles 63/64, and overloading it would make one script two things.
- **New `tools/probe-passthrough.sh`**, same FIFO-driven shape — it is the
  proven pattern now, and the deadlock note in that script (`exec 8<>` not
  `8>`) should be carried across rather than rediscovered.
- **New `make probe-passthrough`**, with `PROBE_QEMU` for the `-smp 4` form,
  mirroring `probe-timebench`.
- **DONE — `gate-matrix.sh` has an unbound device.** The claim path cannot be
  exercised against a device QEMU does not expose, and every function on the
  reference machine had a kernel driver bound, so role 66 reported NOT
  EXERCISED on every boot. The `smp4-iommu` tier now carries
  `-device virtio-rng-pci,disable-legacy=on,disable-modern=off,iommu_platform=on`.

  The device choice is constrained from three directions, and only one option
  satisfies all of them:
  - **Not a second virtio-net.** `virtionet_probe` binds any vendor 1af4 class
    0x02 function, so a duplicate NIC would be host-bound and refused exactly
    like the first. virtio-rng is vendor 1af4 but PCI **class 0x00**, and this
    kernel binds drivers to classes 01/02/03/04 only.
  - **Not `e1000`.** Its BAR 0 is 128 KiB and `SYS_MAP_PCI_BAR` refuses a BAR
    larger than its 64 KiB window rather than truncating it.
  - **`disable-legacy=on,disable-modern=off` is required, not decoration.**
    virtio-rng-pci defaults to a *transitional* device and `iommu_platform=on`
    is modern-only, so without them QEMU refuses the command line and the
    guest never starts — measured, as a `NO-PROMPT` run with a two-line log.

  The synthetic `cmd_vfio_stress` device was considered and rejected as the
  vehicle: it has no BDF and no IOMMU domain, so claiming it would exercise
  neither claim resolution nor confinement while turning the suite green.

  **Note for a later reader:** on q35, `0:31.2` (the ICH9 SATA controller) is
  also registered claimable, because this kernel has no AHCI driver to bind to
  it. Role 66 picks `0:4.0` only because the sweep reaches slot 4 first. The
  explicit rng is still correct — deliberate beats incidental — but a claimable
  storage controller is worth a look on real hardware, where `KDEV_BOUND_HOST`
  protects it only if something is actually bound.

---

## 7. Open questions to resolve before implementation

1. **Can `pci_probe_virtio` be generalised, or does this need a real bus
   walker?** `sys_claim_pci_device` takes a BDF, and today only virtio devices
   have one; every other `kdev` carries `bdf = 0xFFFF`. Nothing can be claimed
   by BDF until that is fixed, and it is the first thing to build.
2. **ANSWERED, NO — `MAX_KDEV 8` did not hold.** The first boot of the
   generalised walk inventoried seven PCI functions on a plain SeaBIOS machine
   against an eight-slot registry, before anyone attached a GPU. Raised to 32,
   and `pci_register_claimable` now reports an overflow rather than dropping
   silently.

6. **NEW — should a device the KERNEL is actively driving be claimable?**
   Auto-registration currently enters virtio-gpu `0:2.0` into the registry
   while this kernel's own virtio-gpu driver is bound to it and the compositor
   is running on it. A `CLAIM_DMA` on that BDF would attach a per-process IOMMU
   domain underneath a live driver, and the kernel's own DMA to the device
   would begin faulting.
   It needs `PCAP_VFIO`, so this is not reachable by an unprivileged process —
   but "privileged callers can break the running system by using the API as
   documented" is a design gap, not an acceptable boundary. Blueprints wants
   exactly this eventually ("detaches a high-performance GPU ... from the host
   kernel"), which means a claim on a bound device has to quiesce the driver
   first. Until that exists, the honest options are to refuse a claim on a
   driver-bound BDF, or to mark such devices in the registry so the refusal is
   explicit. This must be settled before claim/release is exercised by a probe
   role against a real device.
3. **Is `MAX_DMA_GRANTS 8` enough for a real passthrough workload?** See §4b.
4. **Should `SYS_VFIO_WAIT_IRQ` park instead of spin?** See §1c. The spin is now
   visible as system time, `block_ring3_restart` already exists for this shape
   of wait, and epoll is the working precedent.
5. **How large must the IDT become for MSI-X, and what allocates vectors?**
   `idt[52]` is exact today. This is Objective 2's first question and is
   recorded here so it is not discovered late.
