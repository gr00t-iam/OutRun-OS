# OutRun OS v0.91 — verification debt

Status: **cycle open, nothing built.** `VERSION` reads `0.91.0-dev` and the ISO
is named `outrun-os-0.91.0-dev.iso` so a development image cannot be mistaken
for a release artefact by its filename. Nothing here is a released claim.

## WHERE THIS STARTS

v0.90.1 at `d39dca2`, artefact `outrun-os-0.90.1.iso`, md5
`3c946a834b6284ee81c5aa5ae4949ffd`, `release-verify` PASS (45 suites, 0 failing,
0 rank faults, 450 s).

v0.90 shipped the VFS lock decoupling — shared acquisition on `g_vfs_lock`, an
optimistic probe in `vfs_open_for`, per-caller contention attribution — taking
exclusive contention from 36/69/66 waits at 2/4/8 cores to **zero on every
tier**. v0.90.1 then fixed the `append-oversub` phase, which had been reporting
a lost append whenever a worker expired on its deadline.

**This cycle is deliberately not a feature cycle.** Every objective below is a
measurement v0.90 named as missing and then shipped without. That debt is the
work.

---

## Objective 1 — the 100-iteration `vfsstress` soak, on a healthy host

**Why it is outstanding.** v0.90 added shared reader acquisition to the hottest
lock in the kernel, and verified it with **3** soak iterations, not 100. The
harness (`tools/vfs-soak.sh`) exists and is proven — it drove the run that
exposed the v0.90.1 defect — but the full run was never made, because on the
host of the day a `vfsstress` boot took ~700 s and an iteration ~400 s, putting
100 iterations near 22 hours of a machine already known to be degraded. A
100-iteration figure taken there would have measured the host.

**The blocker is the host, and it is unexplained.** Late in v0.90 the same tiers
that reached the shell prompt in ~330 s began taking 405–719 s, on an idle
machine, with the toolchain host updated in between. Nothing in the kernel
accounts for it and nothing has ruled it in or out.

### 1a — TRIAGED: the host is slower, and no kernel change is responsible

**Measured with an identical-image control**, which is the only thing that could
separate "the kernel got slower" from "the machine got slower":

| image | md5 | before | now |
|---|---|---|---|
| `v90-m2f.iso` | `411d1695…` | **310 s** | **460 s** |
| `v91-open.iso` (current tip) | `4bf81ec2…` | — | 465 s |

The *same bytes* that booted in 310 s now take 460 s — 48% slower — and the
current build is statistically indistinguishable from that control (465 s vs
460 s). **Nothing in v0.90 or v0.91 made the kernel slower.** This matters
beyond bookkeeping: without the control, the obvious reading was that v0.90's
shared acquisition on the hottest lock in the tree had cost throughput, and
Objective 3 would have started from a false premise.

Eliminated, each by evidence rather than argument:

| hypothesis | finding |
|---|---|
| KVM / accelerator change | `/dev/kvm` now EXISTS, but the build user is not in the `kvm` group, so QEMU cannot open it and still falls back to TCG |
| host background load | load average 0.24 on 16 vCPUs, no other guest running |
| memory pressure | 12 Gi free of 15 Gi, swap entirely unused |
| CPU capability | Ryzen 7 7800X3D, 16 vCPUs, 8384 BogoMIPS |
| QEMU or grub regression | both binaries unchanged — qemu-system-x86_64 dated 2026-06-24, grub-mkrescue 2025-03-17; the apt upgrades of 2026-09-01 were console-setup, krb5-locales, python3.12, diffutils and udisks2 |

**Still open, and not resolvable from inside WSL2:** CPU frequency and power
state. `/sys/devices/system/cpu/cpu0/cpufreq` does not exist here — the governor
is a Windows-side setting — so a host power plan, thermal state or battery mode
cannot be inspected or excluded from the guest side. That is the leading
remaining hypothesis and it needs checking on the Windows host.

**Recorded baseline for the next comparison**, since none existed and its
absence is what made this triage expensive: a 3,000,000-iteration bash
arithmetic loop takes **3.37 / 3.55 / 3.52 s** on this host today. Re-run it
before blaming a future slowdown on anything.

**CLAUDE.md is now stale on one point.** It states "QEMU here is TCG-only — there
is no KVM". `/dev/kvm` exists; only the group membership withholds it. Adding the
build user to `kvm` would make these boots dramatically faster — and would
invalidate every timing budget in the tree at once, since deadlines, `GATE_CAP`
and the `append-oversub` worker expiry were all calibrated under TCG. It is a
decision to take deliberately, with a re-baselining pass, not a convenience to
switch on.

