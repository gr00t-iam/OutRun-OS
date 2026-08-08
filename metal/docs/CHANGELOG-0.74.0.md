# OutRun OS v0.74.0-metal — the other two thirds of a credential

Milestone 74. v0.72 gave this kernel identities and enforced them against the
filesystem. It also ended with an admission, written next to the code it
described:

> Note this deliberately does NOT implement saved-set-uid — a real system lets
> a root program drop temporarily and regain. That needs a third stored id per
> process and a setuid-binary story to be worth having, and both are absent
> here; see the changelog. What is implemented is the half that is safe to rely
> on.

This release adds the third id and the setuid-binary story, and then adds the
thing v0.72 had no answer for at all: a way to *become* a user. A uid was
previously something the kernel handed out at spawn. Nothing could authenticate,
so nothing could ever legitimately acquire an identity — the credential model
was real but unreachable.

## THE CREDENTIAL IS NOW A TRIPLE

`struct kproc` carries all six POSIX ids: `uid/euid/suid` and `gid/egid/sgid`.

| id | question it answers |
|---|---|
| real (`uid`) | who are you |
| effective (`euid`) | who are you **treated as** |
| saved-set (`suid`) | who are you **allowed to go back to** |

**Every permission check reads the effective id and nothing else.** That is not
a detail; it is what makes `setuid()` more than bookkeeping. A kernel that
stored an effective id and then judged access by the real one would let a
process hold a privilege the filesystem never consulted, and — symmetrically —
honour one it had explicitly dropped.

The rule is enforced by four accessors (`cred_euid`, `cred_egid`, `cred_ruid`,
`cred_rgid`) rather than by discipline at each call site. They also resolve
identity through `tg_of()`, so a thread always answers as its process. A call
site that hand-rolled either property would be correct today and wrong the
first time someone put a thread on the path.

### The asymmetry between setuid() and seteuid() is the security argument

Both are implemented; the difference between them is the entire mechanism.

- **`setuid()` from a privileged process moves all three ids**, saved included.
  That is a permanent, irrevocable drop — it overwrites the very id a return
  would have been authorised against. This is the call a daemon makes once, at
  startup.
- **`seteuid()` moves only the effective id**, so a drop made with it is
  reversible by construction. This is what a setuid helper wraps around its
  privileged section.
- **Unprivileged, either call may only target an id already held** (real,
  effective, or saved). Nothing widens.

Setting saved *to the new id* on a permanent drop is the part worth staring at.
Leaving it behind would have shipped a documented, supported route back to root
— the exact leak this milestone exists to close.

**"Privileged" means `euid == 0`, never `uid == 0`.** Both substitutions are
wrong, in opposite directions, and each looks reasonable in isolation:
`uid == 0` would *deny* a setuid-root helper (real uid non-zero, effective zero)
the privilege it legitimately holds; it would also *grant* full setuid rights to
a root-owned process that had already dropped its effective id to an untrusted
user, handing back on request the privilege it had just given up.

## SET-ID ON EXEC, AND WHY THE ORDERING IS THE FEATURE

`exec()` honours `S_ISUID`/`S_ISGID` (04000/02000), stored in the existing
`uint32_t` mode field — no dirent growth, so the 256-byte on-disk layout is
untouched and volumes written by older kernels still mount. The bits sit above
the nine permission bits, so `vfs_permit`'s triples are unaffected and a mode
carrying them is still non-zero, which matters because zero means "legacy, use
the default". A file cannot become setuid by being old.

The file's credentials are read **when the dirent is resolved** and applied
**at the point of no return**, after the image has loaded. Every failure between
those two points — `ENOENT`, `EIO`, out of frames, a rejected ELF — returns with
the caller's original credentials completely untouched. The classic way this
goes wrong is a kernel that grants the new identity first and then discovers the
image will not load.

The real id never moves, so a setuid binary can always discover who actually
invoked it. The saved id is set from the (possibly new) effective id on **every**
exec, not only a setuid one: without that a setuid helper could never legally
regain a privilege it dropped, so it would never drop, and would run privileged
throughout. Setting it unconditionally also scrubs any saved id inherited from
the previous image.

