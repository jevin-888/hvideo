"""Verify transient capture gaps no longer trigger media-graph recovery."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "src/capture/V4L2Capture.cpp").read_text(encoding="utf-8")
assert "claimMipiShortGapRecovery" not in source
assert 'markMipiRecoveryRequired("frames resumed after >=500ms gap' not in source
assert "kMipiRecoveryTimeouts = 30" in source
print("PASS: media-graph recovery is reserved for source changes and the 3s no-frame watchdog")