### 1b — PARTIAL: 10 iterations pass, the full 100 still gated

Run on the degraded host **deliberately**, because that is where the v0.90.1
tolerant branch actually executes: on a healthy host no worker expires and the
branch never runs, which is exactly what happened in the v0.90.1 four-tier
matrix. A correctness run, not the performance baseline this objective gates.

`tools/vfs-soak.sh .logs/v91-open.iso 10 '-smp 8'`, image `4bf81ec2…`:

| | |
|---|---|
| iterations completed | **10 of 10** |
| failing assertions | **0** (was 3 on this host before v0.90.1) |
| rank faults / panics | 0 / 0 |
| per-iteration result | 11 × `RESULT: 91 passed, 0 failed`, identical every round |
| wall clock | 5,021 s (722 s to prompt, ~430 s per iteration) |

**The tolerant branch was exercised in all 11 rounds**, not once by chance: one
worker expired every time, owing at most 512 B, with actual deficits of
112–464 B — each correctly read as expiry rather than a lost append. That is the
v0.90.1 fix doing its job under sustained load, on the host whose slowness
exposed the original defect.

`tools/vfs-soak.sh`'s counter fix is clean: zero `integer expression expected`
errors across ten iterations, against one per iteration before.

**Side finding worth keeping:** all eleven rounds returned an identical
`91 passed, 0 failed`. `vfsstress` is fully idempotent *within* a boot. The suite
set was historically non-idempotent *across* boots — the reason `gate-dirty`
exists — so the two are now separately established.

**The 100-iteration run remains open** and is still gated on 1a's unresolved
half. At this host's speed it is ~14 hours; on a healthy host it is closer to
five, and the per-iteration cost is dominated by the same slowdown.

### The remaining preconditions



1. **Establish what "healthy" means here.** Boot each tier and record time to
   prompt. A tier is healthy when it matches the v0.90 fresh-matrix figures
   (uniprocessor ~310 s, smp2 ~250 s, smp4 ~270 s, smp8 ~330 s) rather than the
   degraded ones. If it does not, that is Objective 1a and the soak waits.
2. **Then run `tools/vfs-soak.sh <iso> 100 '-smp 8'`.** Zero rank faults, zero
   mismatches, zero panics, no stalled iteration, and every iteration's own
   `RESULT` line at 0 failed.
3. **Say which host it ran on and how fast it booted**, in the result. A soak
   whose host condition is not recorded cannot be compared to the next one.

**What would make this fail honestly:** a stalled iteration is what reader
starvation looks like from outside the guest. The harness already distinguishes
that from a harness that cannot type — a distinction it learned the hard way,
having once reported a kernel stall that was entirely its own `mkfifo` failing
on DrvFs.

---

## Objective 2 — `smp4-iommu` coverage, and a DMA mapping bounds audit

**Why it is outstanding.** `smp4-iommu` (q35 + VT-d, `intremap=on`) is one of the
four configurations `make gate` runs, and it has not been run **since before
v0.89**. Every v0.90 gate table in this repository says so in its own coverage
line. Two milestones of lock work have shipped without it.

It is skipped because it is slow: CLAUDE.md records that it needs
`GATE_CAP=2400` on the reference host and reports `TRUNCATED` at the 900 s
default. On the currently degraded host that budget is a guess.

**Two tasks, and they are separate:**

1. **Coverage.** Run it. `GATE_CAP=2400` minimum, more if it truncates, and read
   the classifier's verdict rather than the assertions a truncated run printed —
   `TRUNCATED` and `NO-PROMPT` outrank `FAIL` precisely because a run that did
   not complete is not a verdict.
2. **The DMA mapping bounds audit**, which is not the same thing and does not
   follow from the boot passing. With VT-d and `intremap=on` the device sees an
   IOMMU-translated address, so a mapping that is wrong in a way the identity
   case tolerates becomes a fault. The audit is of the mapping *sites*: every
   place a physical range is handed to a device, checked for whether its length
   and alignment are validated against the region actually mapped, rather than
   assumed from the caller. `vfio`, `virtio-blk`, `virtio-net`, `virtio-gpu` and
   the passthrough grant path are the surfaces.

**Do not report the audit as done because the tier booted.** Those are different
claims — the v0.89 lesson about a soak named for a lock it could not contend.

