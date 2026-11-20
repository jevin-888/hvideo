#!/usr/bin/env python3
"""Exercise production LAN transfer and reboot restore in auto-cleaned temporary roots."""
import base64
import hashlib
import hmac
import json
import os
from pathlib import Path
import shutil
import socket
import sqlite3
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
SERIAL = "H202609171001"
MODEL = "H6_POR"
BOOT = Path("/proc/sys/kernel/random/boot_id")


def check(value, message="assertion failed"):
    if not value:
        raise AssertionError(message)


def run(args, **kwargs):
    return subprocess.run([str(arg) for arg in args], check=True, text=True, **kwargs)


HARNESS = r"""
#include "network/DeviceFileSync.h"
#include "network/SyncContract.h"
#include "utils/JsonUtils.h"
#include "utils/Sha256.h"
#include "core/LicenseVerify.h"
#include <iomanip>
#include <sstream>
#include "utils/Logger.h"
#include <iostream>
#include <memory>
#include <sys/statvfs.h>
#include <netdb.h>
namespace hsvj { void Logger::log(Level, const char*, const char*, ...) {} }
bool failSpace = false;
extern "C" int __real_statvfs(const char*, struct statvfs*);
extern "C" int __wrap_statvfs(const char* path, struct statvfs* data) {
  int rc = __real_statvfs(path, data);
  if (failSpace && rc == 0) data->f_bavail = 0;
  return rc;
}
extern "C" int __real_getaddrinfo(const char*, const char*, const struct addrinfo*, struct addrinfo**);
extern "C" int __wrap_getaddrinfo(const char* host, const char* port, const struct addrinfo* hints, struct addrinfo** data) {
  // Test-only private addresses map to the isolated local mock without changing host networking.
  const char* target = host && std::string(host).rfind("10.254.253.", 0) == 0 ? "127.0.0.1" : host;
  return __real_getaddrinfo(target, port, hints, data);
}
int main(int argc, char** argv) {
  if (argc != 2) return 2;
  std::unique_ptr<hsvj::DeviceDataGuard> guard;
  std::string line;
  while (std::getline(std::cin, line)) {
    Json::Value req, result(Json::objectValue), out(Json::objectValue); std::string error;
    bool ok = hsvj::JsonUtils::parseJson(line, req, error);
    failSpace = req.get("test_space_fail", false).asBool();
    try {
      const std::string op = req.get("harness", "receive").asString();
      if (op == "send") ok = hsvj::sendDeviceFiles(argv[1], req["dirs"], req["target_ips"],
          req["target_serials"], "H6_POR", result, error);
      else if (op == "hash") {
        hsvj::Sha256Ctx context; hsvj::sha256Init(&context);
        const std::string bytes = req["bytes"].asString();
        size_t step = req.get("step", 17).asUInt();
        for (size_t i = 0; i < bytes.size(); i += step)
          hsvj::sha256Update(&context, reinterpret_cast<const unsigned char*>(bytes.data() + i), std::min(step, bytes.size() - i));
        unsigned char hash[32]; hsvj::sha256Final(&context, hash);
        std::ostringstream hex;
        for (unsigned char value : hash) hex << std::hex << std::setw(2) << std::setfill('0') << unsigned(value);
        result["sha256"] = hex.str(); ok = true;
      } else if (op == "license") {
        ok = hsvj::verifyLicenseSignature(req["payload"], req["key"].asString());
      } else if (op == "status") { result = hsvj::getDeviceFileSyncStatus(argv[1]); ok = true; }
      else if (op == "guard") {
        guard.reset(new hsvj::DeviceDataGuard(argv[1], hsvj::DeviceDataGuard::Purpose::Backup, error));
        ok = guard->acquired();
      } else if (op == "guard-pull") {
        guard.reset(new hsvj::DeviceDataGuard(argv[1], hsvj::DeviceDataGuard::Purpose::Pull, error, "192.168.1.10"));
        ok = guard->acquired();
      } else if (op == "release") { guard.reset(); ok = true; }
      else ok = hsvj::receiveDeviceFiles(argv[1], req, req.get("test_peer", "192.168.1.10").asString(),
                                        "H202609171001", "H6_POR", result, error);
    } catch (const std::exception& failure) { ok = false; error = failure.what(); }
    out["ok"] = ok; out["data"] = result; out["error"] = error;
    std::cout << hsvj::JsonUtils::toString(out) << std::endl;
  }
}
"""
JAVA = r"""
package com.hsvj.engine.util;
import java.nio.file.*;
import java.io.*;
public class SyncRestoreTest {
  public static void main(String[] args) throws Exception {
    Path root = Paths.get(args[0]);
    int stop = Integer.parseInt(args[1]);
    String mode = args[2];
    int[] count = {0};
    DeviceSyncRestore.Move move = (from, to) -> {
      Files.move(from, to, StandardCopyOption.ATOMIC_MOVE);
      if (++count[0] == stop) {
        if (mode.equals("crash")) throw new AssertionError("simulated process loss");
        if (mode.equals("io")) throw new IOException("simulated disk failure");
      }
    };
    System.out.println(DeviceSyncRestore.applyPending(root, move));
  }
}
"""