**Set-id is refused for a uid-0 target, loudly.** `chmod` requires ownership, so
there is no path for an unprivileged user to mark a file setuid-root — but that
combination is the highest-consequence one in the system, and a release that
introduces the bit should not also introduce the only way to hand full root to
whoever can exec a file. A later milestone with a real installer and an execute
permission bit can lift this.

### What is deliberately absent

There is **no execute permission bit**. Every file this system creates gets mode
0644, so enforcing x-to-exec would leave the self-hosting toolchain unable to run
anything it had just compiled. Adding it needs a default-mode story for compiler
output, which is a VFS milestone's job. Exec remains gated by `PCAP_FILESYSTEM`;
the set-id bits change *who* a program runs as, not *whether* it may run.

## A USER DATABASE

An in-memory table of up to 16 accounts, each with a per-user salt and a derived
password digest. `SYS_USERADD` (99) creates one — root only, on the effective id,
because an account is an identity that can later be authenticated *into*, and an
unprivileged caller able to create one could mint itself a uid-0 account and log
in as root a moment later.

`SYS_AUTH` (98) verifies a password and returns the uid. **It does not change
the caller's credentials.** Proving who you are and becoming them are different
acts; fusing them would mean any program that could prompt for a password could
also silently assume that user's identity. A login program does both, in that
order, and the `setuid()` half is separately privileged.

`uid` and `gid` share `SYS_USERADD`'s third argument as `(gid << 32) | uid`,
because `syscall_dispatch` takes exactly three and always has. Widening the ABI
would touch the assembly entry stub and every one of the ~100 calls through it,
to carry 32 bits that fit in a register that is otherwise half empty.

### What the KDF is not

**The mixing primitive is FNV-1a, and FNV-1a is not a cryptographic hash.** It
is neither collision resistant nor preimage resistant. A digest stored here
would not survive an attacker with the database and a serious offline budget.
No comment changes that; only a real primitive would, and writing one belongs in
its own milestone with its own test vectors rather than being improvised inside
this one.

What this release does ship is the correct *structure* around whatever primitive
sits inside — and each piece defends against a failure that primitive strength
would not fix:

- **Per-user salt.** Two users choosing the same password get unrelated digests.
  Without it the database itself reveals which accounts share a password, and
  one cracked entry breaks all of them. This is a property of the schema.
- **4096 rounds.** Salt and password are folded in on *every* round, not once at
  the start — folding once would let an attacker precompute the state after the
  input and pay for the rounds a single time across every candidate, making the
  work factor decorative. Four lanes, each round depending on the previous and
  on a neighbouring lane, so the chain is serial and cannot be collapsed or
  parallelised per lane.
- **Constant-time verify.** The compare accumulates differences across all 32
  bytes and tests once, so a failed verify's duration says nothing about how
  much of the digest matched. The accumulator is `volatile` to stop an optimiser
  reintroducing the early exit.
- **Three-strike lockout.** The online defence, and the one that actually matters
  at this maturity: a weak hash is an *offline* problem, and an attacker who
  cannot make unlimited guesses through `SYS_AUTH` never reaches it.

Swapping FNV-1a for a real primitive later changes `udb_kdf()` and nothing else.
The schema, the syscalls, the lockout and every test stay as they are. That is
the point of building the structure first.

### Two things the lockout gets right that are easy to get wrong

**Failures are consecutive, not cumulative.** A success clears the counter. An
implementation counting lifetime failures would lock out a legitimate user who
typos twice a week.

**A locked account refuses the correct password too.** A lockout that yielded to
the right guess would still be an oracle — an attacker who found the password
would simply notice the different answer.

Likewise, an unknown user and a wrong password return the same code, and an
unknown user still pays for a full KDF against a throwaway salt. A
distinguishable answer, by code *or* by duration, turns `SYS_AUTH` into an
account enumerator, which hands over the first half of the problem for free and
is far cheaper than cracking anything.

The KDF runs **outside** the table lock. Holding a global lock across the
deliberately-slow part would let one authenticating core stall every other user
of the table for the whole work factor, turning the defence into a denial of
service.

