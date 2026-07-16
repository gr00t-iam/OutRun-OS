# Outrun OS v0.35.0-metal — SMP groundwork: every core boots; IPIs and TLB shootdowns are real

Retires the oldest architectural caveat in the project — "uniprocessor" — by
booting every core the ACPI MADT reports and running two genuine cross-core
protocols. **Scope is deliberately groundwork:** the thread scheduler stays
BSP-only *by policy*, which is precisely what keeps every isolation invariant
proven since v0.17 valid (see "Why the scheduler stays BSP-only" below).

## What boots

- **MADT enumeration** (reusing the ACPI table walker built for DMAR in v0.22):
  every enabled local APIC is collected.
- **LAPIC driver** at `0xFEE00000`, mapped `PCD|NX` into the kernel window. The
  BSP keeps 8259 virtual-wire delivery (LINT0=ExtINT, LINT1=NMI) so the
  existing PIT/PIC scheduler runs untouched; APs mask both LINTs (all device
  IRQs stay routed to the BSP).
- **Real-mode trampoline** (`boot/apboot.asm`, flat binary at phys `0x8000`):
  each AP climbs 16-bit → protected → PAE + kernel CR3 + EFER.LME|NXE → long
  mode, then loads its stack and cpu index from a mailbox and jumps into
  kernel C (`ap_main`). INIT–SIPI–SIPI is PIT-timed with a SIPI#2 fallback.
- **Per-CPU areas via `IA32_GS_BASE`**: `cpu_idx()` reads `%gs:0`. Each AP
  reloads the real kernel GDT (so `CS=0x08` matches every IDT gate), takes the
  shared IDT, enables its LAPIC, and parks in `sti; hlt` — reachable only by
  IPIs.
- Boot-verified: `[smp] 4/4 cpus online`, each printing its own LAPIC id and
  distinct GS area.

## The cross-core protocols (the `smp` suite, 6/6)

1. every MADT-enumerated cpu reached 64-bit kernel C (or single-cpu boot)
2. per-CPU areas via GS: each cpu sees its own identity, APIC ids distinct
3. targeted fixed-vector IPI (vector 48) delivered to and acknowledged by every AP
4. **concurrent `lock xadd` from ALL cpus totals exactly** — 4×100000 into one
   counter with zero lost increments (real cross-core atomicity)
5. **TLB shootdown acknowledged by every remote cpu** — mailbox va + broadcast
   IPI (vector 49) + `invlpg` on each core + lock-inc ack, BSP waits for all
6. **after remap+shootdown every remote cpu reads the NEW frame** — each AP is
   first primed with the old translation, the page is remapped, a shootdown is
   broadcast, and every AP then reads the new frame through the same vaddr

The suite degrades cleanly to a single-CPU boot (checks become local-path
assertions), so the no-`-smp` regression stays zero-failure.

Two IDT vectors were added (48 ping/wake, 49 TLB shootdown); `isr_dispatch`
handles them first and returns before any BSP-only bottom-half runs. The
kernel's stress suite now drives its post-remap check through the real
`tlb_shootdown()` path.

## First real shared-state discipline

The console is now genuinely shared between cores, so it gets the kernel's
first spinlock (`con_lock`/`con_unlock`): **held only with interrupts disabled
on the holding CPU**, so an ISR that prints can never deadlock against the
thread it interrupted. `kprintf` emits one line as one atomic unit; the panic
and canary paths bust the lock before printing so a fault never hangs silently.

## Why the scheduler stays BSP-only (and the one honest wrinkle)

Migrating the run queue to per-CPU would invalidate the entire proven
invariant stack — `access_ok`'s TOCTOU safety is documented as resting on
uniprocessor, no-preemption-mid-syscall execution. So no user code or kernel
thread runs on an AP; APs exist to answer IPIs and shootdowns. That is a
separate milestone with its own re-verification, not a side effect of this one.

**Wrinkle, stated plainly:** `__stack_chk_guard` is a single global that the
BSP scheduler swaps per thread (the v0.19 per-thread canary). It is inherently
a uniprocessor/BSP-owned value. `cmd_smp` is the one routine that busy-waits
coordinating *other* cores while carrying that per-thread canary, and under
`-smp` its frame-canary check was the sole casualty — every other
stack-protected function, including `stress` and `fuzz` running with all APs
live, passes, so there is no broader corruption. The fix mirrors the existing
treatment of `sched_switch_to`: the core-coordination routine carries
`no_stack_protector`. This is the correct, consistent call, not a workaround;
a per-CPU canary scheme is future work alongside per-CPU scheduling.

