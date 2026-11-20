"""Read/write RK628 receiver EDID through its existing V4L2 subdevice ioctl.

Builds a temporary Android helper, uses a unique device directory, then removes it.
Only an explicit --write changes EDID; reads do not change capture configuration.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import uuid


SOURCE = r'''
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <linux/v4l2-subdev.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    int fd = open("/dev/v4l-subdev2", argc == 3 ? O_RDWR : O_RDONLY);
    if (fd < 0) { perror("open subdev"); return 3; }
    unsigned char data[4096] = {0};
    struct v4l2_edid edid = {0};
    edid.edid = data;
    if (argc == 3 && strcmp(argv[1], "write") == 0) {
        FILE *f = fopen(argv[2], "rb");
        if (!f) { perror("open EDID"); close(fd); return 4; }
        size_t len = fread(data, 1, sizeof(data), f);
        int extra = fgetc(f);
        fclose(f);
        if (!len || len % 128 || extra != EOF) { close(fd); return 5; }
        edid.blocks = len / 128;
        if (ioctl(fd, VIDIOC_S_EDID, &edid) < 0) {
            perror("VIDIOC_S_EDID"); close(fd); return 6;
        }
        fprintf(stderr, "VIDIOC_S_EDID accepted %u blocks\n", edid.blocks);
    }
    memset(data, 0, sizeof(data));
    memset(&edid, 0, sizeof(edid));
    edid.edid = data;
    edid.blocks = sizeof(data) / 128;
    if (ioctl(fd, VIDIOC_G_EDID, &edid) < 0) {
        perror("VIDIOC_G_EDID"); close(fd); return 7;
    }
    if (edid.blocks > sizeof(data) / 128) { close(fd); return 8; }
    printf("{\"blocks\":%u,\"edid_hex\":\"", edid.blocks);
    for (unsigned i = 0; i < edid.blocks * 128; ++i) printf("%02x", data[i]);
    printf("\"}\n");
    close(fd);
    return 0;
}
'''


def validate(data):
    if len(data) < 128 or len(data) % 128:
        raise ValueError("EDID must contain complete 128-byte blocks")
    if data[:8] != bytes.fromhex("00ffffffffffff00"):
        raise ValueError("Invalid EDID header")
    if data[126] + 1 != len(data) // 128:
        raise ValueError("EDID extension count does not match data length")
    for offset in range(0, len(data), 128):
        if sum(data[offset:offset + 128]) % 256:
            raise ValueError(f"Invalid EDID checksum in block {offset // 128}")


def run(args, **kwargs):
    return subprocess.run(args, check=True, capture_output=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--write", type=Path)
    args = parser.parse_args()
    write_data = args.write.read_bytes() if args.write else None
    if write_data is not None:
        validate(write_data)
    sdk = Path(os.environ.get("ANDROID_SDK_ROOT") or os.environ["ANDROID_HOME"])
    compiler = sdk / "ndk/26.1.10909125/toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe"
    adb = ["adb", "-s", args.serial]
    remote = "/data/local/tmp/hsvj-edid-" + uuid.uuid4().hex
    with tempfile.TemporaryDirectory(prefix="hsvj-rk628-edid-") as temporary:
        temporary = Path(temporary)
        source, binary = temporary / "probe.c", temporary / "probe"
        source.write_text(SOURCE, encoding="utf-8")
        run([str(compiler), "--target=aarch64-linux-android27", "-Wall", "-Wextra", "-Werror", "-O2", str(source), "-o", str(binary)])
        run(adb + ["shell", "mkdir", remote])
        try:
            run(adb + ["push", str(binary), remote + "/probe"])
            run(adb + ["shell", "chmod", "700", remote + "/probe"])
            if args.write:
                staged = temporary / "edid.bin"
                staged.write_bytes(write_data)
                run(adb + ["push", str(staged), remote + "/edid.bin"])
                result = run(adb + ["shell", remote + "/probe", "write", remote + "/edid.bin"])
            else:
                result = run(adb + ["shell", remote + "/probe", "read"])
            readback = bytes.fromhex(json.loads(result.stdout)["edid_hex"])
            validate(readback)
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_bytes(readback)
            if write_data is not None and readback != write_data:
                raise RuntimeError("EDID write readback mismatch; actual data saved to output")
            print(json.dumps({"bytes": len(readback), "checksums_valid": True, "written": bool(args.write), "output": str(args.output)}))
            if result.stderr:
                print(result.stderr.decode(errors="replace"))
        finally:
            run(adb + ["shell", "rm", "-f", remote + "/probe", remote + "/edid.bin"])
            run(adb + ["shell", "rmdir", remote])


if __name__ == "__main__":
    main()
