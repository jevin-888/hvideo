// Internal ROM protocol 1. Only named, allowlisted partitions are writable.
#include <android-base/properties.h>
#include <json/json.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <sparse/sparse.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <linux/fs.h>
#include <fcntl.h>
#include <dirent.h>
#include <limits.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "trusted_key.h"
#include "version_policy.h"

namespace {
const char* kRoot = "/huoshan/.rom-update/";
constexpr const char* kHardware = "universal";
constexpr uint64_t kMaxImage = 4ULL * 1024 * 1024 * 1024;
struct Partition { const char* name; uint64_t start; uint64_t sectors; };
// Must match the existing production GPT. No repartitioning or loader/IDB writes.
const Partition kPartitions[] = {
    {"trust", 0x6000, 0x2000}, {"uboot", 0x4000, 0x2000},
    {"dtbo", 0xa000, 0x2000}, {"vbmeta", 0xc000, 0x800},
    {"super", 0x1db000, 0x614000}, {"userdata", 0x7ef000, 0x448000},
    {"boot", 0xc800, 0x14000}, {"recovery", 0x20800, 0x30000},
};
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
struct Fd {
    int n;
    explicit Fd(int value) : n(value) { require(n >= 0, std::string("open: ") + strerror(errno)); }
    ~Fd() { close(n); }
    Fd(const Fd&) = delete;
};
void write_all(int fd, const void* data, size_t size) {
    auto p = static_cast<const char*>(data);
    while (size) { ssize_t n = write(fd, p, size); if (n < 0 && errno == EINTR) continue;
        require(n > 0, "write failed"); p += n; size -= n; }
}
std::string read_file(const std::string& path, size_t limit) {
    Fd fd(open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    struct stat st{}; require(fstat(fd.n, &st) == 0 && S_ISREG(st.st_mode), "file type rejected: " + path);
    std::string s; std::array<char, 4096> buffer{};
    for (;;) {
        ssize_t n = read(fd.n, buffer.data(), buffer.size());
        if (n < 0 && errno == EINTR) continue;
        require(n >= 0, "read failed");
        if (n == 0) break;
        require(s.size() + n <= limit, "file too large: " + path);
        s.append(buffer.data(), n);
    }
    return s;
}
std::string hex(const unsigned char* p, size_t n) {
    static const char* digits = "0123456789abcdef"; std::string out;
    for (size_t i = 0; i < n; ++i) { out += digits[p[i] >> 4]; out += digits[p[i] & 15]; } return out;
}
std::string hash_region(int fd, uint64_t offset, uint64_t count) {
    SHA256_CTX ctx; SHA256_Init(&ctx); std::array<char, 65536> buf{};
    while (count) { ssize_t n = pread(fd, buf.data(), std::min<uint64_t>(count, buf.size()), offset);
        if (n < 0 && errno == EINTR) continue;
        require(n > 0, "hash read failed"); SHA256_Update(&ctx, buf.data(), n); offset += n; count -= n; }
    unsigned char digest[SHA256_DIGEST_LENGTH]; SHA256_Final(digest, &ctx); return hex(digest, sizeof(digest));
}
std::string hash_text(const std::string& text) { unsigned char digest[32]; SHA256(reinterpret_cast<const unsigned char*>(text.data()), text.size(), digest); return hex(digest, 32); }
bool hash_valid(const std::string& s) { return s.size() == 64 && s.find_first_not_of("0123456789abcdef") == std::string::npos; }
Json::Value parse(const std::string& text) {
    Json::Reader reader(Json::Features::strictMode()); Json::Value v;
    require(reader.parse(text, v, false), "invalid manifest JSON");
    // Canonical encoding also rejects duplicate keys and ignored trailing bytes
    // in the older platform JsonCpp parser.
    require(Json::FastWriter().write(v) == text, "noncanonical JSON");
    return v;
}
std::string json(const Json::Value& v) { return Json::FastWriter().write(v); }
void atomic_json(const std::string& name, const Json::Value& v) {
    std::string path = std::string(kRoot) + name;
    Fd fd(open((path + ".tmp").c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600));
    auto content = json(v); write_all(fd.n, content.data(), content.size()); require(fsync(fd.n) == 0, "fsync state failed");
    require(rename((path + ".tmp").c_str(), path.c_str()) == 0, "rename state failed");
    Fd dir(open(kRoot, O_RDONLY | O_DIRECTORY | O_CLOEXEC)); require(fsync(dir.n) == 0, "fsync directory failed");
}
bool mounted(const std::string& target) {
    std::ifstream in("/proc/mounts"); std::string source, mountpoint, rest;
    while (in >> source >> mountpoint) { std::getline(in, rest); if (mountpoint == target) return true; } return false;
}
void storage(bool recovery) {
    if (!mounted("/huoshan")) {
        require(recovery, "/huoshan must already be mounted");
        mkdir("/huoshan", 0755);
        require(mount("/dev/block/by-name/huoshan", "/huoshan", "ext4", MS_NOATIME | MS_NODEV | MS_NOSUID, nullptr) == 0,
                "cannot mount huoshan; refusing to format");
    }
    struct stat a{}, b{};
    require(stat("/huoshan", &a) == 0 && stat("/dev/block/by-name/huoshan", &b) == 0 && a.st_dev == b.st_rdev, "wrong huoshan mount source");
    require(mkdir(kRoot, 0700) == 0 || errno == EEXIST, "create staging directory failed");
    struct stat root{}; require(lstat("/huoshan/.rom-update", &root) == 0 && S_ISDIR(root.st_mode) && root.st_uid == 0, "staging ownership invalid");
    require(chmod(kRoot, 0700) == 0, "staging permissions failed");
}
std::string block_name(const std::string& name) {
    char resolved[4096]; require(realpath(("/dev/block/by-name/" + name).c_str(), resolved), "missing partition: " + name);
    std::string path(resolved); return path.substr(path.find_last_of('/') + 1);
}
uint64_t sys_number(const std::string& name, const char* field) {
    auto text = read_file("/sys/class/block/" + block_name(name) + "/" + field, 128);
    return std::stoull(text);
}
void layout() {
    std::set<std::string> devices;
    for (const auto& p : kPartitions) {
        auto name = block_name(p.name);
        require(name.find("mmcblk2p") == 0 && devices.insert(name).second, "unexpected storage device");
        require(sys_number(p.name, "start") == p.start && sys_number(p.name, "size") == p.sectors, "partition layout mismatch: " + std::string(p.name));
    }
    require(sys_number("huoshan", "start") == 0xc37000, "huoshan offset mismatch");
    require(block_name("huoshan").find("mmcblk2p") == 0, "unexpected huoshan device");
    require(sys_number("misc", "start") == 0x8000 && sys_number("misc", "size") == 0x2000 && block_name("misc").find("mmcblk2p") == 0, "misc layout mismatch");
}
void targets_idle() {
    for (const auto& p : kPartitions) {
        auto name = block_name(p.name);
        std::string dir = "/sys/class/block/" + name + "/holders";
        std::unique_ptr<DIR, decltype(&closedir)> holders(opendir(dir.c_str()), closedir);
        require(holders != nullptr, "cannot inspect block holders");
        while (auto e = readdir(holders.get())) require(e->d_name[0] == '.', "target has active block mappings: " + name);
        struct stat target{}; require(stat(("/dev/block/by-name/" + std::string(p.name)).c_str(), &target) == 0, "target missing");
        std::ifstream in("/proc/mounts"); std::string source, point, rest;
        while (in >> source >> point) {
            std::getline(in, rest); struct stat st{};
            require(stat(point.c_str(), &st) != 0 || st.st_dev != target.st_rdev, "target is mounted: " + point);
        }
    }
}
Json::Value manifest(const std::string& id) {
    require(hash_valid(id), "invalid release ID");
    auto text = read_file(std::string(kRoot) + "manifest.json", 65536);
    require(hash_text(text) == id, "manifest ID mismatch");
    auto signature = read_file(std::string(kRoot) + "manifest.sig", 1024);
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new_mem_buf(kTrustedPublicKey, -1), BIO_free);
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr), EVP_PKEY_free);
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    require(key && ctx && EVP_DigestVerifyInit(ctx.get(), nullptr, EVP_sha256(), nullptr, key.get()) == 1 &&
            EVP_DigestVerifyUpdate(ctx.get(), text.data(), text.size()) == 1 &&
            EVP_DigestVerifyFinal(ctx.get(), reinterpret_cast<const unsigned char*>(signature.data()), signature.size()) == 1, "release signature rejected");
    auto v = parse(text);
    const std::set<std::string> fields = {"protocol", "product", "hardware", "version_code", "target_fingerprint", "image_size", "image_sha256", "partitions"};
    require(v.isObject() && v.getMemberNames().size() == fields.size(), "manifest fields rejected");
    for (const auto& field : v.getMemberNames()) require(fields.count(field), "unknown manifest field");
    // Protocol 1 retains hardware as descriptive metadata for old clients.
    // Product, signature and the physical partition layout define compatibility.
    require(v["protocol"].asInt() == 1 && v["product"].asString() == "H6_POR", "unsupported release target");
    require(v["hardware"].isString(), "invalid hardware metadata");
    require(v["version_code"].isInt() && v["version_code"].asInt() > 0, "invalid ROM version");
    auto fingerprint = v["target_fingerprint"].asString();
    require(fingerprint.find("CHUANGWEI/H6_POR/H6_POR:11/") == 0 && fingerprint.size() < 256, "invalid target fingerprint");
    require(v["image_size"].isUInt64() && v["image_size"].asUInt64() > 1024 && v["image_size"].asUInt64() <= kMaxImage && hash_valid(v["image_sha256"].asString()), "invalid firmware size/hash");
    require(v["partitions"].isArray() && v["partitions"].size() == 8, "expected eight writable partitions");
    std::set<std::string> names; std::vector<std::pair<uint64_t, uint64_t>> ranges;
    for (const auto& entry : v["partitions"]) {
        require(entry.isObject() && entry.size() == 6 && entry["name"].isString() && entry["offset"].isUInt64() && entry["size"].isUInt64() && entry["expanded_size"].isUInt64() && entry["sha256"].isString() && entry["expanded_sha256"].isString(), "invalid partition fields");
        auto name = entry["name"].asString(); require(names.insert(name).second, "duplicate partition");
        const Partition* part = nullptr; for (const auto& p : kPartitions) if (name == p.name) part = &p;
        require(part, "protected/unknown partition: " + name);
        uint64_t offset = entry["offset"].asUInt64(), length = entry["size"].asUInt64(), expanded = entry["expanded_size"].asUInt64();
        require(length > 0 && offset <= v["image_size"].asUInt64() && length <= v["image_size"].asUInt64() - offset && expanded > 0 && expanded <= part->sectors * 512, "partition bounds rejected");
        require(hash_valid(entry["sha256"].asString()) && hash_valid(entry["expanded_sha256"].asString()), "invalid partition hashes");
        for (const auto& range : ranges) require(offset + length <= range.first || offset >= range.second, "overlapping image entries");
        ranges.emplace_back(offset, offset + length);
    }
    return v;
}
void check_image(const Json::Value& v, int fd) {
    struct stat st{}; require(fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && static_cast<uint64_t>(st.st_size) == v["image_size"].asUInt64(), "firmware size mismatch");
    require(hash_region(fd, 0, st.st_size) == v["image_sha256"].asString(), "firmware SHA256 mismatch");
    for (const auto& e : v["partitions"]) require(hash_region(fd, e["offset"].asUInt64(), e["size"].asUInt64()) == e["sha256"].asString(), "partition SHA256 mismatch");
}
void bcb(const std::string& id) {
    require(sys_number("misc", "start") == 0x8000 && sys_number("misc", "size") == 0x2000 && block_name("misc").find("mmcblk2p") == 0, "refusing BCB write on unexpected storage");
    Fd fd(open("/dev/block/by-name/misc", O_RDWR | O_CLOEXEC));
    std::array<char, 2048> data{};
    if (!id.empty()) { memcpy(data.data(), "boot-recovery", 13); auto command = "recovery\n--hsvj_update=" + id + "\n"; memcpy(data.data() + 64, command.data(), command.size()); }
    require(pwrite(fd.n, data.data(), data.size(), 0) == static_cast<ssize_t>(data.size()) && fsync(fd.n) == 0, "BCB write failed");
}
Json::Value status(const std::string& id, const Json::Value& m, const char* phase, const std::string& message) {
    Json::Value v; v["release_id"] = id; v["version_code"] = m["version_code"]; v["target_fingerprint"] = m["target_fingerprint"];
    v["boot_id"] = read_file("/proc/sys/kernel/random/boot_id", 128);
    v["phase"] = phase; v["message"] = message; atomic_json("status.json", v);
    std::cerr << "HSVJ ROM " << phase << ": " << message << std::endl;
    return v;
}
struct Output { int fd; uint64_t count = 0, limit; SHA256_CTX hash; bool failed = false; };
int sparse_output(void* opaque, const void* data, size_t count) {
    auto& out = *static_cast<Output*>(opaque); static const std::array<char, 65536> zeros{};
    if (count > out.limit - out.count) { out.failed = true; return -1; }
    const char* p = static_cast<const char*>(data);
    try {
        while (count) { size_t n = std::min(count, zeros.size()); const void* bytes = p ? p : zeros.data();
            if (out.fd >= 0) write_all(out.fd, bytes, n);
            SHA256_Update(&out.hash, bytes, n); out.count += n; count -= n; if (p) p += n; }
        return 0;
    } catch (...) { out.failed = true; return -1; }
}
std::string expanded_copy(int source, int destination, uint64_t length) {
    require(lseek(source, 0, SEEK_SET) == 0, "partition seek failed");
    unsigned char magic[4]; require(pread(source, magic, 4, 0) == 4, "partition header missing");
    Output out{destination, 0, length, {}, false}; SHA256_Init(&out.hash);
    if (memcmp(magic, "\x3a\xff\x26\xed", 4) == 0) {
        std::unique_ptr<sparse_file, decltype(&sparse_file_destroy)> sparse(sparse_file_import(source, false, false), sparse_file_destroy);
        require(sparse && sparse_file_len(sparse.get(), false, false) == static_cast<int64_t>(length), "sparse layout rejected");
        require(sparse_file_callback(sparse.get(), false, false, sparse_output, &out) == 0 && !out.failed, "sparse expansion failed");
    } else {
        std::array<char, 65536> buf{}; ssize_t n;
        while ((n = read(source, buf.data(), buf.size())) > 0) require(sparse_output(&out, buf.data(), n) == 0, "raw expansion failed");
        require(n == 0, "partition read failed");
    }
    require(out.count == length, "expanded length mismatch"); unsigned char digest[32]; SHA256_Final(digest, &out.hash); return hex(digest, 32);
}
void extract(int image, const Json::Value& e, int output) {
    uint64_t left = e["size"].asUInt64(), offset = e["offset"].asUInt64(); std::array<char, 65536> buf{};
    while (left) { ssize_t n = pread(image, buf.data(), std::min<uint64_t>(left, buf.size()), offset); require(n > 0, "extract failed"); write_all(output, buf.data(), n); offset += n; left -= n; }
    require(fsync(output) == 0, "partition staging sync failed");
}
int recover(const std::string& id) {
    Json::Value m;
    try {
        storage(true); layout(); targets_idle(); m = manifest(id);
        Fd lock(open((std::string(kRoot) + "lock").c_str(), O_CREAT | O_RDWR | O_NOFOLLOW, 0600)); require(flock(lock.n, LOCK_EX | LOCK_NB) == 0, "upgrade already running");
        auto armed = parse(read_file(std::string(kRoot) + "status.json", 65536));
        require(armed["release_id"] == id && armed["phase"] == "armed", "upgrade is not armed");
        Fd image(open((std::string(kRoot) + "update.img").c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)); check_image(m, image.n);
        // Validate every expanded image before the first partition write.
        std::string tmp = std::string(kRoot) + "partition.tmp";
        for (const auto& e : m["partitions"]) {
            Fd fd(open(tmp.c_str(), O_CREAT | O_TRUNC | O_RDWR | O_NOFOLLOW, 0600)); extract(image.n, e, fd.n);
            require(expanded_copy(fd.n, -1, e["expanded_size"].asUInt64()) == e["expanded_sha256"].asString(), "expanded image hash mismatch");
        }
        // A power failure must never cause an unattended reflash loop.
        bcb(""); status(id, m, "installing", "all firmware checks passed");
        for (const auto& p : kPartitions) {
            const Json::Value* entry = nullptr; for (const auto& e : m["partitions"]) if (e["name"].asString() == p.name) entry = &e;
            require(entry, "missing partition");
            targets_idle();
            Fd staged(open(tmp.c_str(), O_CREAT | O_TRUNC | O_RDWR | O_NOFOLLOW, 0600)); extract(image.n, *entry, staged.n);
            Fd device(open((std::string("/dev/block/by-name/") + p.name).c_str(), O_RDWR | O_CLOEXEC));
            struct stat st{}; uint64_t size = 0;
            require(fstat(device.n, &st) == 0 && S_ISBLK(st.st_mode) && ioctl(device.n, BLKGETSIZE64, &size) == 0 && size == p.sectors * 512, "target block device changed");
            auto expected = (*entry)["expanded_sha256"].asString(); auto count = (*entry)["expanded_size"].asUInt64();
            require(expanded_copy(staged.n, device.n, count) == expected && fsync(device.n) == 0, "partition write failed");
            require(hash_region(device.n, 0, count) == expected, "partition readback mismatch");
            status(id, m, "installing", std::string("verified ") + p.name);
        }
        unlink(tmp.c_str()); status(id, m, "installed", "awaiting Android boot verification"); sync();
        require(android::base::SetProperty("sys.powerctl", "reboot"), "reboot request failed"); return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        try { bcb(""); if (!m.isNull()) status(id, m, "failed", e.what()); unlink((std::string(kRoot) + "partition.tmp").c_str()); } catch (...) {}
        // Remain in recovery on failure. Never format a protected partition or report success.
        return 1;
    }
}
}

