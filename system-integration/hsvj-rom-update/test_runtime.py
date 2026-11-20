#!/usr/bin/env python3
"""Regression checks for boot staging permissions and ART property validation."""
from pathlib import Path
import os
import tempfile
import unittest
from verify_runtime import verify, verify_permissions


class RuntimeTests(unittest.TestCase):
    def test_old_recursive_repair_is_rejected(self):
        with tempfile.TemporaryDirectory() as name:
            script = Path(name) / "old.sh"
            script.write_text("if [ -d /huoshan ]; then\n"
                              "    chown -R system:system /huoshan\n"
                              "    find /huoshan -type d -exec chmod 0777 {} +\n"
                              "    find /huoshan -type f -exec chmod 0666 {} +\n"
                              "fi\n")
            with self.assertRaisesRegex(ValueError, "protected"):
                verify_permissions(script)

    def test_missing_and_conflicting_art_property_are_rejected(self):
        with tempfile.TemporaryDirectory() as name:
            source = Path(name)
            original = source / "vendor/chuangwei-minimal-perf/hsvj_perf_tune.sh"
            product = source / "out-hsvj-user/target/product/h66_por"
            installed = product / "vendor/bin/hsvj_perf_tune.sh"
            # Exercise the exact production script supplied by the caller.
            script = Path(os.environ["HSVJ_PERF_SCRIPT"]).read_bytes()
            for path in (original, installed):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(script)
            with self.assertRaisesRegex(ValueError, "must enable"):
                verify(source)
            prop = product / "system/etc/prop.default"
            prop.parent.mkdir(parents=True)
            prop.write_text("dalvik.vm.dex2oat64.enabled=true\n")
            with self.assertRaisesRegex(ValueError, "executable missing"):
                verify(source)
            compiler = product / "system/apex/com.android.art.release/bin/dex2oat64"
            compiler.parent.mkdir(parents=True)
            compiler.write_bytes(b"\x7fELF\x02\x01" + bytes(12) + b"\xb7\x00")
            compiler.chmod(0o755)
            verify(source)
            override = product / "product/build.prop"
            override.parent.mkdir()
            override.write_text("dalvik.vm.dex2oat64.enabled=false\n")
            with self.assertRaisesRegex(ValueError, "conflicting"):
                verify(source)


if __name__ == "__main__":
    unittest.main()