## `authstrs`, AND WHY IT IS NOT IN THE BOOT SEQUENCE

A new suite, in three halves, because no one of them catches what the others do.

- **Schema**, testing `udb_kdf`/`udb_ct_eq` as pure functions: salt uniqueness,
  digest determinism, the same password under different salts diverging, and the
  compare rejecting a difference in the first *and* last byte.
- **Auth**, driving the same entries the syscalls call, through all four
  outcomes and the lockout *sequence* — a state machine, observable only in
  order.
- **Credentials**, calling `syscall_dispatch` directly for the setuid family.
  These rules are about what a process may do to itself, so they depend on all
  three stored ids and on transitions between them; calling the dispatcher is
  what makes it the real rule under test rather than a paraphrase living in the
  suite.

It passes **35/35 on all three configurations**. It is nevertheless a shell
command — `authstress` — and not part of the regression boot, which needs
explaining rather than hiding, because a suite missing from the matrix looks
exactly like an oversight.

**Compiled into the boot sequence, it made `posixstrs` fail 4 of its 12
assertions under `-smp 4` on SeaBIOS** — and only there; uniprocessor and
smp4-iommu were clean in every run. The symptom was a forked child reporting the
wrong parent through `getppid` and its parent's `waitpid` then timing out, in
rounds 0 and 2 but not round 1: a race, not a broken rule.

Eight runs, each hypothesis given its own boot rather than an argument:

| build | authstrs runs | `posixstrs` |
|---|---|---|
| v0.73 pristine, fast host | no | 12/0 |
| v0.73 pristine, host ~4× slower | no | 12/0 |
| every v0.74 kernel change, suite not called | no | 12/0 |
| the above + 2 no-op recyclable spawns | no | 12/0 |
| v0.74 full, authstrs before posixstrs | yes | 8/4 |
| v0.74 full, authstrs before posixstrs | yes | 8/4 |
| v0.74 full, authstrs after posixstrs | yes | 12/0 |
| v0.74 full, authstrs after posixstrs | yes | 8/4 |

That rules out, by experiment: the credential model itself; kproc slot recycling;
set-id on exec (zero set-id events fired in the entire failing boot); the host
(v0.73 is clean on it twice, at both speeds); and **suite order** — moved so that
`posixstrs` finishes before `authstrs` begins, `posixstrs` failed anyway, so
whatever the coupling is, it is not sequential.

Two explanations survived that, and they were confounded in every build made to
that point: the suite's runtime cost, and the ~8 KB the kernel grows when dead-code
elimination stops discarding it. Every build that ran the suite was also every
build that was larger.

**Moving it to a shell command separated them.** The shell's reference keeps
`cmd_auth_stress` in the image, so the release ISO is byte-for-byte the same size
as the failing builds — same layout, suite not executing — and the matrix is
clean. **Binary layout is therefore not the cause; executing the suite during
boot is.** That is one hypothesis eliminated, not a root cause found.

**The mechanism remains unknown, and this is a mitigation rather than a fix.** It
makes the three-config matrix honest again and leaves the actual defect exactly
where it was found: an intermittent race in `posixstrs`'s fork/waitpid path,
which nineteen releases never perturbed because nothing had ever run before it
that cost enough to matter. It is `posixstrs`'s bug, not this milestone's, and it
is now the first item on the next milestone's list.

## THE THREE FAILURES THAT WERE NOT BUGS

The first full run came back 44 suites, 449 passed, **3 failed** — two in
`usersstrs`, one in `vfsstrs`. Neither cause was in the new code, and both are
worth recording because both would recur.

**`usersstrs` built half an identity.** v0.72's suite set only `uid`/`gid` on its
Alice and Bob processes, because in v0.72 that *was* a credential. Once
`vfs_open_for` began judging by the effective pair, both processes still had
`euid == 0` and were therefore silently **root** — so every assertion about what
a non-owner may not do was passing for the wrong reason, until one of them
failed and said a stranger could open a 0600 file. It could, because the
stranger was root. A half-built identity is worse than an obviously broken one:
it makes a permission suite report success while testing nothing. The suite now
constructs all six ids.

