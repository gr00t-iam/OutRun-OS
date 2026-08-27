# OutRun OS v0.83.0-metal — positional file I/O, and both volumes agreeing

Milestone 83. Written into the tree in v0.85, from the annotated tag and the
commits it covers; the tag remains the primary record and its checksums are
reproduced below unchanged.

v0.82 made reads positional and left writes whole-file, which is a filesystem
where the position means one thing to `read` and nothing to `write`. This is the
release that closed that, and then closed it again on the second volume.

## ARTEFACT

```
outrun-os-0.83.0.iso
MD5    39c9d64b88264f9fcb8174bc2ed1d285
SHA256 9211c11f52e0c5dc911f16d8b6e0cf008b2e76116616a96d94c4c0b63b515fcb
```

`VERSION` and `KERNEL_VERSION` bumped in `6244fb9`, committed **before** the tag.
`make release-verify` PASS on md5 `39c9d64b` — 45 suites, 0 failing assertions,
0 rank faults, 315 s uniprocessor.

## WHAT LANDED

### Positional root I/O and O_TRUNC (`e9c5eec`)

`SYS_WRITE_FILE` overwrites in place with tail preservation, extension, and
sparse zero-fill.

**O_TRUNC is not a separate feature — it is what makes the rest safe.** Before
this, a write replaced a file's entire contents, and that side effect *was* the
only way to shorten one. Making writes positional without providing O_TRUNC
would have left every re-authored file carrying the tail of whatever was longer
before it. `compilerstrs` writes four different bodies through one path, so a
shorter body after a longer one would have handed the compiler malformed source.

### VOL_TMP parity (`edeb480`, `6425615`)

Positional reads, then positional writes, O_TRUNC, and unlink. **Both volumes
now agree on what a file position means** — which is the property the next two
releases kept spending effort to preserve, and the reason v0.84's `ftruncate`
and `O_APPEND` were implemented on both volumes rather than one.

### Two fixes that only became visible once the position mattered

**`SEEK_END` on a tmp descriptor resolved through `DENTS[]` using a TMP INDEX**,
returning an unrelated root file's length. Invisible while tmp reads ignored the
offset; load-bearing the moment they did not. This is the usual way a latent bug
becomes a live one — not by changing, but by something else starting to depend
on it.

**`SYS_VFS_UNLINK` refused `tmp/` at the dispatch**, so a tmp slot could never be
reclaimed for the life of a boot. With `TMP_MAXFILES` at 4 and four names in use
(`scratch`, `redir.txt`, `one.txt`, `two.txt`) the volume was permanently full
the moment those four existed, and the next consumer starved `vsh`. That failure
does not announce itself as "out of tmp slots" — it surfaces as
`vsh: cannot create tmp/two.txt` and a pipestrs assertion about shell pipelines,
a long way from the cause. `TMP_MAXFILES` is now 8.

## VERIFICATION

Each commit cleared its **own** six-tier matrix before being pushed, every tier
on one image:

| commit | image |
|---|---|
| `e9c5eec` | `0c244f1f1c287cbc96a136866be5d7af` |
| `edeb480` | `ee045daf2f27dcc827caac076b52967a` |
| `6425615` | `01efefffb181c6400845ef96ebbc8b89` |

Tiers: uniprocessor, smp2-bios, smp4-bios, smp4-iommu, gate-dirty,
gate-dirty-smp. Zero failing assertions and `ranks=0` throughout; both dirty
tiers reported empty consecutive-boot assertion diffs and intact durable
artefacts at both widths.

**The six-tier evidence covers `6425615`, not this tag's commit.** `6244fb9`
changes only the two version strings, and the published artefact was
independently `release-verify`'d above. Recorded rather than glossed, because a
log that cannot name the binary it came from is not evidence.

**Not run for this tag:** bare metal, Proxmox, soak/repeat beyond the tiers
listed.

## KNOWN, NOT FIXED

- **Tmp unlink is NOT permission-checked.** A tmpfile carries no uid, gid or
  mode, so there is nothing for `vfs_permit` to judge, and any holder of
  `PCAP_FILESYSTEM` may unlink any tmp name. Consistent with tmp open/read/write
  having no ownership model, but a real asymmetry with the root volume.
  *(Closed in v0.84 — and closing only the unlink half left a larger hole that
  v0.84 then had to close as well.)*
- A tail-preserving positional write beyond `REDIR_STAGE_MAX` (32 KiB) still
  returns ENOSPC; only whole-file writes from offset 0 take the unstaged path.
  *(Closed in v0.84.)*
- `threadstrs` still reads the shared ring-3 high-water without resetting its
  own. *(Closed in v0.84.)*