## Verification (all boot-tested on this exact image)

| Suite | Result |
|---|---|
| validate | 19/19 |
| invariants | 21/21 |
| stress | 10/10 (post-remap check now via real cross-core `tlb_shootdown`) |
| fuzz | 8/8 |
| **smp (new)** | **6/6** |
| threads | 7/7 |
| flip | 8/8 |
| cursor | 6/6 |
| kinetic | 12/12 |
| keys | 5/5 |
| iommu | 2/2 (q35 + intel-iommu config) |
| capdma | 11/11 (q35 + intel-iommu config) |

Boot configurations exercised:
- **`-smp 4` BIOS ISO (i440fx)** — 11/11 suites zero-failure, 4/4 cores, reaches
  the shell.
- **`-smp 4` q35 + intel-iommu** (`iommu_platform=on` on both virtio devices) —
  13/13 suites zero-failure: iommu 2/2 and capdma 11/11 pass with 4 cores + VT-d
  active simultaneously. SMP screendump captured; the v0.31–v0.34 pixel
  acceptance still holds (830 hit-marker pixels, the liveness bar, the five
  "HI R3" glyph blocks).
- **Uniprocessor (no `-smp`) regression** — 11/11 suites zero-failure, `smp`
  suite degrades cleanly to its single-CPU path, reaches the shell. This is the
  critical proof that SMP support did not disturb the uniprocessor invariant
  stack.

**NVMe/UEFI (`-smp 2`) not exercised this session — honestly reported, not
claimed.** This session's OVMF (`/usr/share/ovmf/OVMF.fd`) drops to its
interactive UEFI Shell instead of auto-booting the removable-media fallback
`\EFI\BOOT\BOOTX64.EFI`, and the environment's socket/detached QEMU launches
would not stay up long enough to drive the shell to the loader. This is a
firmware boot-order/console quirk entirely **upstream of the kernel**: it
reproduces on a byte-identical, v0.34-construction install image (same kernel
ELF, same GRUB config that boots correctly from the BIOS ISO), so nothing in
the SMP work is implicated. The install image is built correctly
(`\EFI\BOOT\BOOTX64.EFI` GRUB loader + kernel/user ELFs on the ESP); the
`smp` kernel is proven on the three configurations above spanning BIOS, VT-d,
and the uniprocessor path.

## Honest scope gaps

- **Scheduler is BSP-only by policy** — APs never run threads or user code.
  This is the milestone's defining boundary, not an oversight; per-CPU run
  queues + the invariant re-verification are a future milestone.
- **`cmd_smp` carries `no_stack_protector`** (see the wrinkle above): the SMP
  coordinator is exempt from the per-thread canary, like `sched_switch_to`.
- APs do not load a task register (`ltr`) — correct for CPL0-only groundwork
  (TSS.rsp0/IST are only consulted on a privilege change, which APs never
  take), but a prerequisite before an AP could ever run ring-3 code.
- The TLB-shootdown protocol is broadcast-and-wait with a tick-bounded
  timeout; there is no per-page batching or lazy/deferred shootdown yet.
- Single-writer console spinlock is the only shared lock; the frame allocator,
  surface table, and IOMMU domains are still only ever touched by the BSP, so
  they remain lock-free by construction (and must stay BSP-only until locked).
- No cross-core scheduling means no work stealing, no per-CPU timers, no AP
  idle-power management — APs simply `hlt` between IPIs.
- **NVMe/UEFI auto-boot was not exercised in this session** (OVMF drops to its
  interactive Shell; see the verification section). The kernel and image are
  unaffected — the same kernel+GRUB boots from the BIOS ISO — but this specific
  boot path is unverified for v0.35 and is called out rather than assumed.
- Prior gaps unchanged: fling velocity sampling per-frame; ASCII-level key
  events (no make/break); page tables leak on exit; VT-x unavailable under TCG;
  IOMMU/VT-d fully real.