### 2a — DONE: the tier runs, and its live-DMA assertion is unsound

`GATE_CAP=5400` is ample; the tier completes in **425–450 s**, 47 suites (two more
than any other tier — these assertions exist nowhere else). First completion
since before v0.89.

**`capdma` fails about half the time, on every build, and the failure is not a
confinement defect.** Ten of its eleven assertions are page-table checks and all
pass in every run: the device's domain excludes kernel memory, another process's
page, the virtqueue, and device MMIO. The one that flakes is the *live* probe —
kick the confined NIC, then poll the DMAR fault registers.

**Four attributions were made and all four were falsified.** Recorded because the
sequence is the finding:

| attribution | how it died |
|---|---|
| a regression in `92c7e87` (the optimistic probe) | five configurations at n=1 each split cleanly by build; re-running two of them twice **inverted both arms** — the "failing" image passed 2/2, the "passing" one failed 2/2 |
| the `2000000`-iteration poll budget | replaced with a 3-second tick deadline: no change, 3 fails of 4 |
| missing IOMMU cache invalidation | `iommu_attach_proc_domain` already calls `iommu_invalidate_all`, a global context-cache + IOTLB flush |
| "confinement works, the device never completed the transfer" | the instrument could not see a completion: `struct vring_used` is not volatile and the poll body was a bare `pause`, so gcc hoists the load at -O2 |

