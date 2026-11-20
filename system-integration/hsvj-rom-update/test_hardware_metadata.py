"""Read-only native verifier regression using a real signed production bundle.

Run under WSL with HSVJ_TEST_BUNDLE and HSVJ_ANDROID_ROOT set. No device access.
"""
import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


@unittest.skipUnless(os.environ.get("HSVJ_TEST_BUNDLE") and os.environ.get("HSVJ_ANDROID_ROOT"),
                     "requires real bundle and built native host verifier")
class HardwareMetadataTests(unittest.TestCase):
    def test_metadata_and_remaining_guards(self):
        bundle = Path(os.environ["HSVJ_TEST_BUNDLE"]).resolve()
        android = Path(os.environ["HSVJ_ANDROID_ROOT"]).resolve()
        verifier = android / "out-hsvj-user/host/linux-x86/bin/hsvj-rom-update"
        key = android / "vendor/chuangwei-security8.1/platform.pk8"
        original = json.loads((bundle / "manifest.json").read_bytes())
        # Same filesystem for a read-only hard link; no large test copy remains.
        with tempfile.TemporaryDirectory(prefix=".hardware-test-", dir=bundle.parent) as directory:
            root = Path(directory)
            os.link(bundle / "update.img", root / "update.img")

            def check(value, expected=None, corrupt=False):
                data = (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode()
                (root / "manifest.json").write_bytes(data)
                subprocess.run(["openssl", "dgst", "-sha256", "-keyform", "DER", "-sign", str(key),
                                "-out", str(root / "manifest.sig"), str(root / "manifest.json")], check=True)
                if corrupt:
                    (root / "manifest.sig").write_bytes(bytes(256))
                result = subprocess.run([str(verifier), "--verify-bundle", str(root) + "/",
                                         hashlib.sha256(data).hexdigest()], capture_output=True, text=True)
                if expected is None:
                    self.assertEqual(result.returncode, 0, result.stderr)
                else:
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(expected, result.stderr)

            for hardware in ["hw81", "hw82", "universal", "legacy-label"]:
                with self.subTest(hardware=hardware):
                    value = copy.deepcopy(original)
                    value["hardware"] = hardware
                    check(value)
            for field, value, error in [
                ("product", "OTHER", "unsupported release target"),
                ("protocol", 2, "unsupported release target"),
                ("hardware", 81, "invalid hardware metadata"),
                ("image_sha256", "0" * 64, "firmware SHA256 mismatch"),
            ]:
                with self.subTest(field=field):
                    changed = copy.deepcopy(original)
                    changed[field] = value
                    check(changed, error)
            check(original, "release signature rejected", corrupt=True)


if __name__ == "__main__":
    unittest.main()