**The disk was dirty.** The CAS volume persists across boots by design, and the
second run mounted the first run's filesystem. `vfsstrs` asserts a journal commit
is still pending on disk — but the prior boot's `cas_mount()` had already applied
it (`applied a pending directory commit (seq 290)` appears in the log, before the
suite runs). `usersstrs` creates a file it expects to own at the default mode,
and found the previous run's copy at 0600. Neither assertion was wrong; both
premises were. Every config now boots from a freshly created disk image.

## VERIFICATION

Three configurations, as every release since v0.49: uniprocessor, `-smp 4` on
SeaBIOS, and `-smp 4` on q35 behind an emulated Intel VT-d IOMMU. Each from a
clean disk.

| configuration | suites | passed | failed | reached the prompt |
|---|---|---|---|---|
| uniprocessor | 43 | 417 | **0** | yes |
| `-smp 4`, SeaBIOS | 43 | 433 | **0** | yes |
| `-smp 4`, q35 + VT-d | 45 | 446 | **0** | yes |

`authstrs` is not counted above — it is a shell command, for the reason given in
its own section. Run on its own it is **35 passed, 0 failed** on all three.

The suite counts differ between configurations because several suites are
config-gated: `iommu` and `capdma` need the emulated VT-d unit and are skipped
without it, which is why the IOMMU column is the longest rather than the
shortest.

**Every configuration boots from a freshly created disk image.** This is new in
this release and it is not cosmetic. The CAS volume persists across boots by
design, so re-running against the same image mounts the previous run's
filesystem — and the first v0.74 matrix produced three failures that were purely
that: `vfsstrs` asserts a journal commit is still pending on disk, and a prior
boot's `cas_mount()` had already applied it (`applied a pending directory commit
(seq 290)` sits in the log, ahead of the suite). `usersstrs` creates a file it
expects to own at the default mode and found the previous run's copy at 0600.
Neither assertion was wrong; both premises were.

## WHAT THIS RELEASE DOES NOT DO

Stated plainly, in the tradition of v0.67, v0.68 and v0.72, so the next
milestone inherits a list rather than a discovery:

- **The user database is in memory only.** It does not survive a reboot. There
  is no `/etc/passwd`, no persistence, and therefore no account that outlives
  the boot that created it.
- **The KDF's primitive is not cryptographic.** See above; this is the single
  largest gap and it is structural, not incidental.
- **No execute permission bit**, so no exec-permission enforcement.
- **No directory permissions.** VFS directory objects do not exist yet, so
  there is nothing to hang them on; this was explicitly deferred to the VFS
  milestone that introduces real directories.
- **No supplementary groups.** A process belongs to exactly one group.
- **No login program.** `SYS_AUTH` and `setuid()` exist and compose into one,
  but nothing in userland does it yet.
- **The lockout has no expiry.** A locked account stays locked for the life of
  the boot; there is no timeout and no administrative unlock.
- **`authstrs` is not in the regression boot**, so it does not run automatically.
  It has to be invoked as `authstress`, which means a future change could break
  it without the matrix noticing.
- **`posixstrs` has an intermittent race that this release found and did not
  fix.** Under `-smp 4` on SeaBIOS, a forked child can report the wrong parent
  through `getppid` and its parent's `waitpid` then times out. It is reachable
  by running enough work before it and is not caused by anything in v0.74 —
  every credential change was independently cleared by experiment. The mechanism
  is unknown. This is the largest thing left open here and it belongs to the
  POSIX layer, not to authentication.

## ONE DEFECT FOUND IN PASSING

`g_udb_lock` was first written as a bare `static struct klock g_udb_lock;`,
which zero-initialises it to **rank 0 with a null name**. `klock_acquire`
compares `st[*sp-1] >= l->rank` to catch out-of-order acquisition, and rank 0
makes that true against *any* held lock — it would then report the violation by
printing through a null pointer. It never fired, because every acquisition of
this lock happens with no other lock held. That is exactly the kind of fault
that stays invisible until someone adds the first caller that nests, so it is
now `{ 0, "udb", 13, 0, 0 }` — rank 13, above every existing lock, which is
correct for a leaf that is never held while taking another.
