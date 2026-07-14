from __future__ import annotations

import argparse
import math
from pathlib import Path


def plane_stats(values):
    data = values.tobytes() if hasattr(values, "tobytes") else bytes(values)
    if not data:
        return {}
    hist = [0] * 256
    total = 0
    total_sq = 0
    mn = 255
    mx = 0
    for value in data:
        hist[value] += 1
        total += value
        total_sq += value * value
        mn = min(mn, value)
        mx = max(mx, value)
    n = len(data)
    mean = total / n
    var = max(0.0, total_sq / n - mean * mean)

    def percentile(pct):
        target = int((n - 1) * pct / 100.0)
        acc = 0
        for value, count in enumerate(hist):
            acc += count
            if acc > target:
                return value
        return 255

    top = sorted(((count, value) for value, count in enumerate(hist)), reverse=True)[:8]
    return {
        "n": n,
        "min": mn,
        "max": mx,
        "mean": round(mean, 3),
        "std": round(math.sqrt(var), 3),
        "p1": percentile(1),
        "p5": percentile(5),
        "p50": percentile(50),
        "p95": percentile(95),
        "p99": percentile(99),
        "unique": sum(1 for count in hist if count),
        "top": [(value, count) for count, value in top],
    }


def detect_active_region(y, width, height):
    # Avoid very top/bottom UI noise and look for columns that carry real content.
    y0 = height // 20
    y1 = height - height // 20
    sample_step = 4
    col_mean = [0.0] * width
    col_p90 = [0.0] * width
    col_std = [0.0] * width
    active = [False] * width

    for x in range(width):
        vals = []
        for yy in range(y0, y1, sample_step):
            vals.append(y[yy * width + x])
        if not vals:
            continue
        vals.sort()
        n = len(vals)
        total = sum(vals)
        mean = total / n
        p90 = vals[min(n - 1, int(n * 0.9))]
        var = sum((v - mean) * (v - mean) for v in vals) / n
        std = math.sqrt(var)
        col_mean[x] = mean
        col_p90[x] = p90
        col_std[x] = std
        active[x] = p90 > 36 or (mean > 24 and std > 4.5)

    if not any(active):
        return 0, width, {
            "active_found": False,
            "threshold": "p90>36 or mean>24/std>4.5",
        }

    left = active.index(True)
    right = width - list(reversed(active)).index(True)

    # Trim small isolated columns around the main content by requiring local support.
    window = max(9, width // 160)
    need = max(3, window // 3)
    half = window // 2
    supported = [False] * width
    running = 0
    active_int = [1 if value else 0 for value in active]
    for x in range(width):
        add = x + half
        remove = x - half - 1
        if add < width:
            running += active_int[add]
        if remove >= 0:
            running -= active_int[remove]
        supported[x] = running >= need
    if any(supported):
        left = supported.index(True)
        right = width - list(reversed(supported)).index(True)

    def avg(values):
        return sum(values) / len(values) if values else None

    def rounded(value):
        return round(value, 3) if value is not None else None

    stats = {
        "active_found": True,
        "threshold": "p90>36 or mean>24/std>4.5",
        "left_mean": rounded(avg(col_mean[:left])),
        "right_mean": rounded(avg(col_mean[right:])),
        "active_mean": rounded(avg(col_mean[left:right])),
        "left_p90": rounded(avg(col_p90[:left])),
        "right_p90": rounded(avg(col_p90[right:])),
        "active_p90": rounded(avg(col_p90[left:right])),
        "active_std": rounded(avg(col_std[left:right])),
    }
    return left, right, stats


def save_previews(raw_path, buf, width, height, left, right, out_prefix):
    try:
        from PIL import Image
        import numpy as np
    except Exception as exc:
        print(f"PNG conversion skipped: {exc!r}")
        write_bmp_previews(buf, width, height, left, right, out_prefix)
        return

    y = np.frombuffer(buf, dtype=np.uint8, count=width * height).reshape((height, width))
    uv = np.frombuffer(buf, dtype=np.uint8, offset=width * height).reshape((height, width // 2, 2))

    yf = y.astype(np.float32)
    u = uv[:, :, 0].astype(np.float32).repeat(2, axis=1) - 128.0
    v = uv[:, :, 1].astype(np.float32).repeat(2, axis=1) - 128.0
    c = yf - 16.0
    r = 1.164 * c + 1.596 * v
    g = 1.164 * c - 0.392 * u - 0.813 * v
    b = 1.164 * c + 2.017 * u
    rgb = np.dstack([r, g, b]).clip(0, 255).astype(np.uint8)

    full_rgb = out_prefix.with_name(out_prefix.name + "_rgb.png")
    full_y = out_prefix.with_name(out_prefix.name + "_y.png")
    thumb = out_prefix.with_name(out_prefix.name + "_rgb_960.png")
    active_rgb = out_prefix.with_name(out_prefix.name + "_active_rgb.png")
    active_y = out_prefix.with_name(out_prefix.name + "_active_y.png")

    Image.fromarray(rgb, "RGB").save(full_rgb)
    Image.fromarray(y, "L").save(full_y)
    Image.fromarray(rgb, "RGB").resize((960, 540), Image.Resampling.BILINEAR).save(thumb)
    Image.fromarray(rgb[:, left:right, :], "RGB").save(active_rgb)
    Image.fromarray(y[:, left:right], "L").save(active_y)

    print(f"wrote {full_rgb}")
    print(f"wrote {full_y}")
    print(f"wrote {thumb}")
    print(f"wrote {active_rgb}")
    print(f"wrote {active_y}")


def write_24bit_bmp(path, width, height, rgb_rows_top_down):
    row_bytes = width * 3
    padding = (4 - (row_bytes % 4)) % 4
    pixel_bytes = (row_bytes + padding) * height
    file_size = 14 + 40 + pixel_bytes

    header = bytearray()
    header.extend(b"BM")
    header.extend(file_size.to_bytes(4, "little"))
    header.extend((0).to_bytes(4, "little"))
    header.extend((54).to_bytes(4, "little"))
    header.extend((40).to_bytes(4, "little"))
    header.extend(width.to_bytes(4, "little"))
    header.extend(height.to_bytes(4, "little"))
    header.extend((1).to_bytes(2, "little"))
    header.extend((24).to_bytes(2, "little"))
    header.extend((0).to_bytes(4, "little"))
    header.extend(pixel_bytes.to_bytes(4, "little"))
    header.extend((2835).to_bytes(4, "little"))
    header.extend((2835).to_bytes(4, "little"))
    header.extend((0).to_bytes(4, "little"))
    header.extend((0).to_bytes(4, "little"))

    data = bytearray(header)
    pad = b"\x00" * padding
    for row in reversed(rgb_rows_top_down):
        data.extend(row)
        data.extend(pad)
    path.write_bytes(data)


def nv16_pixel_to_bgr(buf, width, height, x, y):
    yv = buf[y * width + x]
    uv_index = width * height + y * width + (x // 2) * 2
    u = buf[uv_index] - 128
    v = buf[uv_index + 1] - 128
    c = yv - 16
    r = max(0, min(255, int(1.164 * c + 1.596 * v)))
    g = max(0, min(255, int(1.164 * c - 0.392 * u - 0.813 * v)))
    b = max(0, min(255, int(1.164 * c + 2.017 * u)))
    return b, g, r


def write_scaled_bmp(buf, width, height, src_left, src_right, out_w, out_h, path):
    src_w = max(1, src_right - src_left)
    rows = []
    for oy in range(out_h):
        sy = min(height - 1, oy * height // out_h)
        row = bytearray()
        for ox in range(out_w):
            sx = min(src_right - 1, src_left + ox * src_w // out_w)
            row.extend(nv16_pixel_to_bgr(buf, width, height, sx, sy))
        rows.append(row)
    write_24bit_bmp(path, out_w, out_h, rows)


def write_bmp_previews(buf, width, height, left, right, out_prefix):
    full = out_prefix.with_name(out_prefix.name + "_rgb_960.bmp")
    active = out_prefix.with_name(out_prefix.name + "_active_rgb_540h.bmp")
    y_full = out_prefix.with_name(out_prefix.name + "_y_960.bmp")

    write_scaled_bmp(buf, width, height, 0, width, 960, 540, full)
    active_w = max(1, right - left)
    active_out_w = max(1, int(round(540 * active_w / height)))
    write_scaled_bmp(buf, width, height, left, right, active_out_w, 540, active)

    y_rows = []
    for oy in range(540):
        sy = min(height - 1, oy * height // 540)
        row = bytearray()
        for ox in range(960):
            sx = min(width - 1, ox * width // 960)
            yv = buf[sy * width + sx]
            row.extend((yv, yv, yv))
        y_rows.append(row)
    write_24bit_bmp(y_full, 960, 540, y_rows)

    print(f"wrote {full}")
    print(f"wrote {active}")
    print(f"wrote {y_full}")


def main():
    parser = argparse.ArgumentParser(description="Analyze one NV16/YUV422 frame.")
    parser.add_argument("raw", type=Path)
    parser.add_argument("--width", type=int, default=1920)
    parser.add_argument("--height", type=int, default=1080)
    args = parser.parse_args()

    raw_path = args.raw
    width = args.width
    height = args.height
    expected = width * height * 2
    buf = raw_path.read_bytes()
    print(f"file {raw_path.resolve()}")
    print(f"size {len(buf)} expected {expected} match {len(buf) == expected}")
    if len(buf) < expected:
        raise SystemExit("raw file is smaller than expected")
    if len(buf) > expected:
        buf = buf[:expected]

    y = memoryview(buf)[: width * height]
    uv = memoryview(buf)[width * height :]

    print("Y stats", plane_stats(y))
    print("U stats", plane_stats(uv[0::2]))
    print("V stats", plane_stats(uv[1::2]))

    print("Y grid mean 8x4:")
    for gy in range(4):
        row = []
        y0 = gy * height // 4
        y1 = (gy + 1) * height // 4
        for gx in range(8):
            x0 = gx * width // 8
            x1 = (gx + 1) * width // 8
            total = 0
            count = 0
            for yy in range(y0, y1):
                seg = y[yy * width + x0 : yy * width + x1]
                total += sum(seg)
                count += len(seg)
            row.append(f"{total / count:6.1f}")
        print(" ".join(row))

    left, right, active_stats = detect_active_region(y, width, height)
    active_width = right - left
    print("active_region", {
        "left": left,
        "right": right,
        "left_bar": left,
        "right_bar": width - right,
        "active_width": active_width,
        "active_height": height,
        "active_ratio": round(active_width / width, 4),
        "scale_to_1016x2160": {
            "x": round(1016 / active_width, 3) if active_width > 0 else None,
            "y": round(2160 / height, 3),
        },
    })
    print("active_stats", active_stats)

    out_prefix = raw_path.with_suffix("")
    save_previews(raw_path, buf, width, height, left, right, out_prefix)


if __name__ == "__main__":
    main()
