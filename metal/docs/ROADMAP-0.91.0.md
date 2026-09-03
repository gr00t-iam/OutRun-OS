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