class Native:
    def __init__(self, executable, root):
        self.process = subprocess.Popen([str(executable), str(root)], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.lock = threading.Lock()

    def call(self, request):
        with self.lock:
            self.process.stdin.write(json.dumps(request) + "\n")
            self.process.stdin.flush()
            line = self.process.stdout.readline()
            while line and line.startswith("[WARN]"):
                line = self.process.stdout.readline()
            if not line:
                raise AssertionError("native exited: " + self.process.stderr.read())
            return json.loads(line)

    def close(self):
        if self.process.poll() is None:
            self.process.stdin.close()
            self.process.wait(timeout=10)
        check(self.process.returncode == 0, "native process failed")
        self.process.stdout.close()
        self.process.stderr.close()


def request(operation, transfer="a" * 32, **values):
    return dict(protocol=2, base_path="current", transfer_id=transfer,
                expected_serial=SERIAL, expected_model=MODEL, operation=operation, **values)


def manifest(files, folders=("config",)):
    return {"files": [{"path": name, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
                      for name, data in files.items()], "directories": list(folders)}


def chunk(index, data, offset=0, transfer="a" * 32):
    return request("chunk", transfer, file_index=index, offset=offset,
                   content_base64=base64.b64encode(data).decode(), sha256=hashlib.sha256(data).hexdigest())


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data if isinstance(data, bytes) else data.encode())


def hash_checks(binary, temp):
    root = temp / "hash"; root.mkdir(); native = Native(binary, root)
    try:
        for size in [0, 1, 3, 55, 56, 63, 64, 65, 119, 120, 127, 128, 129, 1000000]:
            value = ("abcxyz" * (size // 6 + 1))[:size]
            expected = hashlib.sha256(value.encode()).hexdigest()
            for step in [1,17,64,100000]:
                result = native.call(dict(harness="hash", bytes=value, step=step))
                check(result["data"]["sha256"] == expected, (size,step,result))
        for key in ["synthetic-test-key", "K"*131]:
            for payload in [{"version":2,"test":"hello"}, {"version":2,"name":"火山","array":[1,True,None]}]:
                canonical = json.dumps(payload,sort_keys=True,separators=(",",":"),ensure_ascii=True)
                signed = dict(payload, signature=hmac.new(key.encode(),canonical.encode(),hashlib.sha256).hexdigest())
                check(native.call(dict(harness="license",key=key,payload=signed))["ok"], "license HMAC changed")
                signed["version"] = 3
                check(not native.call(dict(harness="license",key=key,payload=signed))["ok"], "tampered license accepted")
    finally: native.close()
    print("PASS shared SHA-256: 56 streaming/boundary vectors against Python hashlib; 8 license HMAC/signature cases", flush=True)


def receiver_checks(binary, temp):
    root = temp / "receive"; root.mkdir()
    write(root / "config/a", "old")
    receiver = Native(binary, root)
    try:
        files = {"config/a": b"new", "config/empty": b""}
        begin = request("begin", manifest=manifest(files, ["config", "config/empty-dir"]))
        check(receiver.call(dict(begin, expected_serial="WRONG"))["ok"] is False, "identity mismatch accepted")
        check(receiver.call(dict(begin, protocol=1))["ok"] is False, "legacy protocol accepted")
        check(receiver.call(dict(begin, test_peer="8.8.8.8"))["ok"] is False, "non-LAN accepted")
        check(receiver.call(dict(begin, test_peer="192.168.001.10"))["ok"] is False, "ambiguous IP accepted")
        check(receiver.call(dict(begin, test_space_fail=True))["ok"] is False, "full disk accepted")
        check(receiver.call({"harness": "guard"})["ok"])
        check(receiver.call(begin)["ok"] is False, "backup/sync race accepted")
        check(receiver.call({"harness": "release"})["ok"])
        for name in ["../outside", "config/../../outside", "config\\escape", "license/license.dat", "config/a/x"]:
            bad = request("begin", manifest=manifest({name: b"new"}))
            check(receiver.call(bad)["ok"] is False, "bad path accepted: " + name)
        (root / "config/link").symlink_to(temp)
        check(receiver.call(request("begin", manifest=manifest({"config/link/out": b"bad"})))["ok"] is False)
        (root / "config/link").unlink()
        check(receiver.call(request("begin", manifest=manifest({"config/file": b"x", "config/file/x": b"y"})))["ok"] is False)
        check(receiver.call({"harness": "guard-pull"})["ok"])
        check(receiver.call(dict(begin, test_peer="192.168.1.11"))["ok"] is False, "unreserved pull peer accepted")
        check(receiver.call(begin)["ok"])
        check(receiver.call(begin)["ok"], "begin retry not idempotent")
        check(receiver.call(request("begin", "b" * 32, manifest=manifest(files)))["ok"] is False, "overlapping transfer accepted")
        check(receiver.call({"harness": "guard"})["ok"] is False, "backup started over active transfer")
        check(receiver.call(dict(chunk(0, b"new"), sha256="0" * 64))["ok"] is False)
        check(receiver.call(chunk(0, b"new", offset=1))["ok"] is False, "out-of-order chunk accepted")
        check(receiver.call(request("commit"))["ok"] is False, "incomplete transfer committed")
        good = chunk(0, b"new")
        check(receiver.call(good)["ok"])
        check(receiver.call(good)["ok"], "chunk retry not idempotent")
        check(receiver.call(chunk(0, b"bad"))["ok"] is False)
        check(receiver.call(chunk(1, b""))["ok"])
        check((root / "config/a").read_text() == "old", "live data changed while receiving")
        write(root / (".hsvj-sync-" + "a" * 32) / "stage/config/a", "bad")
        check(receiver.call(request("commit"))["ok"] is False, "final hash corruption accepted")
        write(root / (".hsvj-sync-" + "a" * 32) / "stage/config/a", "new")
        check(receiver.call(request("commit"))["ok"])
        check(receiver.call(request("commit"))["ok"], "commit retry not idempotent")
        check(receiver.call(request("abort"))["ok"] is False, "committed transfer aborted")
        check(receiver.call({"harness": "status"})["data"]["state"] == "pending_restart")
        check((root / "config/a").read_text() == "old", "live data changed before reboot")
    finally:
        receiver.close()
    # A process restart releases dead leases in the same boot.
    other = temp / "expired"; other.mkdir()
    first = Native(binary, other)
    check(first.call(request("begin", manifest=manifest({"config/a": b"x"})))["ok"])
    first.close()
    second = Native(binary, other)
    try:
        check(second.call(request("begin", "b" * 32, manifest=manifest({"config/a": b"y"})))["ok"])
        check(second.call(request("abort", "b" * 32))["ok"])
        check(second.call(request("abort", "b" * 32))["ok"])
    finally:
        second.close()
    print("PASS native receiver: identity, protocol, paths, symlinks, disk, concurrency, offsets, hashes, retries, staging, lease recovery", flush=True)


class Peer(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *_): pass

    def reply(self, status, data=None, error=None):
        raw = json.dumps({"ok": status == 200, "data": data,
                          "error": None if status == 200 else {"message": error}}).encode()
        self.send_response(status); self.send_header("Content-Length", str(len(raw)))
        self.send_header("Connection", "close"); self.end_headers()
        self.wfile.write(raw)

    def fixture(self):
        return self.server.fixtures[self.headers["Host"].split(":", 1)[0]] if hasattr(self.server, "fixtures") else self.server

    def do_GET(self):
        peer = self.fixture()
        if self.path != "/api/v1/system/discovery": return self.reply(404, error="bad route")
        self.reply(200, dict(app_id="com.hsvj.engine", model=MODEL, serial=peer.serial,
                             config_sync=dict(protocol=peer.protocol, state="idle")))

    def do_POST(self):
        if self.path != "/api/v1/sync/receive": return self.reply(404, error="bad route")
        req = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        op = req["operation"]
        peer = self.fixture()
        mode = peer.mode
        if mode == "interrupt" and op == "chunk": return self.reply(422, error="simulated disconnection")
        req["test_peer"] = "192.168.1.10"
        response = peer.receiver.call(req)
        if response["ok"] and op in ("begin", "chunk", "commit") and mode == "retry" and op not in peer.dropped:
            peer.dropped.add(op)
            self.connection.shutdown(socket.SHUT_RDWR); self.connection.close(); return
        if response["ok"] and op == "commit" and mode == "lost-commit":
            self.connection.shutdown(socket.SHUT_RDWR); self.connection.close(); return
        if response["ok"] and op == "chunk" and mode == "wrong-ack": response["data"]["offset"] += 1
        self.reply(200 if response["ok"] else 422, response["data"] if response["ok"] else None, response["error"])


def sender_checks(binary, temp, java_apply):
    source = temp / "source"; source.mkdir()
    remote_config = dict(networkStaticIp="192.168.1.10", networkIpMode="static", startupWirelessMode="hotspot",
                         onlineVodRoomId="source-room", syncGroup={"role": "leader"}, volume=42)
    write(source / "config/config.json", json.dumps(remote_config))
    write(source / "Image/中文二进制.dat", bytes(range(256)) * 17)
    (source / "Image/empty").mkdir()
    (source / "data").mkdir()
    database = sqlite3.connect(source / "data/songs.db")
    database.execute("PRAGMA journal_mode=WAL")
    database.execute("PRAGMA wal_autocheckpoint=0")
    database.execute("CREATE TABLE songs (id INTEGER PRIMARY KEY, name TEXT)")
    database.execute("INSERT INTO songs VALUES (1, 'committed only in WAL')"); database.commit()
    write(source / "video/large.bin", b"large-video" * (51 * 1024 * 1024 // 11 + 1))
    write(source / "config/export.tmp.part", "temporary")
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as route:
        route.connect(("8.8.8.8", 80)); ip = route.getsockname()[0]  # routing only; no datagram sent
    check(ip.startswith(("10.", "172.", "192.168.")), "test host needs a private LAN interface")
    server = ThreadingHTTPServer((ip, 8081), Peer)
    server.serial = SERIAL; server.protocol = 2; server.dropped = set()
    worker = threading.Thread(target=server.serve_forever, daemon=True); worker.start()
    try:
        for mode in ["wrong-serial", "legacy", "wrong-ack", "interrupt", "lost-commit", "retry"]:
            target = temp / mode; target.mkdir()
            local = dict(networkStaticIp="192.168.1.20", networkIpMode="static", startupWirelessMode="wifi",
                         onlineVodRoomId="target-room", syncGroup={"role": "follower"}, volume=3)
            write(target / "config/config.json", json.dumps(local))
            write(target / "data/songs.db-wal", "old WAL")
            write(target / "data/songs.db-shm", "old SHM")
            write(target / "license/license.dat", "keep license")
            write(target / "Music/keep", "unselected")
            write(target / "Image/extra", "target only")
            receiver = Native(binary, target); server.receiver = receiver
            server.mode = mode; server.dropped = set()
            server.serial = "OTHER" if mode == "wrong-serial" else SERIAL
            server.protocol = 1 if mode == "legacy" else 2
            sender = Native(binary, source)
            try:
                result = sender.call(dict(harness="send", dirs=["config", "Image", "data", "video"],
                                           target_ips=[ip], target_serials={ip: SERIAL}))
            finally:
                sender.close(); receiver.close()
            check(json.loads((target / "config/config.json").read_text()) == local, "sender changed live config")
            pending = (target / ".hsvj-sync-pending").exists()
            if mode == "retry":
                check(result["ok"], result)
                check(server.dropped == {"begin", "chunk", "commit"}, "retry cases not exercised")
                check(pending)
                # Same boot is safe even when the source receiver process has exited.
                java_apply(target)
                check(json.loads((target / "config/config.json").read_text()) == local)
                work = target / (".hsvj-sync-" + (target / ".hsvj-sync-pending").read_text())
                plan = json.loads((work / "plan.json").read_text()); plan["boot_id"] = "previous-boot"
                (work / "plan.json").write_text(json.dumps(plan))
                java_apply(target)
                applied = json.loads((target / "config/config.json").read_text())
                check(applied["volume"] == 42 and all(applied[key] == local[key] for key in local if key != "volume"))
                check((target / "video/large.bin").read_bytes() == (source / "video/large.bin").read_bytes(), ">50 MB file not exact")
                check((target / "Image/中文二进制.dat").read_bytes() == (source / "Image/中文二进制.dat").read_bytes())
                check((target / "Image/empty").is_dir())
                check((target / "Image/extra").read_text() == "target only")
                check((target / "Music/keep").read_text() == "unselected")
                check((target / "license/license.dat").read_text() == "keep license")
                check(not (target / "data/songs.db-wal").exists() and not (target / "data/songs.db-shm").exists())
                with sqlite3.connect(target / "data/songs.db") as imported:
                    check(imported.execute("SELECT name FROM songs").fetchone()[0] == "committed only in WAL")
                check(not (target / "config/export.tmp.part").exists())
                check(not any(target.glob(".hsvj-sync-*")) or len(list(target.glob(".hsvj-sync-*"))) == 1)
            elif mode == "lost-commit":
                check(not result["ok"] and pending)
                check(result["data"]["unconfirmed_count"] == 1 and "未确认" in result["error"], "lost commit falsely reported")
            else:
                check(not result["ok"] and not pending, (mode, result))
            check(not list(source.glob(".hsvj-sync-source-*")), "source snapshots leaked")
            print("PASS production HTTP sender:", mode, flush=True)
    finally:
        database.close(); server.shutdown(); worker.join(); server.server_close()



def multi_target_checks(binary, temp):
    source = temp / "multi-source"; source.mkdir()
    write(source / "Image/a", "exact multi-target bytes")
    server = ThreadingHTTPServer(("127.0.0.1", 8081), Peer)
    server.fixtures = {}; targets = {}
    for suffix, mode in [(1,"retry"), (2,"interrupt"), (3,"normal")]:
        ip = "10.254.253." + str(suffix)
        target = temp / ("multi-" + str(suffix)); target.mkdir(); targets[ip] = target
        write(target / "Image/a", "old")
        server.fixtures[ip] = SimpleNamespace(serial=SERIAL, protocol=2, mode=mode, dropped=set(), receiver=Native(binary,target))
    worker = threading.Thread(target=server.serve_forever, daemon=True); worker.start()
    sender = Native(binary, source)
    try:
        result = sender.call(dict(harness="send", dirs=["Image"], target_ips=list(targets),
                                  target_serials={ip:SERIAL for ip in targets}))
        check(not result["ok"] and result["data"]["synced_count"] == 2 and result["data"]["failed_count"] == 1, result)
        check([item["ok"] for item in result["data"]["details"]] == [True, False, True], result)
        check("10.254.253.2" in result["error"], "failed IP not visible in error message")
        for ip, target in targets.items():
            check((target / "Image/a").read_text() == "old", "live target modified")
            check((target / ".hsvj-sync-pending").exists() == (ip != "10.254.253.2"), "partial success mislabeled")
        check(not list(source.glob(".hsvj-sync-source-*")))
    finally:
        sender.close()
        for fixture in server.fixtures.values(): fixture.receiver.close()
        server.shutdown(); worker.join(); server.server_close()
    print("PASS multi-target: one failure preserves both successful pending transfers and reports exact failed IP", flush=True)


def java_checks(temp, java_apply):
    def setup(name):
        root = temp / name; root.mkdir()
        write(root / "config/config.json", '{"volume":3,"networkStaticIp":"target"}')
        write(root / "Image/a", "old image"); write(root / "data/a.db", "old database")
        write(root / "data/a.db-wal", "old WAL")
        data = {"config/config.json": b'{"volume":42,"networkStaticIp":"source"}',
                "Image/a": b"new image", "data/a.db": b"new database"}
        work = root / (".hsvj-sync-" + "c" * 32); (work / "stage").mkdir(parents=True)
        for path, value in data.items(): write(work / "stage" / path, value)
        plan = manifest(data, ["config", "Image", "data", "Image/empty"]); plan["boot_id"] = "previous-boot"
        write(work / "plan.json", json.dumps(plan)); write(root / ".hsvj-sync-pending", "c" * 32)
        return root, work

    # Interrupt at each real rename: all old files back up first, then all incoming files apply.
    for mode in ("crash", "io"):
        for stop in range(1, 8):
            root, work = setup(f"java-{mode}-{stop}")
            code = java_apply(root, stop, mode, expect_error=(mode == "crash"))
            if mode == "crash":
                check(code != 0, "process-loss hook did not execute")
                java_apply(root)
                check((root / "Image/a").read_text() == "new image")
                check(json.loads((root / "config/config.json").read_text()) == {"volume":42, "networkStaticIp":"target"})
                check(not (root / "data/a.db-wal").exists())
            else:
                check((root / "Image/a").read_text() == "old image")
                check((root / "data/a.db").read_text() == "old database")
                check((root / "data/a.db-wal").read_text() == "old WAL")
                check(json.loads((root / "config/config.json").read_text())["volume"] == 3)
            check(not (root / ".hsvj-sync-pending").exists() and not work.exists())
    root, work = setup("java-corrupt")
    write(work / "stage/Image/a", "corrupt")
    java_apply(root)
    check((root / "Image/a").read_text() == "old image" and not work.exists())
    root, work = setup("java-symlink")
    (work / "stage/Image/a").unlink(); (work / "stage/Image/a").symlink_to(root / "Image/a")
    java_apply(root)
    check((root / "Image/a").read_text() == "old image" and not work.exists())
    root, work = setup("java-committed")
    write(work / "COMMITTED", "")
    java_apply(root)
    check(not work.exists() and not (root / ".hsvj-sync-pending").exists())
    root, work = setup("java-cleanup-half")
    write(root / ".hsvj-sync-pending", "done:" + "c" * 32)
    shutil.rmtree(work / "stage"); (work / "plan.json").unlink()
    java_apply(root); check(not work.exists())
    root, work = setup("java-cleanup-gone")
    write(root / ".hsvj-sync-pending", "done:" + "c" * 32); shutil.rmtree(work)
    java_apply(root); check(not (root / ".hsvj-sync-pending").exists())
    # Unrecoverable mixed state must stop startup, retaining the rollback copy.
    root, work = setup("java-stop")
    write(work / "journal.json", '[{"path":"Image/a","incoming":true}]')
    (work / "stage/Image/a").unlink(); (root / "Image/a").unlink()
    check(java_apply(root, expect_error=True) != 0)
    check((root / ".hsvj-sync-pending").exists() and work.exists())
    root, work = setup("java-missing-backup")
    write(work / "journal.json", json.dumps([{"path":"Image/a","incoming":True,"original_exists":True,
          "sha256":hashlib.sha256(b"new image").hexdigest()}]))
    (root / "Image/a").unlink()
    check(java_apply(root, expect_error=True) != 0, "missing rollback file was silently accepted")
    root, work = setup("java-orphan")
    (root / ".hsvj-sync-pending").unlink()
    java_apply(root); check(not work.exists(), "dead orphan stage was not cleaned")
    print("PASS Java restore: 14 rename faults, config identity, WAL cleanup, raw-hash retry, corruption, symlinks, cleanup recovery, startup stop", flush=True)


def main():
    if os.name == "nt":
        linux_root = "/mnt/" + str(ROOT)[0].lower() + str(ROOT)[2:].replace("\\", "/")
        return subprocess.call(["wsl", "-d", "Ubuntu-24.04", "--", "python3",
                                linux_root + "/scripts/test_lan_config_sync.py"])
    with tempfile.TemporaryDirectory(prefix="hsvj-sync-test-") as temporary:
        temp = Path(temporary)
        (temp / "native.cpp").write_text(HARNESS)
        run(["gcc", "-O2", "-c", ROOT / "vendor/sqlite/sqlite3.c", "-o", temp / "sqlite.o"], capture_output=True)
        run(["g++", "-std=c++17", "-O1", "-Wno-deprecated-declarations", "-I" + str(ROOT / "include"),
             "-I" + str(ROOT / "vendor/jsoncpp/include"), "-I" + str(ROOT / "vendor/sqlite"),
             temp / "native.cpp", ROOT / "src/network/DeviceFileSync.cpp", ROOT / "src/utils/HttpClient.cpp",
             ROOT / "src/utils/JsonUtils.cpp", ROOT / "src/utils/Sha256.cpp", ROOT / "src/core/LicenseVerify.cpp",
             ROOT / "vendor/jsoncpp/src/lib_json/json_reader.cpp",
             ROOT / "vendor/jsoncpp/src/lib_json/json_value.cpp", ROOT / "vendor/jsoncpp/src/lib_json/json_writer.cpp",
             temp / "sqlite.o", "-lpthread", "-ldl", "-lm", "-Wl,--wrap=statvfs", "-Wl,--wrap=getaddrinfo", "-o", temp / "native"])
        print("PASS production native compilation", flush=True)
        jar_root = Path("/mnt/c/Users/T14S/.gradle/caches/modules-2/files-2.1/org.json/json")
        jar = next(jar_root.glob("*/*/*.jar"))
        (temp / "SyncRestoreTest.java").write_text(JAVA)
        run(["javac", "-encoding", "UTF-8", "-cp", jar, "-d", temp,
             ROOT / "app/src/main/java/com/hsvj/engine/util/ConfigBackupArchive.java",
             ROOT / "app/src/main/java/com/hsvj/engine/util/DeviceSyncRestore.java", temp / "SyncRestoreTest.java"])
        def java_apply(root, stop=0, mode="normal", expect_error=False):
            result = subprocess.run(["java", "-cp", str(temp) + ":" + str(jar),
                                     "com.hsvj.engine.util.SyncRestoreTest", str(root), str(stop), mode],
                                    capture_output=True, text=True, timeout=30)
            if not expect_error: check(result.returncode == 0, result.stderr)
            return result.returncode
        hash_checks(temp / "native", temp)
        receiver_checks(temp / "native", temp)
        java_checks(temp, java_apply)
        sender_checks(temp / "native", temp, java_apply)
        multi_target_checks(temp / "native", temp)
    print("PASS: temporary native/Java files, snapshots and test roots removed", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
