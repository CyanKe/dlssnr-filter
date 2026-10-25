#!/usr/bin/env python3
"""Enumerate the model resources inside nvngx_dlssnr.dll.

The DLL is the only authority on which networks it can run: 310.8.0 carries one
(`WEIGHTS_HT`), 310.9.16 carries two (`WEIGHTS_HT` + `WEIGHTS_CC_SMALL_FP8`).
This script reports the version resource and every WEIGHTS_* name it can find,
so a new DLL can be classified without guessing from its file version.

    python tools\\inspect_nr_dll.py nvngx_dlssnr.dll
    python tools\\inspect_nr_dll.py nvngx_dlssnr.dll app\\nvngx_dlssnr.dll --json

The same two facts are what the filter's own panel needs at runtime (which
presets this DLL can serve); src\\dlssnr_engine.cpp does the C++ half of this
scan. Keep the two in step: the prefix matched here is the prefix it looks for.
"""
import argparse
import json
import re
import struct
import sys
from pathlib import Path

WEIGHTS_PREFIX = "WEIGHTS_"
PARAM_PREFIX = "DLSSNR"
NAME_RE = re.compile(rb"[A-Za-z0-9_.]{2,64}")


def _pe_sections(data: bytes):
    """[(name, va, raw_off, raw_size)] for a PE32+ image, or [] if not a PE."""
    if data[:2] != b"MZ":
        return []
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e_lfanew:e_lfanew + 4] != b"PE\0\0":
        return []
    coff = e_lfanew + 4
    nsec, = struct.unpack_from("<H", data, coff + 2)
    opt_size, = struct.unpack_from("<H", data, coff + 16)
    opt = coff + 20
    magic, = struct.unpack_from("<H", data, opt)
    dd = opt + (0x70 if magic == 0x20B else 0x60)     # data directories
    if len(data) < dd + 8 * 3:
        return []
    res_rva, res_size = struct.unpack_from("<II", data, dd + 2 * 8)  # entry 2
    sec = opt + opt_size
    out = []
    for i in range(nsec):
        off = sec + i * 40
        name = data[off:off + 8].rstrip(b"\0").decode("ascii", "replace")
        va, raw_size, raw_off = struct.unpack_from("<III", data, off + 12)
        out.append((name, va, raw_off, raw_size))
    return out, res_rva, res_size


def _rva_to_off(sections, rva: int):
    for _n, va, raw_off, raw_size in sections:
        if va <= rva < va + raw_size:
            return raw_off + (rva - va)
    return None


def read_version(path: Path, data: bytes) -> dict:
    """File/product version from the RT_VERSION resource, via the real API order.

    Parsed by hand (not by searching for the 0xFEEF04BD signature) because that
    signature also appears inside the packed weights, which is exactly how an
    earlier version of this script reported 46863.0.46863.4696 for 310.9.16.
    """
    info = {}
    secs = _pe_sections(data)
    if not secs:
        return info
    sections, res_rva, res_size = secs
    root = _rva_to_off(sections, res_rva)
    if root is None:
        return info

    def entries(off):
        n_named, n_id = struct.unpack_from("<HH", data, off + 12)
        return off + 16, n_named + n_id

    p, n = entries(root)
    ver_off = None
    for i in range(n):
        e = p + i * 8
        name, off = struct.unpack_from("<II", data, e)
        if name == 16 and off & 0x80000000:                     # RT_VERSION
            ver_off = root + (off & 0x7FFFFFFF)
    if ver_off is None:
        return info
    p, n = entries(ver_off)
    if not n:
        return info
    _name, off = struct.unpack_from("<II", data, p)
    lang_off = ver_off + (off & 0x7FFFFFFF)
    p, n = entries(lang_off)
    if not n:
        return info
    _name, off = struct.unpack_from("<II", data, p)
    blob_off = lang_off + (off & 0x7FFFFFFF)
    size, = struct.unpack_from("<I", data, blob_off + 4)
    blob = data[blob_off:blob_off + size]

    # VS_FIXEDFILEINFO starts at the first 0xFEEF04BD *inside the version blob*.
    i = blob.find(b"\xbd\x04\xef\xfe")
    if i >= 0:
        _sig, _ver, _t, _m, ms, ls = struct.unpack_from("<IIIIII", blob, i)
        info["FixedFileVersion"] = "%d.%d.%d.%d" % (
            ms >> 16, ms & 0xFFFF, ls >> 16, ls & 0xFFFF)
    for key, label in ((b"FileVersion", "FileVersion"),
                       (b"ProductVersion", "ProductVersion")):
        m = re.search(re.escape(key.decode().encode("utf-16-le")) + b"\x00\x00", blob)
        if m:
            start = m.end()
            chunk = blob[start:start + 96]
            s = chunk.decode("utf-16-le", "ignore").split("\x00")[0]
            info[label] = s.strip()
    return info


def scan_names(data: bytes) -> dict:
    """WEIGHTS_* / DLSSNR* names, ASCII and UTF-16, with occurrence counts."""
    found = {}
    for prefix in (WEIGHTS_PREFIX, PARAM_PREFIX):
        names = {}
        for enc, step in (("ascii", 1), ("utf-16-le", 2)):
            try:
                needle = prefix.encode(enc)
            except LookupError:
                continue
            start = 0
            while True:
                off = data.find(needle, start)
                if off < 0:
                    break
                start = off + 1
                # Read the surrounding printable run, then trim to the prefix.
                lo = off
                while lo > 0 and NAME_RE.match(data[lo - 1:lo + 1] or b""):
                    lo -= 1
                    if off - lo > 64:
                        break
                m = NAME_RE.match(data, off)
                if not m:
                    continue
                name = m.group().decode("ascii", "replace")
                if not name.startswith(prefix):
                    continue
                names.setdefault(name, {"count": 0, "encodings": set()})
                names[name]["count"] += 1
                names[name]["encodings"].add(enc)
        found[prefix] = {
            k: {"count": v["count"], "encodings": sorted(v["encodings"])}
            for k, v in sorted(names.items())
        }
    return found


def main() -> int:
    try:                       # the console here is cp936; never crash on a name
        sys.stdout.reconfigure(errors="replace")
    except Exception:
        pass
    ap = argparse.ArgumentParser()
    ap.add_argument("dll", nargs="+", type=Path)
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()

    out = []
    for path in a.dll:
        if not path.is_file():
            print(f"missing: {path}", file=sys.stderr)
            return 2
        data = path.read_bytes()
        out.append({
            "path": str(path),
            "size": len(data),
            "version": read_version(path, data),
            "names": scan_names(data),
        })

    if a.json:
        print(json.dumps(out, indent=2))
        return 0

    for rec in out:
        v = rec["version"]
        print(f"== {rec['path']}")
        print(f"   size    : {rec['size']:,} bytes ({rec['size'] / 1048576:.1f} MiB)")
        print(f"   version : {v.get('FixedFileVersion', '?')}"
              f"   FileVersion={v.get('FileVersion', '?')}")
        weights = rec["names"].get(WEIGHTS_PREFIX, {})
        if weights:
            print(f"   weights : {len(weights)} found")
            for name, meta in weights.items():
                print(f"             {name:32s} x{meta['count']:<3d} {meta['encodings']}")
        else:
            print("   weights : (none found)")
        params = rec["names"].get(PARAM_PREFIX, {})
        presets = [n for n in params if "Preset" in n or "preset" in n]
        print(f"   params  : {len(params)} DLSSNR* names"
              f"{'; preset hints: ' + ', '.join(presets) if presets else ''}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
