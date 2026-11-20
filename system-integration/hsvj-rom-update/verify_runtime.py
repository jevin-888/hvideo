#!/usr/bin/env python3
"""Exercise boot permission repair and verify the installed ART configuration."""
import os
from pathlib import Path
import shlex
import stat
import subprocess
import sys
import tempfile


def verify_permissions(script):
    text = script.read_text()
    start = text.index("if [ -d /huoshan ]; then\n")
    end = text.index("\nfi", start) + len("\nfi")
    block = text[start:end]
    with tempfile.TemporaryDirectory(prefix="hsvj-permissions-") as name:
        root = Path(name)
        storage = root / "huoshan"
        staging = storage / ".rom-update"
        staging.mkdir(parents=True, mode=0o700)
        state = staging / "status.json"
        state.write_text('{"phase":"installed"}\n')
        state.chmod(0o600)
        nested = staging / "nested"
        nested.mkdir(mode=0o700)
        nested_file = nested / "partition.tmp"
        nested_file.write_bytes(b"protected")
        nested_file.chmod(0o600)
        media = storage / "media with spaces"
        media.mkdir(mode=0o700)
        song = media / "song.dat"
        song.write_bytes(b"media")
        song.chmod(0o600)
        download = storage / ".rom-download"
        download.mkdir(mode=0o700)
        outside = root / "outside"
        outside.mkdir(mode=0o700)
        (storage / "link").symlink_to(outside, target_is_directory=True)
        protected = [staging, state, nested, nested_file, outside]
        before = [(p.stat().st_mode, p.stat().st_uid, p.stat().st_gid) for p in protected]
        calls = root / "calls"
        mocks = root / "bin"
        mocks.mkdir()
        for command in ("chown", "restorecon"):
            executable = mocks / command
            executable.write_text('#!/bin/sh\nprintf "%s\\n" "$@" >> "$HSVJ_TEST_CALLS"\n')
            executable.chmod(0o755)
        env = dict(os.environ, PATH=str(mocks) + ":" + os.environ["PATH"], HSVJ_TEST_CALLS=str(calls))
        command = "logi() { :; }\n" + block.replace("/huoshan", shlex.quote(str(storage)))
        # Two passes model first boot and a later restart of the same service.
        for _ in range(2):
            subprocess.run(["sh", "-eu", "-c", command], check=True, env=env)
        after = [(p.stat().st_mode, p.stat().st_uid, p.stat().st_gid) for p in protected]
        if after != before or state.read_text() != '{"phase":"installed"}\n' or nested_file.read_bytes() != b"protected":
            raise ValueError("boot repair changed protected ROM staging or a symlink target")
        for path, mode in ((storage, 0o777), (media, 0o777), (song, 0o666), (download, 0o777)):
            if stat.S_IMODE(path.stat().st_mode) != mode:
                raise ValueError("media permission repair failed: " + str(path))
        arguments = calls.read_text().splitlines()
        if any(str(staging) in value or str(outside) in value or value in ("-R", "-RF") for value in arguments):
            raise ValueError("ownership or labeling traverses protected storage")
        if str(song) not in arguments or str(media) not in arguments:
            raise ValueError("media ownership/label repair was skipped")


def verify(source):
    product = source / "out-hsvj-user/target/product/h66_por"
    original = source / "vendor/chuangwei-minimal-perf/hsvj_perf_tune.sh"
    installed = product / "vendor/bin/hsvj_perf_tune.sh"
    if original.read_bytes() != installed.read_bytes():
        raise ValueError("installed boot repair differs from source")
    verify_permissions(installed)
    properties = {}
    for relative in ("system/etc/prop.default", "system/build.prop", "system_ext/build.prop", "vendor/build.prop", "odm/etc/build.prop", "product/build.prop"):
        path = product / relative
        if not path.exists():
            continue
        for line in path.read_text().splitlines():
            if line.strip() and not line.lstrip().startswith("#") and "=" in line:
                key, value = line.split("=", 1)
                if key.strip() == "dalvik.vm.dex2oat64.enabled" and value.strip() != "true":
                    raise ValueError("conflicting dex2oat64 property: " + relative)
                properties[key.strip()] = value.strip()
    if properties.get("dalvik.vm.dex2oat64.enabled") != "true":
        raise ValueError("ROM must enable its 64-bit dex2oat before package manager starts")
    compiler = product / "system/apex/com.android.art.release/bin/dex2oat64"
    if not compiler.is_file() or not os.access(compiler, os.X_OK):
        raise ValueError("ART dex2oat64 executable missing")
    with compiler.open("rb") as stream:
        header = stream.read(20)
    if header[:6] != b"\x7fELF\x02\x01" or header[18:20] != b"\xb7\x00":
        raise ValueError("ART dex2oat64 must be an AArch64 ELF executable")
    print("Runtime verified: protected ROM staging survives boot repair; AArch64 dex2oat64 installed and enabled")


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--permissions-only":
        verify_permissions(Path(sys.argv[2]))
        print("Boot permission regression passed")
    else:
        verify(Path(sys.argv[1]).resolve())
