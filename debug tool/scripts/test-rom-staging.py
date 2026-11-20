"""Exercise the production Android directory preparation on a Linux tmpfs.

Run as root under WSL: ownership migration uses real chown, not a mock.
"""
import os
from pathlib import Path
import re
import stat
import subprocess
import tempfile
import unittest


SOURCE = Path(__file__).resolve().parents[1] / "src-tauri/src/system_update.rs"
SCRIPT = re.search(r'const PREPARE_ROM_DIRS: &str = r#"(.*?)"#;',
                   SOURCE.read_text(encoding="utf-8"), re.S).group(1)


@unittest.skipUnless(os.geteuid() == 0, "requires root for ownership checks")
class StagingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="hsvj-staging-", dir="/dev/shm")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.storage = self.root / "huoshan"
        self.storage.mkdir()
        self.stage = self.storage / ".rom-update"
        self.download = self.storage / ".rom-download"

    def prepare(self):
        return subprocess.run(["sh", "-c", SCRIPT.replace("/huoshan", str(self.storage))],
                              capture_output=True).returncode

    def test_legacy_owner_repaired_without_changing_payload(self):
        self.stage.mkdir(mode=0o700)
        os.chown(self.stage, 1000, 1000)
        state = self.stage / "status.json"
        state.write_bytes(b'{"phase":"succeeded"}')
        state.chmod(0o600)
        os.chown(state, 1000, 1000)
        self.download.mkdir(mode=0o777)
        os.chown(self.download, 1000, 1000)
        for _ in range(2):
            self.assertEqual(self.prepare(), 0)
            self.assertEqual(self.stage.stat().st_uid, 0)
            self.assertEqual(stat.S_IMODE(self.stage.stat().st_mode), 0o700)
            self.assertEqual(self.download.stat().st_uid, 1000)
            self.assertEqual(stat.S_IMODE(self.download.stat().st_mode), 0o777)
            self.assertEqual(state.read_bytes(), b'{"phase":"succeeded"}')
            self.assertEqual(state.stat().st_uid, 1000)

    def test_fresh_directories(self):
        self.assertEqual(self.prepare(), 0)
        self.assertTrue(self.download.is_dir())
        self.assertEqual(self.download.stat().st_uid, 1000)
        self.assertEqual(stat.S_IMODE(self.download.stat().st_mode), 0o777)
        self.assertEqual(stat.S_IMODE(self.stage.stat().st_mode), 0o700)

    def test_symlink_cannot_redirect_root_ownership(self):
        outside = self.root / "outside"
        outside.mkdir(mode=0o755)
        os.chown(outside, 1000, 1000)
        for path in (self.stage, self.download):
            path.symlink_to(outside, target_is_directory=True)
            self.assertNotEqual(self.prepare(), 0)
            self.assertEqual(outside.stat().st_uid, 1000)
            self.assertEqual(stat.S_IMODE(outside.stat().st_mode), 0o755)
            path.unlink()

    def test_unknown_owner_and_file_are_rejected(self):
        self.stage.mkdir()
        os.chown(self.stage, 2000, 2000)
        self.assertNotEqual(self.prepare(), 0)
        self.assertEqual(self.stage.stat().st_uid, 2000)
        self.stage.rmdir()
        self.stage.write_bytes(b"not a directory")
        self.assertNotEqual(self.prepare(), 0)
        self.assertEqual(self.stage.read_bytes(), b"not a directory")

    def test_unmounted_or_missing_storage_is_rejected(self):
        self.storage.rmdir()
        self.assertNotEqual(self.prepare(), 0)
        # The host root filesystem is not a separately mounted media volume.
        script = SCRIPT.replace("/huoshan", "/")
        self.assertNotEqual(subprocess.run(["sh", "-c", script], capture_output=True).returncode, 0)


if __name__ == "__main__":
    unittest.main()
