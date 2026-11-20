#!/usr/bin/env python3
"""Read-only audit of the delivered ROM-system HW82 APK and production sync assets."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SDK = Path.home() / "AppData/Local/Android/Sdk"
TOOLS = SDK / "build-tools/34.0.0"


def main():
    apk = Path(sys.argv[1]).resolve()
    output = subprocess.check_output([str(TOOLS / "aapt.exe"), "dump", "badging", str(apk)], text=True, encoding="utf-8")
    package = re.search(r"package: name='([^']+)' versionCode='([^']+)' versionName='([^']+)'", output)
    if not package: raise AssertionError("APK package metadata missing")
    app_id, code, version = package.groups()
    assert app_id == "com.hsvj.engine"
    assert apk.name == "hsvj-engine-" + version + "-hw82.apk", "filename/version mismatch"
    audit = dict(apk=str(apk), version=version, code=int(code), sha256=hashlib.sha256(apk.read_bytes()).hexdigest(), size=apk.stat().st_size)
    signature = subprocess.check_output([str(TOOLS / "apksigner.bat"), "verify", "--verbose", "--print-certs", str(apk)], text=True, encoding="utf-8")
    cert = re.search(r"Signer #1 certificate SHA-256 digest: ([0-9a-f]+)", signature)
    assert cert and cert.group(1) == "c8a2e9bccf597c2fb6dc66bee293fc13f2fc47ec77bc6b2b0d52c11f51192ab8", "platform signer mismatch"
    audit["certificate_sha256"] = cert.group(1)
    with zipfile.ZipFile(apk) as archive:
        paths = archive.namelist()
        assets = {}
        for relative in ["index.html", "style.css", "script.js", "modules/core/navigation.js",
                         "modules/pages/systemSettings.js", "mobile/script.js", "mobile/modules/pages/mobileControl.js"]:
            path = "assets/web/" + relative
            data = archive.read(path)
            assert data == (ROOT / "src/network/web" / relative).read_bytes(), "asset differs: " + relative
            assets[path] = hashlib.sha256(data).hexdigest()
        dex = b"".join(archive.read(name) for name in paths if re.fullmatch(r"classes\d*\.dex", name))
        assert b"Lcom/hsvj/engine/util/DeviceSyncRestore;" in dex, "reboot restore class absent"
        native = archive.read("lib/arm64-v8a/libhsvj_engine.so")
        for marker in [b".hsvj-sync-pending", b"expected_serial", b"expected_model", b"pending_restart", b"unconfirmed_count"]:
            assert marker in native, "protocol field absent: " + marker.decode()
        codec = [path for path in paths if path.startswith("lib/") and Path(path).name in
                 {"libmpp.so", "libavcodec.so", "libavformat.so", "libavutil.so", "libswscale.so", "libswresample.so"}]
        assert not codec, "ROM codecs duplicated"
        shaders = [path for path in paths if path.startswith("assets/shaders/") and path.endswith(".spv")]
        assert len(shaders) == 31, "shader count mismatch"
        audit.update(assets=assets, restore_class=True, native_protocol=True, codec_libraries=codec, shaders=len(shaders))
    record = ROOT / "records/20261013-lan-sync-audit/apk-audit.json"
    record.write_text(json.dumps(audit, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key:value for key,value in audit.items() if key != "assets"},ensure_ascii=False,indent=2))
    return 0


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    sys.exit(main())