int main(int argc, char** argv) {
    try {
        // Same signature, range and sparse verifier on the build host; no block access.
        if (argc == 4 && std::string(argv[1]) == "--verify-bundle") {
            std::string directory(argv[2]);
            require(!directory.empty() && directory.front() == '/' && directory.back() == '/', "verification directory must be absolute with trailing slash");
            kRoot = directory.c_str();
            auto m = manifest(argv[3]);
            Fd image(open((directory + "update.img").c_str(), O_RDONLY | O_NOFOLLOW));
            check_image(m, image.n);
            for (const auto& e : m["partitions"]) {
                std::string pattern = directory + "verify-XXXXXX";
                Fd temporary(mkstemp(pattern.data())); unlink(pattern.c_str());
                extract(image.n, e, temporary.n);
                require(expanded_copy(temporary.n, -1, e["expanded_size"].asUInt64()) == e["expanded_sha256"].asString(), "expanded image hash mismatch");
            }
            std::cout << "verified bundle " << argv[3] << std::endl; return 0;
        }
        require(getuid() == 0, "root required");
        if (argc == 3 && std::string(argv[1]) == "--recover") {
            char parent[PATH_MAX]{};
            auto path = "/proc/" + std::to_string(getppid()) + "/exe";
            ssize_t n = readlink(path.c_str(), parent, sizeof(parent) - 1);
            require(n > 0 && (std::string(parent) == "/system/bin/recovery" || std::string(parent) == "/sbin/recovery") && !mounted("/data"), "installer must be invoked by Recovery");
            return recover(argv[2]);
        }
        require(android::base::GetProperty("ro.product.device", "") == "H6_POR" && android::base::GetProperty("ro.hsvj.rom_update.protocol", "") == "1", "ROM does not support internal upgrade protocol 1");
        const bool verification = argc >= 3 && (std::string(argv[1]) == "--manifest" || std::string(argv[1]) == "--check" || std::string(argv[1]) == "--arm");
        const bool allow_downgrade = verification && argc == 4 && std::string(argv[3]) == "--allow-downgrade";
        require((argc == 2 && std::string(argv[1]) == "--status") ||
                (argc == 3 && std::string(argv[1]) == "--stage") ||
                (verification && (argc == 3 || allow_downgrade)),
                "usage: --status | --stage NAME | --manifest ID [--allow-downgrade] | --check ID [--allow-downgrade] | --arm ID [--allow-downgrade]");
        storage(false); layout();
        Fd lock(open((std::string(kRoot) + "lock").c_str(), O_CREAT | O_RDWR | O_NOFOLLOW, 0600));
        require(flock(lock.n, LOCK_EX | LOCK_NB) == 0, "upgrade already running");
        if (argc == 2 && std::string(argv[1]) == "--status") {
            Json::Value v; v["protocol"] = 1; v["version_code"] = android::base::GetIntProperty("ro.hsvj.rom.version", 0);
            v["supports_reflash"] = true; v["supports_downgrade"] = true;
            v["hardware_restricted"] = false;
            v["product"] = "H6_POR"; v["hardware"] = kHardware;
            v["device_id"] = android::base::GetProperty("ro.serialno", "");
            if (access((std::string(kRoot) + "status.json").c_str(), F_OK) == 0) {
                auto state = parse(read_file(std::string(kRoot) + "status.json", 65536));
                bool pending = state["phase"] == "installing" || state["phase"] == "armed";
                bool bootChanged = state["boot_id"].asString() != read_file("/proc/sys/kernel/random/boot_id", 128);
                if (state["phase"] == "installed" || (pending && bootChanged)) {
                    bool success = state["phase"] == "installed" && state["target_fingerprint"].asString() == android::base::GetProperty("ro.build.fingerprint", "") && state["version_code"] == v["version_code"];
                    state["phase"] = success ? "succeeded" : "failed"; state["message"] = success ? "ROM boot version verified" : "booted ROM does not match target"; atomic_json("status.json", state);
                }
                v["result"] = state;
                if (state["phase"] == "succeeded" || state["phase"] == "failed") {
                    for (const char* file : {"update.img", "partition.tmp"}) unlink((std::string(kRoot) + file).c_str());
                }
            }
            std::cout << json(v) << std::endl; return 0;
        }
        if (access((std::string(kRoot) + "status.json").c_str(), F_OK) == 0) {
            auto previous = parse(read_file(std::string(kRoot) + "status.json", 65536));
            require(!hsvj_rom::upgrade_pending(previous["phase"].asString()), "an upgrade is pending; staging is locked");
        }
        if (argc == 3 && std::string(argv[1]) == "--stage") {
            std::string name(argv[2]);
            require(name == "manifest.json" || name == "manifest.sig" || name == "update.img", "invalid staging resource");
            Fd input(open(("/huoshan/.rom-download/" + name).c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
            struct stat st{}; require(fstat(input.n, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 && static_cast<uint64_t>(st.st_size) <= (name == "update.img" ? kMaxImage : 65536), "invalid staging input");
            std::string path = std::string(kRoot) + name;
            Fd output(open((path + ".tmp").c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_NOFOLLOW, 0600));
            std::array<char, 65536> buffer{}; ssize_t n;
            uint64_t copied = 0;
            while ((n = read(input.n, buffer.data(), buffer.size())) > 0) {
                copied += n; require(copied <= static_cast<uint64_t>(st.st_size), "staging input grew");
                write_all(output.n, buffer.data(), n);
            }
            require(copied == static_cast<uint64_t>(st.st_size), "staging input changed");
            require(n == 0 && fsync(output.n) == 0 && rename((path + ".tmp").c_str(), path.c_str()) == 0, "stage failed");
            Fd dir(open(kRoot, O_RDONLY | O_DIRECTORY)); require(fsync(dir.n) == 0, "stage sync failed");
            std::cout << "staged" << std::endl; return 0;
        }
        auto m = manifest(argv[2]);
        const int current_version = android::base::GetIntProperty("ro.hsvj.rom.version", 0);
        require(current_version > 0, "invalid current ROM version");
        require(hsvj_rom::version_allowed(current_version, m["version_code"].asInt(), allow_downgrade), "ROM downgrade requires --allow-downgrade");
        if (std::string(argv[1]) == "--manifest") { std::cout << json(m) << std::endl; return 0; }
        // Terminal transactions can be retried explicitly, including the same
        // release. Pending transactions stay locked above; Recovery clears BCB
        // before writing, so a power failure never schedules an automatic retry.
        Fd image(open((std::string(kRoot) + "update.img").c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)); check_image(m, image.n);
        struct statvfs fs{}; require(statvfs(kRoot, &fs) == 0, "cannot inspect free storage");
        uint64_t largest = 0; for (const auto& e : m["partitions"]) largest = std::max<uint64_t>(largest, e["size"].asUInt64());
        require(static_cast<uint64_t>(fs.f_bavail) * fs.f_frsize > largest + 64 * 1024 * 1024, "not enough space for partition staging");
        if (std::string(argv[1]) == "--arm") {
            status(argv[2], m, "armed", "reboot to internal ROM installer requested"); bcb(argv[2]); sync();
            if (!android::base::SetProperty("sys.powerctl", "reboot,recovery")) {
                bcb(""); status(argv[2], m, "failed", "reboot request failed");
                throw std::runtime_error("reboot request failed");
            }
        }
        std::cout << "verified" << std::endl; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << std::endl; return 1; }
}
