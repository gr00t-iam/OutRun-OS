#!/usr/bin/env python3
"""Bake a PEM CA bundle into a C string for the freestanding browser build.

Usage: python mk-ca-roots.py apps/vendor/cacert.pem apps/web_ca_roots.h

The browser has no filesystem path to a trust store at run time -- it is a
boot module, not a Unix process -- so the roots it will verify against have to
be part of the binary. Emitting them as a C array makes that explicit and
makes the trust set reviewable in the diff that changes it, which a blob
loaded from disk would not be.

mbedtls_x509_crt_parse() requires the buffer length to INCLUDE the terminating
NUL for PEM input; callers therefore pass `sizeof web_ca_roots`, not
`sizeof web_ca_roots - 1`. Getting that wrong parses zero certificates and
every HTTPS request then fails closed with "CA roots missing", which looks
like a network fault rather than a build error.
"""
import pathlib
import sys

# The bundle's prose header carries non-ASCII CA names (accented characters in
# issuer organisations). Only the base64 certificate blocks are kept below, and
# those are ASCII by definition, so decode leniently rather than failing on
# text that is about to be discarded.
src = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8", errors="replace")

# Keep only the certificate blocks; the bundle's prose header is not parsed by
# mbedtls and only bloats the image.
keep, out = False, []
for line in src.splitlines():
    if line.startswith("-----BEGIN CERTIFICATE-----"):
        keep = True
    if keep:
        out.append(line)
    if line.startswith("-----END CERTIFICATE-----"):
        keep = False

if not out:
    sys.exit("mk-ca-roots: no certificates found in " + sys.argv[1])

body = "".join(line + "\\n" for line in out)
chunks = [body[i:i + 76] for i in range(0, len(body), 76)]
# Never split an escape sequence across two string literals.
fixed = []
for chunk in chunks:
    while chunk.endswith("\\"):
        chunk = chunk[:-1]
    fixed.append(chunk)
    if not chunk:
        sys.exit("mk-ca-roots: chunking failed")

text = "\n".join('    "%s"' % c for c in fixed if c)
header = (
    "/* Generated from %s by tools/mk-ca-roots.py -- do not edit.\n"
    " * %d certificate line(s). Pass `sizeof web_ca_roots` (NUL included) to\n"
    " * mbedtls_x509_crt_parse; PEM parsing requires the terminator. */\n"
    "#ifndef OUTRUN_WEB_CA_ROOTS_H\n"
    "#define OUTRUN_WEB_CA_ROOTS_H\n"
    "static const unsigned char web_ca_roots[] =\n%s;\n"
    "#endif\n"
) % (pathlib.Path(sys.argv[1]).name, len(out), text)

pathlib.Path(sys.argv[2]).write_text(header, encoding="ascii")
print("Generated", sys.argv[2], "-", len(out), "certificate lines")
