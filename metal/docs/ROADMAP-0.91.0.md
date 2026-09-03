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

So this objective has a precondition, and the precondition is the first task:

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
