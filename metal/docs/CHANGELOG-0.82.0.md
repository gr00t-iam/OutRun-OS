# OutRun OS v0.82.0-metal — the release that gave the file position a verb

Milestone 82. Written into the tree in v0.85, from the annotated tag and the
commits it covers; the tag remains the primary record and its checksums are
reproduced below unchanged.

`struct ofile` had carried an `off` field for many releases. Nothing on an
ordinary read consulted it. This is the release where the file position stopped
being decoration.

## ARTEFACT

```
outrun-os-0.82.0.iso
MD5    0a077f3660a68674e4a78b18842abaa2
SHA256 e77680224546b40a435f2da7206faeb86c364ce5223010178aac10ccbf3b373c
```

`VERSION` and `KERNEL_VERSION` bumped in `701b5fe`, committed **before** the tag.
`make release-verify` PASS on md5 `0a077f36` — 45 suites, 0 failing assertions,
0 rank faults, 315 s uniprocessor.

## WHAT LANDED

### #92 — the generation counter that counted nothing

`kproc_reset()` gained a `cmemset` backstop in v0.72 to stop new struct fields
being inherited by a recycled slot. It sat thirty lines above `p->gen++` and
zeroed `gen` on every reset, so **every slot read `gen == 1` on every recycle**.

`ppid_live()`'s generation compare therefore evaluated `1 == 1` forever and
degenerated to the `used`-only test it had been written in v0.75 to replace.
v0.75 defect B was live in every shipped release from **v0.72 through v0.81**.

The comment on the increment already warned that "a memset here would silently
reinstate the bug". The memset that did it was added earlier, above the warning.

**It was shown, not argued.** Reverting the fix and re-running measured 0
detections while `getppid()` returned a stranger's pid in 5 of 5 boots — the
guard was *unreachable*, not merely quiet. That distinction is the whole reason
the counter is now read as a pair: `detected == 0` means INCONCLUSIVE, not pass.

### #93 — role 54: the one-way privilege drop, observed from ring 3

`usersstrs` had claimed for several releases that ring 3 exercised
`setuid`/`setgid`. **No such caller existed.** Nothing checks that a comment's
subject is real.

Role 54 makes the claim true rather than deleting it. Four routes back to root
are refused, `seteuid`/`setegid` included — the sharper pair, because a
permanent drop that left `saved` at 0 would be a loan rather than a drop.

### #94 — `SYS_LSEEK`, and `SYS_READ` made positional on VOL_ROOT

A seek added beside a read that ignores the offset would have been a phantom
syscall: it would return correct-looking offsets and change nothing observable.
The ring-3 worker caught exactly that on its first boot.

Role 34's expectation was corrected in the same change. It asserted that a
forked child re-reads the parent's bytes and called that POSIX — but `fork`
shares the open file *description*, so the child reads on from where the parent
stopped. The old assertion passed only because reads ignored the offset
entirely.

## AUDIT

All 100 defined syscalls are dispatched: 98 in the switch, `SYS_FORK` and
`SYS_SIGRETURN` in the trap path where the interrupted frame exists. No ENOSYS
path, no FIXME, no TODO. **The gaps in this tree are absent calls, not stubs.**

## GATE

Every tier on ONE image, `8187d8744c6b994f5180c28dbb8d2e7b`, built from main at
`85402a5` (the tag adds only the two version strings):

| tier | suites | passed | failed |
|---|---|---|---|
| uniprocessor | 45 | 496 | 0 |
| smp2-bios | 45 | 506 | 0 |
| smp4-bios | 45 | 512 | 0 |
| smp4-iommu | 47 | 525 | 0 |
| gate-dirty (3 boots, one reused image) | 45 | 0 failing | — |
| gate-dirty-smp (3 boots, `-smp 4`) | 45 | 0 failing | — |

Both dirty tiers reported empty consecutive-boot assertion diffs and intact
durable artefacts — which is what clears the positional-read change of state
drift across volume reuse.

**Not run for this tag:** bare metal, Proxmox, soak/repeat beyond the above.

## KNOWN, NOT FIXED

- `SYS_WRITE_FILE` is still a whole-file COW replacement and ignores the
  position, so lseek-then-write is not yet POSIX. *(Closed in v0.83.)*
- VOL_TMP reads are still whole-file; a seek on a tmp descriptor moves an offset
  its reads ignore. *(Closed in v0.83.)*
- `threadstrs` still reads the shared ring-3 high-water without resetting its
  own. *(Closed in v0.84.)*

Both storage boundaries were named in the code rather than left to be
discovered — which is why v0.83 could close them without first rediscovering
them.
