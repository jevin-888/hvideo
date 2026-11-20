#!/usr/bin/env python3
"""Reject ROMs that reproduce blank-storage startup failures."""
from pathlib import Path
import tempfile
import unittest

from verify_recovery import verify_android_storage


class StorageBuildTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.source = Path(self.temp.name)
        self.product = self.source / "product"
        self.fstab = self.product / "vendor/etc/fstab.rk30board"
        self.fstab.parent.mkdir(parents=True)
        self.fstab.write_text("/dev/block/by-name/huoshan /huoshan ext4 "
                              "noatime,nosuid,nodev latemount,wait,check,formattable,nofail\n")
        (self.product / "root/huoshan").mkdir(parents=True)
        for name in ("mke2fs", "e2fsdroid", "e2fsck"):
            path = self.product / "system/bin" / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("#!/bin/sh\nexit 0\n")
            path.chmod(0o755)
        self.original = self.source / "device/rockchip/rk356x_box/h66_por/init.h66_por.rc"
        self.installed = self.product / "vendor/etc/init/init.h66_por.rc"
        self.init_text = ("on post-fs-data\n    chown system system /huoshan\n"
                          "    chmod 0777 /huoshan\n    restorecon_recursive /huoshan\n")
        self.set_init(self.init_text)

    def set_init(self, text):
        for path in (self.original, self.installed):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)

    def test_complete_first_boot_chain_passes(self):
        verify_android_storage(self.source, self.product)

    def test_old_v39_fstab_is_rejected(self):
        self.fstab.write_text(self.fstab.read_text().replace(",formattable", ""))
        with self.assertRaisesRegex(ValueError, "initialize wiped"):
            verify_android_storage(self.source, self.product)

    def test_missing_mount_directory_is_rejected(self):
        (self.product / "root/huoshan").rmdir()
        with self.assertRaisesRegex(ValueError, "mount point missing"):
            verify_android_storage(self.source, self.product)

    def test_missing_format_tool_is_rejected(self):
        (self.product / "system/bin/e2fsdroid").unlink()
        with self.assertRaisesRegex(ValueError, "executable missing"):
            verify_android_storage(self.source, self.product)

    def test_permissions_only_after_boot_are_rejected(self):
        self.set_init(self.init_text.replace("on post-fs-data", "on property:sys.boot_completed=1"))
        with self.assertRaisesRegex(ValueError, "ready in post-fs-data"):
            verify_android_storage(self.source, self.product)

    def test_stale_installed_init_is_rejected(self):
        self.installed.write_text(self.init_text.replace("    chmod 0777 /huoshan\n", ""))
        with self.assertRaisesRegex(ValueError, "differ from source"):
            verify_android_storage(self.source, self.product)

    def test_duplicate_mount_is_rejected(self):
        self.fstab.write_text(self.fstab.read_text() * 2)
        with self.assertRaisesRegex(ValueError, "Ambiguous"):
            verify_android_storage(self.source, self.product)


if __name__ == "__main__":
    unittest.main()