**What the last one exposed is the real defect in the test.** A positive control
was added — *the unconfined device must complete this same transfer* — and it
**fails**, with the volatile read corrected and with the device provably not yet
confined (capdma's own grant is the first confinement event in the boot). The NIC
does not complete a TX at this point in the boot at all. `vfiostrs` runs earlier
and takes the device through VFIO.

So the probe's only ever evidence was a fault that may or may not be recorded,
against a device that may not be transacting. **It cannot distinguish "the
hardware blocked the access" from "nothing was attempted"**, which is why it
flakes and why every mechanical explanation for the flake was wrong.

**Nothing was changed in the kernel.** Three candidate fixes were built and
measured; none produced a sound assertion, and shipping an unvalidated change to
a security-relevant check is worse than leaving it documented. The tree is at
`014a7f9` on this path.

**What the next attempt needs**, in order:

1. Establish the NIC is transacting at that point — restore or re-initialise it
   after `vfiostrs`, and assert an unconfined TX completes. Without that control
   nothing downstream means anything.
2. Only then assert the confined TX does **not** complete. That is the security
   property, stated directly, and it does not depend on the emulated IOMMU
   choosing to record a fault.
3. Keep the fault report as corroboration, printed either way, asserted only for
   source-ID correctness when present.
4. Make the wait a deadline. It is not the cause, but a spin count is the wrong
   shape for a hardware wait and CLAUDE.md forbids it.

### 2c — RESOLVED: the NIC arrived broken, and a reset fixes it

**Correction first, because it was recorded above as fact and was not.** §2a says
"`vfiostrs` runs earlier and takes the device through VFIO". That is **wrong**.
`vfiostrs` registers a *synthetic* device (`g_vfio_test_dev` at `n_kdev`, BAR
magic `0xCAFEBABE`) precisely so no real device IRQ line is hijacked; it never
touches the NIC. That claim was inferred from suite ordering and stated without
being checked, and it is the fifth attribution in this investigation to be
falsified.

**Measured, at last.** A probe placed in `capdma` BEFORE any confinement:

```
PRE-CONFINE probe: avail 2 -> 3, used 2 -> 2 after 300 tick(s)
```

The driver published a descriptor and kicked; the device consumed nothing, for
three full seconds, entirely unconfined. **The NIC arrives at `capdma` already
broken**, and the kernel's own `[iommu]` banner, printed immediately before, says
why:

> *"a blocked DMA puts a virtio device into its broken state, so it is exercised
> exactly once"*

That is virtio `DEVICE_NEEDS_RESET`: a device whose DMA is refused stops
servicing its queues permanently, and only a full reset clears it. A DMA gets
refused earlier in the boot, around the IOMMU suite enabling translation. Whether
that lands is timing-dependent — which is the whole flake, and why every
explanation assuming the fault was inside `capdma` was wrong.

**The fix** is `vnet_reinit()`: status reset, feature renegotiation, queue
re-registration, `DRIVER_OK`, all tick-bounded, called before the probe and
asserted (a reset that did not take is not a working NIC). The fault poll is now
a 3-second deadline rather than a 2,000,000-iteration spin.

| | before | after |
|---|---|---|
| `capdma` result | ~50% flake, 10 or 11 of 11 | **12 of 12, four boots of four** |
| fault recorded | ~50% of boots | **4 of 4** |
| verdict line | alternated | `CAPABILITY-BOUND DMA VERIFIED` ×4 |

**The original assertion was restored, and calling it unsound was an
overcorrection.** `caught && sid == bdf` is not a proxy: a DMAR fault carrying
this device's source-id IS the hardware reporting it refused that device's
access. It was only ever unreliable because no DMA was attempted on a broken
NIC, so no fault could exist — which read as a confinement failure and was not
one.

An attempt to assert non-completion via the used ring was tried and **abandoned
on evidence**: even after a successful reset the NIC does not retire a TX
descriptor at this point in the boot, unconfined, within three seconds. An
assertion built on that would test nothing. It remains as a printed observation
labelled as unusable, so the next reader does not re-derive it.

**So: a confined device IS blocked, and the proof now works.** That reverses the
open question §2a left.

**Not covered:** one of the four boots failed `langstrs` (exit 970, a compiler
wall-clock budget) while passing 10/0 in the others. That suite touches nothing
here and the failure is this host's documented slow-host symptom, but it is
recorded rather than filtered out — 3 of 4 boots were clean across all 47 suites,
not 4 of 4. That gap is
the whole reason this assertion exists and it is still open.

### 2b — DONE: DMA mapping bounds audit

Every site that hands a length to a device, traced to where the length comes
from:

| surface | length source | verdict |
|---|---|---|
| `SYS_AUDIO_WRITE` → `snd_tx_fill_and_notify` | **ring 3** | **bounded** — clamped to `bufsize`; the address is `g_audio_phys`, kernel-owned, never caller-supplied; ownership and `PCAP_AUDIO` both checked |
| virtio-blk request chain | fixed 16 / 512 / 1 | bounded by construction |
| virtio-net rx/tx | fixed buffers | bounded by construction |
| **virtio-gpu** `gpu_fill_desc_and_notify` | caller `cmdlen` | **unchecked** |

The GPU path `cmemcpy`s `cmdlen` bytes into `g_gpu_cmdbuf[64]` and then publishes
a descriptor of that length to the device, with no validation. All seven callers
pass `sizeof` a fixed struct; the largest is `transfer_to_host_2d` at **56
bytes**. Safe today by the callers' construction rather than by enforcement,
with **8 bytes of headroom**.

That margin is the finding. One added 16-byte field to a command struct —
ordinary for a scanout or 3D extension — overflows a static kernel buffer *and*
hands the device a descriptor longer than the region backing it. Under VT-d with
`intremap=on` that is a DMA read past the mapped range rather than a benign
over-read. No ring-3 input reaches `cmdlen` today, so this is a latent bound and
not a live vulnerability; a `if (cmdlen > sizeof g_gpu_cmdbuf) return;` guard
closes it.

---

## Objective 3 — VFS throughput and latency after the decoupling

**Why it is outstanding.** v0.90 measured contention — waits, retries, per-caller
attribution — and never measured **throughput or latency**. "Exclusive contention
went to zero" is not the same claim as "the filesystem got faster", and this tree
has been careful elsewhere not to let one stand in for the other.

It is genuinely open which way this goes. The shared path adds an atomic
increment, a re-check of `v`, and a rank push/pop per read acquisition; on a lock
that was contended 36% of the time that should pay for itself, and on an
uncontended one it is pure overhead. Nobody has measured either.

**What to build:** a phase that times a fixed VFS workload — opens, reads,
appends, stats — under 1/2/4/8 cores, reporting operations per tick rather than
a wall-clock figure, since TCG makes wall clock a property of the host. Compare
three builds on one host in one window:

- `95f7b4c` (v0.90.0, shared acquisition + probe)
- `3dcb309` (attribution instrument, exclusive locks throughout)
- the v0.91 tip

`3dcb309` is the honest control and already exists; it was built for exactly this
kind of comparison during the v0.90.1 investigation.

**The result may be "no measurable difference", and that is a result.** v0.89
recorded a measured no on descriptor-layer optimisation and was better for it.

### 3a — DONE: the decoupling is worth about 4% on reads, and 2.6x on contention

**Steps 1 and 2 of this objective were already complete** and are recorded here
so nobody redoes them: v0.90 M2/M3 converted the four read-heavy sites
(`vfs_read_range`, `vfs_len_of`, `vfs_permit` under `SYS_WRITE_FILE`, and
`vfs_open_for`'s probe) to shared acquisition, kept mutations exclusive, and took
measured exclusive contention from 36/69/66 waits at 2/4/8 cores to **zero on
every tier**. There is no coarse lock left blocking concurrent readers.

What was never measured is whether any of that made the filesystem FASTER. Zero
contention says the lock stopped being waited on; it does not say reads got
quicker, and the shared path is not free — an atomic increment, a re-check of
`v`, and a rank push/pop per acquisition.

**The instrument.** `vfsbench`, a shell command rather than a suite phase
because this is a measurement and not an assertion. Each core runs 400 timed
operations; latency goes into a 32-bucket log2 histogram of TSC cycles;
throughput is reported as operations per tick.

**The control is `-DVFS_EXCLUSIVE_ONLY`**, which makes `klock_read_acquire`
itself take the lock exclusively. The two builds therefore differ in the lock
discipline and in nothing else — a hand-reverted copy of four call sites would
also differ in whatever the hand got wrong.

`-smp 4`, three benchmark passes per build, both builds on one host in one
window:

| workload | shared build | exclusive build |
|---|---|---|
| **READ**, ticks for 1,600 ops | **130 / 132 / 132** | 137 / 135 / 137 |
| READ, ops per tick | **12** | 11 |
| READ, mean cycles/op | 10.03 M / 10.16 M / 9.91 M | 10.20 M / 10.03 M / 10.04 M |
| READ, p50 / p95 (TSC) | 2.1 M / 16.8 M | 2.1 M / 16.8 M |
| **WRITE**, ticks for 1,600 ops | 19,924 / 20,069 / 19,716 | 20,351 / 19,921 / 20,060 |
| `g_vfs_lock` contended, per pass | **813 / 471 / 541** | 2,565 / 1,283 / 1,350 |

**Reads are ~3.7% faster** — 131.3 ticks against 136.3 on the mean, with the two
ranges not overlapping across three runs each. Small, but real and repeatable.

**Contended acquisitions fall about 2.6x**, which is the mechanism the throughput
gain comes from and the thing the v0.90 contention numbers were already showing.

**Writes are unchanged**, 19,903 against 20,111 ticks — about 1%, inside the
run-to-run spread. That is the experiment's internal control rather than a
disappointment: writes take the exclusive lock in BOTH builds, so if they had
moved, the switch would have been changing something it was not supposed to
touch and the read comparison would be untrustworthy.

**Why the read gain is modest, stated so the number is not oversold.** Each
benchmark read is a `vfs_len_of` plus a 512-byte `vfs_read_range`, and the latter
spends most of its time in `cas_get` hashing and copying under the rank-3 CAS
lock. Lock acquisition is a small fraction of the operation, so a 2.6x reduction
in contention moves the total by single digits. A workload with more lock traffic
per unit of work would show more; this one is honest about what it measured.

**Latency percentiles show no difference** between builds — p50 and p95 land in
the same buckets. The histogram is factor-of-two granular, so it can only say
the distributions are not different by more than that. It is not evidence they
are identical.

**Absolute figures are not reported, deliberately.** IOPS and MB/s on TCG
emulation, on a host currently 48% slower than this cycle's own baseline (§1a,
unresolved), would measure the machine. Sub-millisecond percentiles cannot come
from `g_ticks` at all — it is 100 Hz, the only clock this kernel has. The
cycle counts above are TCG-derived and are comparable between these two builds
in this window and nowhere else.

---

## STANDING DEBT, carried in rather than restated per objective

- **`gate-dirty` / `gate-dirty-smp` were not re-run for v0.90.1.** They passed at
  all four tiers for v0.90.0, and the patch touched only assertion logic in one
  phase — but that is an argument, not a measurement.
- **One boot per tier** remains the basis of every v0.90 figure. It cannot see an
  intermittent below roughly 1 in 4.
- **The `append-oversub` deadline itself was never re-examined.** v0.90.1 fixed
  the assertion that misread an expiry; it did not ask whether one worker in
  sixteen expiring at `-smp 8` is itself the right behaviour, or a sign the
  budget is too tight for that width.

## NOT IN THIS CYCLE

No new ring-3 roles, no new lock modes, no descriptor-layer work — v0.89 measured
`g_ofile_lock` at ~1% under real traffic and recorded optimisation there as
unjustified. If an objective above turns up a defect, fixing it is in scope;
going looking for features is not.
