"""Compares two CC1 shader packs: a pack directory or a cc1.pack file on either side.

usage: compare_shaderpack.py <reference pack> <candidate pack> [--list]
"""

import collections
import csv
import io
import os
import struct
import sys

HELPERS = ["rect_expand.gs.dxil", "point_expand.gs.dxil", "clip_planes.gs.dxil", "blit.vs.dxil", "blit.ps.dxil"]


def load_pack(path):
    """Returns {file name: bytes} for the manifest and every DXIL file."""
    if os.path.isdir(path):
        files = {}
        for name in os.listdir(path):
            if name == "manifest.csv" or name.endswith(".dxil"):
                with open(os.path.join(path, name), "rb") as handle:
                    files[name] = handle.read()
        return files
    with open(path, "rb") as handle:
        data = handle.read()
    magic, version, count = struct.unpack_from("<4sII", data, 0)
    if magic != b"CC1P" or version != 1:
        raise SystemExit(path + " is not a CC1P version 1 pack")
    at = 12
    table = []
    for _ in range(count):
        (length,) = struct.unpack_from("<H", data, at)
        name = data[at + 2:at + 2 + length].decode("ascii")
        (size,) = struct.unpack_from("<I", data, at + 2 + length)
        table.append((name, size))
        at += 2 + length + 4
    files = {}
    for name, size in table:
        files[name] = data[at:at + size]
        at += size
    return files


def manifest_rows(files):
    """Returns {(hash, type): row} for the rows that name a DXIL file."""
    text = files["manifest.csv"].decode("ascii")
    rows = {}
    for row in csv.DictReader(io.StringIO(text)):
        if row["dxil"]:
            rows[(row["runtime_ucode_hash"], row["type"])] = row
    return rows


def dxbc_parts(blob):
    """Returns the container digest and the parts in order as (fourcc, bytes)."""
    magic, digest, _version, _size, count = struct.unpack_from("<4s16sIII", blob, 0)
    if magic != b"DXBC":
        return None, []
    parts = []
    for offset in struct.unpack_from("<%dI" % count, blob, 32):
        fourcc, size = struct.unpack_from("<4sI", blob, offset)
        parts.append((fourcc.decode("ascii"), blob[offset + 8:offset + 8 + size]))
    return digest, parts


def describe(reference, candidate):
    """Names what differs between two DXIL containers."""
    ref_digest, ref_parts = dxbc_parts(reference)
    new_digest, new_parts = dxbc_parts(candidate)
    if [name for name, _ in ref_parts] != [name for name, _ in new_parts]:
        return "part layout differs"
    differing = [name for (name, a), (_, b) in zip(ref_parts, new_parts) if a != b]
    if not differing:
        return "container digest only" if ref_digest != new_digest else "identical"
    if differing == ["HASH"]:
        return "embedded hash only"
    if "DXIL" not in differing:
        return "code equal, differs in " + "+".join(differing)
    others = [name for name in differing if name not in ("DXIL", "HASH")]
    return "code differs" + (", also " + "+".join(others) if others else "")


def code_delta(reference, candidate):
    """Returns the size change of the DXIL part, for the per-shader listing."""
    ref_code = dict(dxbc_parts(reference)[1]).get("DXIL", b"")
    new_code = dict(dxbc_parts(candidate)[1]).get("DXIL", b"")
    return "%+d bytes of code" % (len(new_code) - len(ref_code))


def main():
    args = [a for a in sys.argv[1:] if a != "--list"]
    listing = "--list" in sys.argv[1:]
    if len(args) != 2:
        raise SystemExit(__doc__)
    reference, candidate = load_pack(args[0]), load_pack(args[1])
    ref_rows, new_rows = manifest_rows(reference), manifest_rows(candidate)

    def stages(rows):
        return "%d vs + %d ps" % (sum(1 for k in rows if k[1] == "vs"), sum(1 for k in rows if k[1] == "ps"))

    print("reference: %s (%d shaders: %s)" % (args[0], len(ref_rows), stages(ref_rows)))
    print("candidate: %s (%d shaders: %s)" % (args[1], len(new_rows), stages(new_rows)))

    missing = sorted(k for k in ref_rows if k not in new_rows)
    extra = sorted(k for k in new_rows if k not in ref_rows)
    common = sorted(k for k in ref_rows if k in new_rows)
    print("reference shaders present with the same type: %d of %d" % (len(common), len(ref_rows)))
    for key in missing:
        other = (key[0], "ps" if key[1] == "vs" else "vs")
        print("  missing %s %s%s" % (key[0], key[1], " (present as the other type)" if other in new_rows else ""))
    print("extra shaders in the candidate: %d" % len(extra))
    for key in extra:
        print("  extra %s %s" % key)

    fields = collections.Counter()
    kinds = collections.Counter()
    details = []
    identical = trimmed = trimmed_identical = 0
    for key in common:
        ref_row, new_row = ref_rows[key], new_rows[key]
        for field in ("bindings", "float_bitmap", "fingerprint"):
            fields[field] += ref_row[field] == new_row[field]
        ref_blob, new_blob = reference.get(ref_row["dxil"]), candidate.get(new_row["dxil"])
        if ref_blob is None or new_blob is None:
            kind = "DXIL file absent"
        else:
            kind = describe(ref_blob, new_blob) if ref_blob != new_blob else "identical"
        identical += kind == "identical"
        if key[1] == "vs":
            ref_trim, new_trim = reference.get(key[0] + ".vst.dxil"), candidate.get(key[0] + ".vst.dxil")
            if ref_trim is not None:
                trimmed += 1
                if new_trim is None:
                    kind += "; trimmed variant absent"
                elif new_trim == ref_trim:
                    trimmed_identical += 1
                else:
                    kind += "; trimmed variant: " + describe(ref_trim, new_trim)
            elif new_trim is not None:
                kind += "; trimmed variant only in the candidate"
        kinds[(key[1], kind)] += 1
        if kind != "identical":
            delta = code_delta(ref_blob, new_blob) if ref_blob and new_blob else ""
            details.append("  %s %s: %s (%s)" % (key[0], key[1], kind, delta))

    print("manifest fields equal: bindings %d, float_bitmap %d, fingerprint %d of %d"
          % (fields["bindings"], fields["float_bitmap"], fields["fingerprint"], len(common)))
    print("DXIL byte-identical: %d of %d" % (identical, len(common)))
    print("trimmed vertex variants (.vst.dxil) byte-identical: %d of %d" % (trimmed_identical, trimmed))
    print("by kind:")
    for (stage, kind), count in sorted(kinds.items()):
        print("  %4d %s %s" % (count, stage, kind))
    if listing:
        print("\n".join(details))

    helpers_equal = 0
    for name in HELPERS:
        if name not in candidate:
            state = "absent from the candidate"
        elif name not in reference:
            state = "absent from the reference"
        elif reference[name] == candidate[name]:
            state = "identical"
            helpers_equal += 1
        else:
            state = describe(reference[name], candidate[name])
        print("helper %s: %s" % (name, state))

    clean = not missing and identical == len(common) and trimmed_identical == trimmed and helpers_equal == len(HELPERS)
    return 0 if clean else 1


if __name__ == "__main__":
    sys.exit(main())
