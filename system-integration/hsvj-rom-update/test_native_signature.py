"""Exercise the production native verifier without block devices or install calls."""
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile

helper = Path(sys.argv[1]).resolve()
resources = Path(sys.argv[2]).resolve()
with tempfile.TemporaryDirectory(prefix="hsvj-native-signature-") as name:
    directory = Path(name)
    original = (resources / "rom-signature-selftest.txt").read_bytes()
    (directory / "manifest.sig").write_bytes((resources / "rom-signature-selftest.sig").read_bytes())
    for data, expected in [(original, "invalid manifest JSON"), (b"X" + original[1:], "release signature rejected")]:
        (directory / "manifest.json").write_bytes(data)
        release_id = hashlib.sha256(data).hexdigest()
        result = subprocess.run([str(helper), "--verify-bundle", str(directory) + "/", release_id], capture_output=True, text=True)
        if result.returncode != 1 or expected not in result.stderr:
            raise AssertionError((expected, result.returncode, result.stdout, result.stderr))
    result = subprocess.run([str(helper), "--verify-bundle", str(directory) + "/", "../escape"], capture_output=True, text=True)
    if result.returncode != 1 or "invalid release ID" not in result.stderr:
        raise AssertionError(result)
print("Native verifier passed: platform signature, tampering and invalid release ID; no block device access")
