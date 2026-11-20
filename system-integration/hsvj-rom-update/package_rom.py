#!/usr/bin/env python3
"""Build an authenticated internal-update bundle from the canonical H66 build.

Runs in the Android build host. Preparing a bundle never publishes it.
"""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
from verify_recovery import verify as verify_recovery
from verify_runtime import verify as verify_runtime

PARAMETER_SHA256 = "b05b590c79b35c838581803b3810aad7164380176deb51b47606b7bd4371c247"
PARTITIONS = {"trust": (0x6000, 0x2000), "uboot": (0x4000, 0x2000),
              "dtbo": (0xa000, 0x2000), "vbmeta": (0xc000, 0x800),
              "super": (0x1db000, 0x614000), "userdata": (0x7ef000, 0x448000),
              "boot": (0xc800, 0x14000), "recovery": (0x20800, 0x30000)}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path, offset=0, size=None):
    h = hashlib.sha256()
    with Path(path).open("rb") as f:
        f.seek(offset)
        left = Path(path).stat().st_size - offset if size is None else size
        while left:
            data = f.read(min(left, 1024 * 1024))
            require(data, "truncated image")
            h.update(data)
            left -= len(data)
    return h.hexdigest()


def parse_image(path):
    total = path.stat().st_size
    require(1024 < total <= 4 * 1024**3, "image size out of bounds")
    with path.open("rb") as f:
        header = f.read(102)
        require(header[:4] == b"RKFW" and struct.unpack_from("<H", header, 4)[0] == 102, "invalid RKFW header")
        base, length = struct.unpack_from("<II", header, 33)
        require(base >= 102 and base + length <= total, "invalid firmware bounds")
        f.seek(base)
        rkaf = f.read(140)
        require(rkaf[:4] == b"RKAF", "missing RKAF")
        count = struct.unpack_from("<I", rkaf, 136)[0]
        require(count == 9, "expected parameter and eight image entries")
        entries = {}
        ranges = []
        for _ in range(count):
            row = f.read(112)
            require(len(row) == 112, "truncated RKAF entry")
            name_bytes, file_bytes, sectors, offset, flash, used, size = struct.unpack("<32s60sIIIII", row)
            name = name_bytes.split(b"\0", 1)[0].decode("ascii")
            filename = file_bytes.split(b"\0", 1)[0].decode("ascii")
            require(name not in entries and name in {*PARTITIONS, "parameter"}, "unknown/protected/duplicate entry")
            require(offset >= 140 + 112 * count and size > 0 and offset + size <= length, "entry out of bounds")
            require(all(offset + size <= start or offset >= end for start, end in ranges), "overlapping entries")
            ranges.append((offset, offset + size))
            if name in PARTITIONS:
                require(flash == PARTITIONS[name][0], "partition offset changed: " + name)
                require(filename == "Image/" + name + ".img", "unexpected package path")
            else:
                require(filename == "Image/parameter.txt", "unexpected parameter path")
            entries[name] = {"name": name, "offset": base + offset, "size": size}
    parameter = entries["parameter"]
    with path.open("rb") as f:
        f.seek(parameter["offset"])
        wrapper = f.read(8)
    require(wrapper[:4] == b"PARM", "missing parameter wrapper")
    parameter_size = struct.unpack_from("<I", wrapper, 4)[0]
    require(parameter_size + 12 == parameter["size"], "parameter wrapper length mismatch")
    require(digest(path, parameter["offset"] + 8, parameter_size) == PARAMETER_SHA256, "partition map changed")
    return entries


def properties(path):
    result = {}
    for line in path.read_text().splitlines():
        if line and not line.startswith("#") and "=" in line:
            key, value = line.split("=", 1)
            result[key] = value
    return result


