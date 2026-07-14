from pathlib import Path
import math


WIDTH = 1920
HEIGHT = 1080
RAW_PATH = Path(__file__).with_name("cap_nv12.raw")


def plane_stats(data):
    vals = data.tobytes() if hasattr(data, "tobytes") else bytes(data)
    n = len(vals)
    hist = [0] * 256
    total = 0
    total_sq = 0
    mn = 255
    mx = 0
    for b in vals:
        hist[b] += 1
        total += b
        total_sq += b * b
        mn = min(mn, b)
        mx = max(mx, b)

    mean = total / n
    variance = total_sq / n - mean * mean

    def percentile(pct):
        target = (n - 1) * pct / 100.0
        acc = 0
        for value, count in enumerate(hist):
            acc += count
            if acc - 1 >= target:
                return value
        return 255

    top = sorted(((count, value) for value, count in enumerate(hist)),
                 reverse=True)[:8]
    return {
        "n": n,
        "min": mn,
        "max": mx,
        "mean": round(mean, 3),
        "std": round(math.sqrt(max(0.0, variance)), 3),
        "p1": percentile(1),
        "p5": percentile(5),
        "p50": percentile(50),
        "p95": percentile(95),
        "p99": percentile(99),
        "unique": sum(1 for count in hist if count),
        "top": [(value, count) for count, value in top],
    }


def print_y_grid(y_plane):
    print("Y grid mean 8x4:")
    for gy in range(4):
        row = []
        y0 = gy * HEIGHT // 4
        y1 = (gy + 1) * HEIGHT // 4
        for gx in range(8):
            x0 = gx * WIDTH // 8
            x1 = (gx + 1) * WIDTH // 8
            total = 0
            count = 0
            for yy in range(y0, y1):
                seg = y_plane[yy * WIDTH + x0:yy * WIDTH + x1]
                total += sum(seg)
                count += len(seg)
            row.append(f"{total / count:6.1f}")
        print(" ".join(row))


def write_previews(buf, y_plane, uv_plane):
    pgm_path = RAW_PATH.with_name("cap_nv12_y.pgm")
    pgm_path.write_bytes(b"P5\n1920 1080\n255\n" + y_plane.tobytes())

    try:
        from PIL import Image
        import numpy as np
    except Exception as exc:
        print(f"PNG conversion skipped: {exc!r}")
        print(f"wrote {pgm_path}")
        write_bmp_previews(buf, y_plane)
        return

    y = np.frombuffer(buf, dtype=np.uint8, count=WIDTH * HEIGHT).reshape(
        (HEIGHT, WIDTH)).astype(np.float32)
    uv = np.frombuffer(buf, dtype=np.uint8, offset=WIDTH * HEIGHT).reshape(
        (HEIGHT // 2, WIDTH // 2, 2)).astype(np.float32)
    u = uv[:, :, 0].repeat(2, 0).repeat(2, 1) - 128.0
    v = uv[:, :, 1].repeat(2, 0).repeat(2, 1) - 128.0

    c = y - 16.0
    r = 1.164 * c + 1.596 * v
    g = 1.164 * c - 0.392 * u - 0.813 * v
    b = 1.164 * c + 2.017 * u
    rgb = np.dstack([r, g, b]).clip(0, 255).astype(np.uint8)

    rgb_path = RAW_PATH.with_name("cap_nv12_rgb.png")
    y_path = RAW_PATH.with_name("cap_nv12_y.png")
    thumb_path = RAW_PATH.with_name("cap_nv12_rgb_640.png")

    Image.fromarray(rgb, "RGB").save(rgb_path)
    Image.fromarray(
        np.frombuffer(y_plane, dtype=np.uint8).reshape((HEIGHT, WIDTH)),
        "L").save(y_path)
    Image.fromarray(rgb, "RGB").resize(
        (640, 360), Image.Resampling.BILINEAR).save(thumb_path)

    print(f"wrote {rgb_path}")
    print(f"wrote {y_path}")
    print(f"wrote {thumb_path}")


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


def write_bmp_previews(buf, y_plane):
    out_w, out_h = 640, 360
    rgb_rows = []
    gray_rows = []
    for oy in range(out_h):
        sy = oy * HEIGHT // out_h
        rgb_row = bytearray()
        gray_row = bytearray()
        uv_y = (sy // 2) * WIDTH
        for ox in range(out_w):
            sx = ox * WIDTH // out_w
            yv = buf[sy * WIDTH + sx]
            uv_index = WIDTH * HEIGHT + (sy // 2) * WIDTH + (sx // 2) * 2
            u = buf[uv_index] - 128
            v = buf[uv_index + 1] - 128
            c = yv - 16
            r = max(0, min(255, int(1.164 * c + 1.596 * v)))
            g = max(0, min(255, int(1.164 * c - 0.392 * u - 0.813 * v)))
            b = max(0, min(255, int(1.164 * c + 2.017 * u)))
            rgb_row.extend((b, g, r))
            gray_row.extend((yv, yv, yv))
        rgb_rows.append(rgb_row)
        gray_rows.append(gray_row)

    rgb_bmp = RAW_PATH.with_name("cap_nv12_rgb_640.bmp")
    gray_bmp = RAW_PATH.with_name("cap_nv12_y_640.bmp")
    write_24bit_bmp(rgb_bmp, out_w, out_h, rgb_rows)
    write_24bit_bmp(gray_bmp, out_w, out_h, gray_rows)
    print(f"wrote {rgb_bmp}")
    print(f"wrote {gray_bmp}")


def main():
    buf = RAW_PATH.read_bytes()
    expected = WIDTH * HEIGHT * 3 // 2
    print(f"file {RAW_PATH.resolve()}")
    print(f"size {len(buf)} expected {expected} match {len(buf) == expected}")

    y_plane = memoryview(buf)[:WIDTH * HEIGHT]
    uv_plane = memoryview(buf)[WIDTH * HEIGHT:]

    print("Y stats", plane_stats(y_plane))
    print("U stats", plane_stats(uv_plane[0::2]))
    print("V stats", plane_stats(uv_plane[1::2]))
    print_y_grid(y_plane)

    line_means = []
    for yy in range(HEIGHT):
        line = y_plane[yy * WIDTH:(yy + 1) * WIDTH]
        line_means.append(sum(line) / WIDTH)
    print(
        "line_mean min/max",
        round(min(line_means), 3),
        round(max(line_means), 3),
        "first10",
        [round(v, 1) for v in line_means[:10]],
    )

    write_previews(buf, y_plane, uv_plane)


if __name__ == "__main__":
    main()
