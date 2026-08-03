# OutRun OS v0.72.0-metal — users: identity, ownership and permission

Milestone 72. Until now every process in this system was root, every file
belonged to nobody, and "permission" meant a capability bitmask on a process
rather than a relationship between a subject and an object. This adds the
missing half.

## WHAT A CAPABILITY COULD NOT SAY

Since v0.47 this kernel has had capabilities — `PCAP_FILESYSTEM`,
`PCAP_WIMP`, `PCAP_AUDIO` — and they are a genuinely good mechanism. They
answer *"may this process use the filesystem at all?"*

They cannot answer *"may this process read **that** file?"* A capability is a
property of the subject alone. Access control needs a property of the pair, and
that requires the object to carry an owner.

So the two systems are complementary and both are kept. A process must hold
`PCAP_FILESYSTEM` to make a VFS call **and** satisfy the mode bits on the
specific file. Neither subsumes the other, and the capability check stays first
because it is the cheaper one.

## CREDENTIALS

`struct kproc` gains `uid` and `gid`. They are **per thread group**, not per
thread: `tg_of()` resolves to the leader before any credential is read, so a
thread cannot hold an identity different from the process it belongs to. That
matches POSIX and it is the only reading that makes `setuid` meaningful.

| # | call | notes |
| --- | --- | --- |
| 88 | `SYS_GETUID()` | |
| 89 | `SYS_GETGID()` | |
| 90 | `SYS_SETUID(uid)` | root may become anyone; a non-root process may not change its uid at all |
| 91 | `SYS_SETGID(gid)` | same rule |

**There is no setuid-on-exec bit and no saved-set-uid**, deliberately. Dropping
privilege is one-way here: a process that lowers its uid cannot raise it again.
That is a *stricter* rule than POSIX, and it is stated as a limit rather than
dressed up as a feature — real systems need `seteuid` to drop and regain
privilege around a critical section, and this cannot express that yet.

`fork` inherits credentials, which is the whole point of them; `execve` keeps
them, because without a setuid bit there is nothing that would change them.

## OWNERSHIP ON DISK

`struct dirent` gains `uid`, `gid` and `mode`. The dirent had spare bytes, so
this needs **no format change and no version bump** — a v0.71 volume mounts
unchanged.

That backward compatibility creates the one interesting decision in this
release. An old dirent has `mode == 0`, which as a POSIX mode means *nobody may
do anything*. Enforcing that literally would make every file on every existing
volume permanently inaccessible — a correct reading of the bits and a
catastrophic reading of the intent.

So **mode 0 is treated as "unset" and read as the default 0644**, and the suite
asserts it explicitly rather than leaving it to be discovered. The alternative
— rewriting every dirent at mount — would turn a read-only mount into a write
and lose the property that an older kernel can still read the volume.

## THE PERMISSION RULE

`vfs_permit(dirent, uid, gid, want)` implements the POSIX triple with the rule
that actually matters: **first match wins, not most-permissive wins.**

- uid 0 bypasses the mode entirely;
- else if the uid matches the owner, the **owner** triple decides — and that is
  final;
- else if the gid matches, the **group** triple decides;
- else `other`.

An implementation that OR-ed the three triples together would pass almost every
test one might write, and would be wrong in exactly one case: a file whose
owner is granted *less* than a stranger. `0046` — owner `---`, group `r--`,
other `rw-` — must deny its owner a write it grants everyone else. Without
first-match-wins, nobody can ever lock themselves out of their own file, and
the owner triple is advisory rather than binding.

The suite asserts that case and its neighbour (group beats `other` even when
`other` is more permissive), because those two are what separate a real
implementation from a plausible one.

Enforcement lives in `vfs_open_for`, so it is on the path every ring-3 open
takes, rather than at each call site where one could be forgotten.

## THE DEFECT THIS SHIPPED WITH FOR ABOUT AN HOUR

`kproc_reset` blanks `struct kproc` **field by field** rather than `memset`-ing
it. The new `uid`/`gid` were not on that list. A slot freed by one process
handed its credentials to whoever took the slot next.

That is not a leak of memory. It is a leak of **authority**, and it is the
worst possible defect for the milestone that introduces credentials.

It surfaced nowhere near its cause. `usersstrs` leaves two slots owned by uid
1000 and 1001; the toolchain drivers spawn into those recycled slots; a
compiler that had always run as root was suddenly not root and could no longer
write root-owned files. **Nine failures across `toolstrs`, `compstrs` and
`langstrs`** — every one of which read as a permission bug in code that had
just been written and was in fact correct.

The first diagnosis was wrong. A descriptor leak in the same suite was found
and fixed first — real, worth fixing, and *not this*. Fixing it is what made
the real defect visible: with descriptors accounted for, the toolchain still
failed, and identity was the only explanation left.

`usersstrs` now spawns into a just-recycled slot and requires uid and gid to be
zero, placed immediately after the block that marks those slots reusable so the
next spawn must take one of them.

### The pattern, not the instance

This is the **second time in three milestones** that a field was added to a
struct and forgotten in the one place responsible for resetting it. v0.70 was
`wimp_teardown_kproc` open-coding what `wm_destroy` already did; this is
`kproc_reset` missing two new fields. Both were silent. Both surfaced far from
their cause. Both were found by a test failing for a reason that looked like
something else.

The instances are fixed. The pattern is not, and it deserves its own audit of
every hand-rolled reset and teardown path in this kernel rather than another
per-release patch.

## VERIFICATION

42 suites (44 on VT-d), 0 FAIL on uniprocessor/BIOS, SMP-4/BIOS and
q35 + VT-d IOMMU (`-smp 4`), each against a freshly formatted volume. One suite
is added — `usersstrs`, 18 assertions. Boot logs are in `docs/`.

The decision table is checked directly against `vfs_permit`, and the
**end-to-end** behaviour is checked through `vfs_open_for`, which is what ring 3
actually reaches:

- a file created by a user is owned by that user, with the default mode;
- a stranger may open another user's `0644` file for reading but has no write
  right to it, while the owner does;
- after `chmod 0600` the stranger cannot open it at all, and the owner still can;
- root opens a `0600` file it does not own;
- a legacy dirent reads as the default rather than as no-access;
- and a recycled process slot does not inherit the dead occupant's identity.

The suite also returns every descriptor it takes and asserts the open-file table
is exactly as full as it was before it ran — added after this fixture starved
the 16-slot table and broke nine assertions in unrelated suites twenty minutes
later.

### Warnings

46, all pre-existing; this release adds none.

### Not done

- **No `seteuid`/`setreuid` and no saved-set-uid.** Privilege drop is one-way.
- **No setuid-on-exec bit**, so there is no way to write a privileged helper.
- **No supplementary groups** — one gid per process, so a user cannot be in two
  groups at once.
- **No `chown`.** A file's owner is whoever created it, permanently.
- **Directories have no permissions of their own**, because this VFS has no
  real directory objects — path prefixes are part of the name. Traversal
  therefore cannot be restricted, only the files themselves.
- **No password, no login, no authentication of any kind.** `setuid` is
  available to root on request; nothing establishes *who* a user is. This is
  the mechanism for a multi-user system, not yet a multi-user system.