def prepare(args):
    source = args.android_root.resolve()
    product = source / "out-hsvj-user/target/product/h66_por"
    props = properties(product / "system/build.prop")
    props.update(properties(product / "system/etc/prop.default"))
    require(props.get("ro.hsvj.rom_update.protocol") == "1" and props.get("ro.hsvj.rom.version") == str(args.version), "built ROM protocol/version mismatch")
    # init derives this from the production identity overrides in vendor/build.prop.
    vendor = properties(product / "vendor/build.prop")
    require(vendor.get("ro.product.device") == "H6_POR" and vendor.get("ro.product.name") == "H6_POR", "product identity mismatch")
    fingerprint = "CHUANGWEI/H6_POR/H6_POR:" + props["ro.build.version.release"] + "/" + props["ro.build.id"] + "/" + props["ro.build.version.incremental"] + ":" + props["ro.build.type"] + "/" + props["ro.build.tags"]
    entries = parse_image(args.image)
    require(args.version > 31, "internal ROM bundles start at v32")
    verify_recovery(source, product / "recovery.img")
    verify_runtime(source)
    with tempfile.TemporaryDirectory(prefix="hsvj-rom-package-") as temp_name:
        temp = Path(temp_name)
        recovery = product / "recovery.img"
        require(digest(recovery) == digest(args.image, entries["recovery"]["offset"], entries["recovery"]["size"]), "Recovery does not match current build")
        subprocess.run(["python3", str(source / "system/tools/mkbootimg/unpack_bootimg.py"), "--boot_img", str(recovery), "--out", str(temp / "recovery")], check=True, stdout=subprocess.DEVNULL)
        ramdisk = (temp / "recovery/ramdisk").read_bytes()
        archive = gzip.decompress(ramdisk)
        installed = subprocess.run(["cpio", "-i", "--to-stdout", "system/bin/hsvj-rom-update"], input=archive, capture_output=True, check=True).stdout
        require(installed and b"installer must be invoked by Recovery" in installed, "protected updater missing from Recovery")
        built = (product / "system/bin/hsvj-rom-update").read_bytes()
        require(b"release signature rejected" in built, "Android ROM controller missing")
        require(args.hardware.encode("ascii") + b"\0" in built and
                args.hardware.encode("ascii") + b"\0" in installed,
                "Android/Recovery controllers must be rebuilt for the signed hardware target")
        rows = []
        for name, (_, sectors) in PARTITIONS.items():
            row = entries[name]
            image = args.pack_dir / "Image" / (name + ".img")
            row["sha256"] = digest(args.image, row["offset"], row["size"])
            require(image.stat().st_size == row["size"] and digest(image) == row["sha256"], "packed image mismatch: " + name)
            if name in {"boot", "recovery", "super", "userdata"}:
                require(digest(product / (name + ".img")) == row["sha256"], "image differs from current build: " + name)
            with image.open("rb") as f:
                sparse = f.read(4) == b"\x3a\xff\x26\xed"
            raw = temp / "expanded.img"
            if sparse:
                subprocess.run([str(source / "out-hsvj-user/host/linux-x86/bin/simg2img"), str(image), str(raw)], check=True)
            else:
                raw = image
            row["expanded_size"] = raw.stat().st_size
            require(0 < row["expanded_size"] <= sectors * 512, "expanded partition overflow")
            row["expanded_sha256"] = digest(raw)
            if sparse:
                raw.unlink()
            rows.append(row)
        manifest = {"protocol": 1, "product": "H6_POR", "hardware": args.hardware, "version_code": args.version,
                    "target_fingerprint": fingerprint, "image_size": args.image.stat().st_size,
                    "image_sha256": digest(args.image), "partitions": rows}
        data = (json.dumps(manifest, sort_keys=True, separators=(",", ":")) + "\n").encode("ascii")
        release_id = hashlib.sha256(data).hexdigest()
        manifest_file = temp / "manifest.json"
        manifest_file.write_bytes(data)
        signature = temp / "manifest.sig"
        key = source / "vendor/chuangwei-security8.1/platform.pk8"
        subprocess.run(["openssl", "dgst", "-sha256", "-keyform", "DER", "-sign", str(key), "-out", str(signature), str(manifest_file)], check=True)
        cert = source / "vendor/chuangwei-security8.1/platform.x509.pem"
        public_key = subprocess.run(["openssl", "x509", "-in", str(cert), "-pubkey", "-noout"], capture_output=True, check=True).stdout
        pub = temp / "public.pem"
        pub.write_bytes(public_key)
        subprocess.run(["openssl", "dgst", "-sha256", "-verify", str(pub), "-signature", str(signature), str(manifest_file)], check=True)
        compatibility_files = []
        for compatibility in ("hw81", "hw82"):
            compatibility_manifest = dict(manifest)
            compatibility_manifest["hardware"] = compatibility
            compatibility_data = (json.dumps(compatibility_manifest, sort_keys=True, separators=(",", ":")) + "\n").encode("ascii")
            compatibility_file = temp / f"manifest-{compatibility}.json"
            compatibility_signature = temp / f"manifest-{compatibility}.sig"
            compatibility_file.write_bytes(compatibility_data)
            subprocess.run(["openssl", "dgst", "-sha256", "-keyform", "DER", "-sign", str(key), "-out", str(compatibility_signature), str(compatibility_file)], check=True)
            subprocess.run(["openssl", "dgst", "-sha256", "-verify", str(pub), "-signature", str(compatibility_signature), str(compatibility_file)], check=True)
            compatibility_files.extend((compatibility_file, compatibility_signature))
        # Local delivery uses the human-readable ROM version. The signed
        # release_id remains the manifest hash used by the server/device.
        output = args.output / ("V" + str(args.version))
        if output.exists():
            # A retry may finish records for an already verified delivery, but
            # must never overwrite a different image under the same version.
            require((output / "manifest.json").read_bytes() == data,
                    "existing version bundle differs from current build")
            subprocess.run([str(source / "out-hsvj-user/host/linux-x86/bin/hsvj-rom-update"), "--verify-bundle", str(output.resolve()) + "/", release_id], check=True)
            for source_file in compatibility_files:
                target = output / source_file.name
                if target.exists():
                    require(target.read_bytes() == source_file.read_bytes(), "existing hardware manifest differs: " + target.name)
                else:
                    shutil.copyfile(source_file, target)
            print(json.dumps({"bundle": str(output), "release_id": release_id, "version_code": args.version, "fingerprint": fingerprint, "published": False}))
            return
        args.output.mkdir(parents=True, exist_ok=True)
        pending = Path(tempfile.mkdtemp(prefix=".preparing-", dir=args.output))
        try:
            shutil.copyfile(manifest_file, pending / "manifest.json")
            shutil.copyfile(signature, pending / "manifest.sig")
            for source_file in compatibility_files:
                shutil.copyfile(source_file, pending / source_file.name)
            shutil.copyfile(args.image, pending / "update.img")
            require(digest(pending / "update.img") == manifest["image_sha256"], "image changed during packaging")
            for path in pending.iterdir():
                with path.open("rb") as stream:
                    os.fsync(stream.fileno())
            subprocess.run([str(source / "out-hsvj-user/host/linux-x86/bin/hsvj-rom-update"), "--verify-bundle", str(pending.resolve()) + "/", release_id], check=True)
            pending.rename(output)
        finally:
            if pending.exists():
                shutil.rmtree(pending)
        print(json.dumps({"bundle": str(output), "release_id": release_id, "version_code": args.version, "fingerprint": fingerprint, "published": False}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--android-root", type=Path, required=True)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--pack-dir", type=Path, required=True)
    parser.add_argument("--version", type=int, required=True)
    parser.add_argument("--hardware", choices=("universal", "hw81", "hw82"), default="universal",
                        help="manifest metadata (universal removes the new-ROM hardware gate)")
    parser.add_argument("--output", type=Path, required=True)
    prepare(parser.parse_args())
