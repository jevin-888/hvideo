"""Add 2160x1920 CVT reduced-blanking timings while keeping existing modes.

The replacement slots must be duplicates of modes retained elsewhere in EDID.
1080p stays preferred by default; --prefer-2160 promotes the 30Hz custom DTD.
"""
import argparse
import json
from pathlib import Path

from rk628_edid import validate


def cvt_rb(frequency):
    width, height = 2160, 1920
    # CVT reduced blanking v1: 160 horizontal blanking pixels, minimum
    # 460us vertical blanking, 3-line front porch, 10-line sync for this ratio.
    hperiod_ns = (1_000_000_000 - 460_000 * frequency) // (height * frequency)
    vblank = max(460_000 // hperiod_ns + 1, 3 + 10 + 6)
    pixel_clock = ((width + 160) * 1_000_000_000 // hperiod_ns) // 250_000 * 250_000
    return dict(width=width, height=height, hblank=160, vblank=vblank,
                hfront=48, hsync=32, vfront=3, vsync=10,
                pixel_clock=pixel_clock, requested_hz=frequency,
                actual_hz=pixel_clock / ((width + 160) * (height + vblank)))


def dtd(mode):
    w, h, hb, vb = (mode[k] for k in ("width", "height", "hblank", "vblank"))
    hf, hs, vf, vs = (mode[k] for k in ("hfront", "hsync", "vfront", "vsync"))
    result = bytearray(18)
    result[:2] = (mode["pixel_clock"] // 10000).to_bytes(2, "little")
    result[2:8] = bytes([w & 255, hb & 255, ((w >> 8) << 4) | (hb >> 8),
                         h & 255, vb & 255, ((h >> 8) << 4) | (vb >> 8)])
    result[8:12] = bytes([hf & 255, hs & 255, ((vf & 15) << 4) | (vs & 15),
                          ((hf >> 8) << 6) | ((hs >> 8) << 4) | ((vf >> 4) << 2) | (vs >> 4)])
    result[17] = 0x1a  # Digital separate sync, +HSync / -VSync.
    return bytes(result)


def timings(data):
    offsets = list(range(54, 126, 18))
    for block in range(128, len(data), 128):
        if data[block] == 2 and data[block + 2] >= 4:
            offsets += list(range(block + data[block + 2], block + 109, 18))
    return [(offset, bytes(data[offset:offset + 18])) for offset in offsets
            if data[offset:offset + 2] != b"\x00\x00"]


def mode_key(descriptor):
    # Physical image dimensions are not part of the signal timing.
    return descriptor[:12] + descriptor[15:]


def make(original, prefer_2160=False):
    validate(original)
    before = timings(original)
    keys = [mode_key(descriptor) for _, descriptor in before]
    replacements = []
    for offset, descriptor in before:
        if offset == 54:  # Keep this slot until the optional preferred-mode swap.
            continue
        if keys.count(mode_key(descriptor)) > 1:
            # Keep the first instance of each timing unless it is the base
            # 720p DTD and another instance remains in CTA.
            remaining = [d for o, d in before if o not in replacements and o != offset]
            if mode_key(descriptor) in [mode_key(d) for d in remaining]:
                replacements.append(offset)
        if len(replacements) == 2:
            break
    if len(replacements) != 2:
        raise ValueError("Need two duplicate DTD slots; refusing to remove existing modes")
    result = bytearray(original)
    modes = [cvt_rb(30), cvt_rb(60)]
    for offset, mode in zip(replacements, modes):
        result[offset:offset + 18] = dtd(mode)
        mode["dtd_offset"] = offset
    if prefer_2160:
        offset = modes[0]["dtd_offset"]
        result[54:72], result[offset:offset + 18] = (
            result[offset:offset + 18], result[54:72])
        modes[0]["dtd_offset"] = 54
        result[24] |= 0x02  # EDID 1.3 preferred timing in the first base DTD.
    for block in range(128, len(result), 128):
        if result[block] == 2:
            first_dtd = block + result[block + 2]
            native_count = result[block + 3] & 15
            if prefer_2160 or any(first_dtd <= offset < first_dtd + native_count * 18
                   for offset in replacements):
                # Added custom timings must not inherit a native DTD flag.
                # Preserve native flags in the unchanged video data block.
                result[block + 3] &= 0xf0
            if prefer_2160:
                index = block + 4
                while index < first_dtd:
                    header = result[index]
                    length = header & 31
                    if index + 1 + length > first_dtd:
                        raise ValueError("Invalid CTA data block boundary")
                    if header >> 5 == 2:
                        for slot in range(index + 1, index + 1 + length):
                            # Keep all VICs, but remove conflicting native flags.
                            # Values >= 193 are extended VICs, not native flags.
                            if 129 <= result[slot] <= 192:
                                result[slot] &= 0x7f
                    index += 1 + length
    for offset in range(0, len(result), 128):
        result[offset + 127] = (-sum(result[offset:offset + 127])) & 255
    validate(result)
    if not prefer_2160:
        assert result[54:72] == original[54:72]
    if not prefer_2160:
        assert result[132:128 + original[130]] == original[132:128 + original[130]]
    assert set(keys).issubset({mode_key(d) for _, d in timings(result)})
    for mode in modes:
        descriptor = result[mode["dtd_offset"]:mode["dtd_offset"] + 18]
        assert descriptor[2] + ((descriptor[4] >> 4) << 8) == 2160
        assert descriptor[5] + ((descriptor[7] >> 4) << 8) == 1920
        assert int.from_bytes(descriptor[:2], "little") * 10000 == mode["pixel_clock"]
        assert abs(mode["actual_hz"] - mode["requested_hz"]) < .1
    return result, modes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("original", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--prefer-2160", action="store_true")
    args = parser.parse_args()
    data, modes = make(args.original.read_bytes(), prefer_2160=args.prefer_2160)
    args.output.write_bytes(data)
    args.output.with_suffix(".json").write_text(json.dumps(modes, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(modes, indent=2))


if __name__ == "__main__":
    main()
