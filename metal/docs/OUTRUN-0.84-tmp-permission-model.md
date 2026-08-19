# VOL_TMP grows a permission model — v0.84

Earlier in v0.84, `c8f8e07` gave `struct tmpfile` a creator uid and enforced
owner-or-root in `vfs_unlink`. Its own commit message named what it left open:

> Still no gid and no mode on this volume. tmp open, read and write have no
> permission model, and guarding only unlink would invite the assumption that
> the rest is guarded too.

That assumption is now true. Any holder of `PCAP_FILESYSTEM` could read or
overwrite another user's scratch file, having merely been unable to delete it —
a leak of AUTHORITY, not of bytes. This closes it.

## The rule

**Owner or root, on open, read, write and truncate.** uid 0 bypasses, which is
what root means here and is stated in one place for the reason `vfs_permit`
states it in one place.

There is still **no mode**. The rule is owner-or-root, not the root volume's
nine-bit triple, so there is nothing for a mode to vary: a tmpfile is private to
its creator and root. A tmpfile cannot be shared with a group or made
world-readable, because this volume has no way to say so. That is a real
difference from the root volume and is named in the struct rather than left to
be discovered.

`gid` is stamped from the effective pair at creation, is **not** part of the
decision, and is printed per-slot by `vfsstrs`. It is recorded so a later group
rule has the identity it needs, and printed because a field no code reads and no
log shows is exactly the shape this project has been burned by.

## Where the checks live

One predicate, `tmp_owner_ok_locked()`, called with `g_vfs_lock` already held.
Every caller resolves the slot, decides, and acts under ONE acquisition, so no
window exists in which the name could become a different file between the
decision and the access. `vfs_unlink`'s tmp branch established that shape
earlier this release — it deliberately did not reuse the root volume's
lookup-release-lookup pattern — and the access paths follow it. A tmp slot is a
recycled index (`ofile.dirent` holds a TMP INDEX, not a name), and this tree has
already spent a carryover on what a stale index resolves to when nothing pins
it.

Guarded: `vfs_open_for` (existing files only), `tmp_read_range`,
`tmp_read_file`, `tmp_write_at`, `tmp_write_file`. The last two are the
redirection paths — leaving them open would have made `< tmp/x` a way to read
what `read()` may not.

**O_TRUNC is guarded by ORDERING, not by a second test.** The open check sits
before the truncation, in the same lock acquisition. A duplicate test beside the
truncation would be unreachable, and an assertion that cannot fail is not a
guard.

**`lseek` is deliberately NOT checked.** It moves an offset and consults no
content, matching the root volume. The ring-3 test asserts it still succeeds for
a caller that may not read, so that stays a decision rather than an accident.

## The test, and why it uses an inherited descriptor

Once open is owner-or-root, an unprivileged process cannot obtain a descriptor
on someone else's tmp file by asking. A test that only called `open()` would
exercise one guard and leave the other two **unreachable** — a check no caller
can reach is a check that cannot fail.

So root opens the file and **leaves the descriptor open across `fork()`**; the
child drops to uid 1000 (setgid then setuid — order is load-bearing) and uses
the descriptor it already has. That is the real case these guards exist for, and
the same case the root volume's write check was written for: a process may hold
a perfectly valid descriptor it is not entitled to use.

The child also creates, writes, rewinds, reads and removes its **own** tmp file.
Without that, "refuse uid 1000 everything" — a different and much worse rule —
would pass.

The three refusals are reported as a **bitmask** (bit 0 read, 1 write, 2 open),
not as the first failure. They are not sequential steps; they are three
independent guards that the reverted build reverts together, and a
first-failure exit would leave two of them untested in the very run whose job is
to show they can fail.

## Falsifiability

`make EXTRA=-DTMP_PERM_REPRO` reverts the open/read/write checks and **nothing
else** — `vfs_unlink`'s rule shipped earlier in v0.84 with its own assertion and
stays enforced, so the log can say which guard the failure belongs to. The
refusal is still COUNTED in that build; only the enforcement is dropped.

Measured 2026-08-18, uniprocessor, fresh image per boot:

| | baseline | `-DTMP_PERM_REPRO` |
|---|---|---|
| iso md5 | `8fecc4a9cef3811d5fb5800bf5df09b2` | `b3c004fad0a877b48de12ec0ece5e625` |
| boot | 45 suites, **498 passed, 0 failed**, ranks 0 | 45 suites, **497 passed, 1 failed**, ranks 0 |
| `[vfsstrs]` | 22 passed, 0 failed | 21 passed, 1 failed |
| worker | clean | exit **1817** — "tmp ownership is NOT ENFORCED AT ALL" |
| decisions / refusals | 77 / 5 | 72 / 5 |

The reproducer does not merely return the wrong value, it does the damage:

```
[vfsstrs] tmp slot 0 'rperm' uid 0 gid 0 len 6
```

Root wrote `abc` (3 bytes). The unprivileged child's write landed, extending it
to `abcXYZ`. That is the vulnerability, reproduced on demand.

Logs, each stamped with the md5 of the image it booted:
`OUTRUN-0.84-tmpperm-baseline.log`, `OUTRUN-0.84-tmpperm-repro.log`.

### A first attempt that was wrong, recorded because it passed review

The first version of this test proved the file had survived the refused unlink
by checking that a later `open()` was refused. That made an assertion about the
UNLINK rule depend on the OPEN rule, and the reverted build duly reported
*"the refused unlink REMOVED the file anyway"* about an unlink that had been
refused correctly — naming the wrong guard. Worse, the worker exited there, so
the read and write refusals were never reached in the one run meant to falsify
them. Baseline-green plus reproducer-red looked like valid evidence and was not.

The child now asserts only what it can prove without the open guard (that the
refusal is stable across a second attempt); the owner checks the bytes.

## The six-tier gate

Image `fc7595b88864e8c901a7bf55fcbee797`, all ten boots on that one image.
Clean rebuild, no compiler warnings or errors.

| tier | suites | passed | failed | ranks |
|---|---|---|---|---|
| uniprocessor | 45 | 498 | 0 | 0 |
| smp2-bios | 45 | 510 | 0 | 0 |
| smp4-bios | 45 | 514 | 0 | 0 |
| smp4-iommu (q35 + VT-d, `intremap=on`) | 47 | 527 | 0 | 0 |
| gate-dirty (3 boots, one reused image) | 45 | 0 failing each | — | 0 |
| gate-dirty-smp (3 boots, `-smp 4`) | 45 | 0 failing each | — | 0 |

Every fresh tier is **+2 assertions** against the same tier at `886750d`
(496/508/512/525), which is the two new counter assertions and nothing else —
the count moving by exactly the number added is what says no existing assertion
was silently dropped.

Both dirty tiers reported empty consecutive-boot assertion diffs and intact
durable artefacts at both widths, so the new refusals leave no residue that
changes what the next boot starts from. That matters more than usual here: this
change adds a per-slot owner, and the dirty gate is the configuration that would
show a tmp slot coming back owned by a dead process.

## Not covered

- The counters assert `decisions > 0` and `refusals > 0`. They do not
  distinguish WHICH path refused, so a boot could satisfy both on the unlink
  rule alone; the ring-3 worker is what pins the access paths specifically.
- No group or mode semantics exist to test, by design.
- `lseek`/`SEEK_END` on another user's tmp descriptor still reports that file's
  length. Metadata, not content, and it matches the root volume — named here
  rather than fixed.
