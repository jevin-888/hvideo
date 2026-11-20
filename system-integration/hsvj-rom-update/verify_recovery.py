#!/usr/bin/env python3
"""Validate the freshly built internal Recovery without booting or flashing it."""
import gzip
import hashlib
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def verify_android_storage(source, product):
    """Require native first-boot initialization and permissions before app startup."""
    normal_mounts = 0
    required = {"latemount", "wait", "check", "formattable", "nofail"}
    for path in (product / "vendor/etc").glob("fstab*"):
        mounts = []
        for line in path.read_text().splitlines():
            fields = line.split()
            if not fields or fields[0].startswith("#"):
                continue
            if len(fields) >= 2 and fields[1] == "/huoshan":
                mounts.append(fields)
        if len(mounts) > 1:
            raise ValueError("Ambiguous Android huoshan mount: " + str(path))
        for fields in mounts:
            if (len(fields) < 5 or fields[0] != "/dev/block/by-name/huoshan"
                    or fields[2] != "ext4" or set(fields[4].split(",")) != required):
                raise ValueError("Android huoshan must mount ext4 and initialize wiped partitions")
            normal_mounts += 1
    if not normal_mounts:
        raise ValueError("Android huoshan mount configuration missing")
    if not (product / "root/huoshan").is_dir():
        raise ValueError("Android huoshan mount point missing")
    for name in ("mke2fs", "e2fsdroid", "e2fsck"):
        tool = product / "system/bin" / name
        if not tool.is_file() or not os.access(tool, os.X_OK):
            raise ValueError("Android huoshan executable missing: " + name)
    original = source / "device/rockchip/rk356x_box/h66_por/init.h66_por.rc"
    installed = product / "vendor/etc/init/init.h66_por.rc"
    if not installed.is_file() or installed.read_bytes() != original.read_bytes():
        raise ValueError("Android huoshan boot permissions differ from source")
    actions = []
    in_post_fs_data = False
    for line in installed.read_text().splitlines():
        clean = line.strip()
        if not clean or clean.startswith("#"):
            continue
        if not line[0].isspace():
            in_post_fs_data = clean == "on post-fs-data"
        elif in_post_fs_data:
            actions.append(clean)
    expected = ["chown system system /huoshan", "chmod 0777 /huoshan",
                "restorecon_recursive /huoshan"]
    if not all(action in actions for action in expected):
        raise ValueError("Android huoshan permissions must be ready in post-fs-data")


def verify(source, image):
    with tempfile.TemporaryDirectory(prefix="hsvj-recovery-verify-") as name:
        temp = Path(name)
        subprocess.run(["python3", str(source / "system/tools/mkbootimg/unpack_bootimg.py"), "--boot_img", str(image), "--out", str(temp)], check=True, stdout=subprocess.DEVNULL)
        stable = source / "vendor/chuangwei-minimal-perf/prebuilts/H66Boot/Image"
        if hashlib.sha256((temp / "kernel").read_bytes()).digest() != hashlib.sha256(stable.read_bytes()).digest():
            raise ValueError("Recovery changed the stable kernel")
        archive = gzip.decompress((temp / "ramdisk").read_bytes())

        def member(path):
            data = subprocess.run(["cpio", "-i", "--to-stdout", path], input=archive, capture_output=True, check=True).stdout
            if not data:
                raise ValueError("Missing Recovery member: " + path)
            return data

        helper = member("system/bin/hsvj-rom-update")
        if b"installer must be invoked by Recovery" not in helper or b"release signature rejected" not in helper:
            raise ValueError("Protected installer missing")
        if b"cannot mount huoshan; refusing to format" not in helper:
            raise ValueError("Recovery must not initialize huoshan during an internal upgrade")
        if b"--hsvj_update=" not in member("system/bin/recovery"):
            raise ValueError("Recovery entry point missing")
        member("system/bin/linker64")
        file = temp / "helper"
        file.write_bytes(helper)
        elf = subprocess.run(["readelf", "-d", str(file)], check=True, capture_output=True, text=True).stdout
        for library in re.findall(r"\(NEEDED\).*\[(.*?)\]", elf):
            member("system/lib64/" + library)
        fstab = member("system/etc/recovery.fstab").decode()
        for partition, mount in [("huoshan", "/huoshan"), ("userdata", "/data")]:
            if not re.search(r"^/dev/block/by-name/" + partition + r"\s+" + mount + r"\s+ext4\s", fstab, re.M):
                raise ValueError("Recovery ext4 mount mismatch: " + partition)
        for line in fstab.splitlines():
            fields = line.split()
            if (len(fields) >= 5 and fields[1] == "/huoshan"
                    and "formattable" in fields[4].split(",")):
                raise ValueError("Recovery huoshan must remain non-formattable")
        if image.stat().st_size > 0x30000 * 512:
            raise ValueError("Recovery exceeds partition")
        verify_android_storage(source, image.parent)
        print("Recovery verified: stable kernel, protected upgrades, dependencies; Android huoshan first-boot initialization and early permissions")


if __name__ == "__main__":
    verify(Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve())
