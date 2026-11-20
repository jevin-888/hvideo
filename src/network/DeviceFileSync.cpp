#include "network/DeviceFileSync.h"
#include "network/SyncContract.h"
#include "utils/HttpClient.h"
#include "utils/JsonUtils.h"
#include "utils/Sha256.h"
#include <sqlite3.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace hsvj {
namespace {
namespace fs = std::filesystem;
using namespace sync_contract;
std::mutex receiveMutex;
bool dataOperationActive = false;
DeviceDataGuard::Purpose dataOperationPurpose = DeviceDataGuard::Purpose::Source;
std::string reservedPullPeer;
std::string cachedRoot, cachedId;
Json::Value cachedPlan;
std::chrono::steady_clock::time_point lastChunkAt;

void require(bool ok, const std::string& message) {
  if (!ok) throw std::runtime_error(message);
}
std::string hex(const unsigned char* bytes, size_t size) {
  std::ostringstream out;
  for (size_t i = 0; i < size; ++i)
    out << std::hex << std::setw(2) << std::setfill('0') << unsigned(bytes[i]);
  return out.str();
}
std::string hashBytes(const std::string& bytes) {
  unsigned char hash[32];
  Sha256Ctx context;
  sha256Init(&context);
  sha256Update(&context, reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
  sha256Final(&context, hash);
  return hex(hash, sizeof(hash));
}
std::string hashFile(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  require(bool(in), "无法读取文件：" + file.string());
  Sha256Ctx context;
  sha256Init(&context);
  std::array<char, 128 * 1024> buffer;
  while (in) {
    in.read(buffer.data(), buffer.size());
    if (in.gcount()) sha256Update(&context, reinterpret_cast<const unsigned char*>(buffer.data()), size_t(in.gcount()));
  }
  require(in.eof(), "读取文件失败：" + file.string());
  unsigned char hash[32];
  sha256Final(&context, hash);
  return hex(hash, sizeof(hash));
}
std::string readText(const fs::path& path) {
  require(!fs::is_symlink(path) && fs::is_regular_file(path) &&
          fs::file_size(path) <= kMaxReceiveBodyBytes, "任务记录无效或过大");
  std::ifstream in(path, std::ios::binary);
  require(bool(in), "无法读取任务记录");
  std::string result(std::istreambuf_iterator<char>(in), {});
  require(!in.bad(), "任务记录读取不完整");
  return result;
}
Json::Value readJson(const fs::path& path) {
  Json::Value value;
  std::string error;
  require(JsonUtils::parseJson(readText(path), value, error), "任务记录损坏");
  return value;
}
void syncDirectory(const fs::path& path) {
  int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY);
  require(fd >= 0, "无法打开目录进行持久化");
  int rc = fsync(fd);
  int saved = errno;
  close(fd);
  require(rc == 0 || saved == EINVAL || saved == ENOTSUP, "目录持久化失败");
}
void durableWrite(const fs::path& path, const std::string& bytes) {
  fs::path temporary = path.string() + ".tmp";
  int fd = open(temporary.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_NOFOLLOW, 0600);
  require(fd >= 0, "无法创建任务记录");
  size_t written = 0;
  while (written < bytes.size()) {
    ssize_t n = write(fd, bytes.data() + written, bytes.size() - written);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    written += size_t(n);
  }
  bool ok = written == bytes.size() && fsync(fd) == 0;
  if (close(fd) != 0) ok = false;
  if (!ok) { fs::remove(temporary); throw std::runtime_error("写入任务记录失败，请检查剩余空间"); }
  fs::rename(temporary, path);
  syncDirectory(path.parent_path());
}
bool safeId(const std::string& id) {
  return id.size() == 32 && id.find_first_not_of("0123456789abcdef") == std::string::npos;
}
bool safeHash(const Json::Value& value) {
  return value.isString() && value.asString().size() == 64 &&
         value.asString().find_first_not_of("0123456789abcdef") == std::string::npos;
}
std::string randomId() {
  std::array<unsigned char, 16> bytes{};
  std::ifstream random("/dev/urandom", std::ios::binary);
  random.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
  require(random.gcount() == std::streamsize(bytes.size()), "无法创建同步任务标识");
  return hex(bytes.data(), bytes.size());
}
void checkSpace(const fs::path& root, uint64_t bytes) {
  struct statvfs space{};
  require(statvfs(root.c_str(), &space) == 0 &&
          uint64_t(space.f_bavail) * space.f_frsize >= bytes + 16ULL * 1024 * 1024,
          "设备空间不足，未修改原数据");
}
void noLinks(const fs::path& base, const std::string& relative) {
  require(!fs::is_symlink(base) && fs::is_directory(base), "同步基础目录无效");
  fs::path current = base;
  const fs::path parts(relative);
  for (auto part = parts.begin(); part != parts.end(); ++part) {
    current /= *part;
    std::error_code error;
    auto status = fs::symlink_status(current, error);
    require(!error || error == std::errc::no_such_file_or_directory,
            "无法检查同步路径：" + relative);
    require(!fs::is_symlink(status), "同步路径不能包含符号链接：" + relative);
    if (std::next(part) != parts.end())
      require(!fs::exists(status) || fs::is_directory(status), "同步路径父级不是目录：" + relative);
  }
}
bool safeSerial(const Json::Value& serial) {
  return serial.isString() && !serial.asString().empty() && serial.asString().size() <= 128 &&
         serial.asString().find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_-") == std::string::npos;
}
std::string processIdentity(const std::string& pid) {
  if (pid.empty() || pid.find_first_not_of("0123456789") != std::string::npos) return {};
  std::ifstream input("/proc/" + pid + "/stat");
  std::string stat;
  if (!std::getline(input, stat)) return {};
  const auto end = stat.rfind(')');
  if (end == std::string::npos) return {};
  std::istringstream fields(stat.substr(end + 2));
  std::string value;
  for (int i = 0; i <= 19; ++i) if (!(fields >> value)) return {};
  return pid + ":" + value;
}
void clearCache() {
  cachedRoot.clear(); cachedId.clear(); cachedPlan = Json::Value();
}
const Json::Value& loadPlan(const fs::path& root, const std::string& id) {
  if (cachedRoot != root.string() || cachedId != id) {
    cachedPlan = readJson(root / (".hsvj-sync-" + id) / "plan.json");
    cachedRoot = root.string(); cachedId = id;
    lastChunkAt = std::chrono::steady_clock::now();
  }
  return cachedPlan;
}
// A killed sender/receiver must not leave a permanent busy state.
void cleanupExpiredTransfer(const fs::path& root) {
  const fs::path active = root / ".hsvj-sync-active";
  if (fs::exists(root / ".hsvj-sync-pending") || !fs::exists(active)) return;
  require(!fs::is_symlink(active), "同步记录不能是符号链接");
  const std::string id = readText(active);
  require(safeId(id), "中断同步任务标识无效");
  const fs::path work = root / (".hsvj-sync-" + id);
  require(!fs::is_symlink(work), "同步目录不能是符号链接");
  bool expired = !fs::exists(work / "plan.json");
  if (!expired) {
    const auto& plan = loadPlan(root, id);
    const std::string owner = plan.get("owner", "").asString();
    expired = plan.get("boot_id", "").asString() != readText("/proc/sys/kernel/random/boot_id") ||
              owner.empty() || processIdentity(owner.substr(0, owner.find(':'))) != owner ||
              std::chrono::steady_clock::now() - lastChunkAt > std::chrono::seconds(kReceiveLeaseSeconds);
  }
  if (expired) {
    fs::remove(active); syncDirectory(root);
    fs::remove_all(work); syncDirectory(root); clearCache();
  }
}
void cleanupOrphanTransfers(const fs::path& root) {
  if (fs::exists(root / ".hsvj-sync-pending")) return;
  const std::string active = fs::exists(root / ".hsvj-sync-active")
      ? readText(root / ".hsvj-sync-active") : "";
  bool cleaned = false;
  for (const auto& entry : fs::directory_iterator(root)) {
    const std::string name = entry.path().filename().string();
    const std::string prefix = ".hsvj-sync-";
    if (name.rfind(prefix, 0) != 0) continue;
    const std::string id = name.substr(prefix.size());
    if (!safeId(id) || id == active || entry.is_symlink() || !entry.is_directory()) continue;
    fs::remove_all(entry.path()); cleaned = true;
  }
  if (cleaned) syncDirectory(root);
}
void syncStageDirectories(const fs::path& stage) {
  std::vector<fs::path> directories{stage};
  for (const auto& entry : fs::recursive_directory_iterator(stage)) {
    require(!entry.is_symlink(), "暂存目录不能包含符号链接");
    if (entry.is_directory()) directories.push_back(entry.path());
  }
  std::sort(directories.rbegin(), directories.rend());
  for (const auto& directory : directories) syncDirectory(directory);
}
std::string encode(const std::string& bytes) {
  static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((bytes.size() + 2) / 3 * 4);
  for (size_t i = 0; i < bytes.size(); i += 3) {
    unsigned a = static_cast<unsigned char>(bytes[i]);
    unsigned b = i + 1 < bytes.size() ? static_cast<unsigned char>(bytes[i+1]) : 0;
    unsigned c = i + 2 < bytes.size() ? static_cast<unsigned char>(bytes[i+2]) : 0;
    out += table[a >> 2]; out += table[((a & 3) << 4) | (b >> 4)];
    out += i + 1 < bytes.size() ? table[((b & 15) << 2) | (c >> 6)] : '=';
    out += i + 2 < bytes.size() ? table[c & 63] : '=';
  }
  return out;
}
std::string decode(const std::string& text) {
  require(text.size() <= (kChunkBytes + 2) / 3 * 4 && text.size() % 4 == 0,
          "同步数据块大小无效");
  auto value = [](unsigned char ch) -> int {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    return ch == '+' ? 62 : (ch == '/' ? 63 : -1);
  };
  std::string out;
  out.reserve(text.size() / 4 * 3);
  for (size_t i = 0; i < text.size(); i += 4) {
    int a=value(text[i]), b=value(text[i+1]);
    int c=text[i+2]=='=' ? 0 : value(text[i+2]);
    int d=text[i+3]=='=' ? 0 : value(text[i+3]);
    require(a >= 0 && b >= 0 && c >= 0 && d >= 0 &&
            (i+4 == text.size() || (text[i+2]!='=' && text[i+3]!='=')) &&
            (text[i+2]!='=' || (text[i+3]=='=' && (b & 15)==0)) &&
            (text[i+3]!='=' || text[i+2]=='=' || (c & 3)==0), "Base64 数据损坏");
    out += char((a<<2)|(b>>4));
    if (text[i+2]!='=') out += char((b<<4)|(c>>2));
    if (text[i+3]!='=') out += char((c<<6)|d);
  }
  require(out.size() <= kChunkBytes, "同步数据块过大");
  return out;
}
bool isDatabase(const fs::path& path) {
  std::string ext = path.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) { return std::tolower(ch); });
  return ext == ".db" || ext == ".sqlite" || ext == ".sqlite3";
}
bool transientFile(const std::string& path) {
  std::string name = fs::path(path).filename().string();
  return name.find(".tmp.") != std::string::npos || name.rfind(".hsvj-", 0)==0 ||
         (name.size()>4 && (name.compare(name.size()-4,4,"-wal")==0 ||
                           name.compare(name.size()-4,4,"-shm")==0)) ||
         (name.size()>8 && name.compare(name.size()-8,8,"-journal")==0);
}
void databaseSnapshot(const fs::path& from, const fs::path& to) {
  sqlite3 *source=nullptr, *destination=nullptr;
  sqlite3_backup* backup=nullptr;
  auto cleanup = [&]() { if(backup) sqlite3_backup_finish(backup);
                        if(source) sqlite3_close(source); if(destination) sqlite3_close(destination); };
  try {
    require(sqlite3_open_v2(from.c_str(), &source, SQLITE_OPEN_READONLY, nullptr)==SQLITE_OK,
            "无法打开数据库进行一致性快照：" + from.filename().string());
    require(sqlite3_open(to.c_str(), &destination)==SQLITE_OK, "无法创建数据库快照");
    backup=sqlite3_backup_init(destination,"main",source,"main");
    require(backup!=nullptr, "数据库快照初始化失败");
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
    int rc;
    do {
      rc=sqlite3_backup_step(backup,256);
      if(rc==SQLITE_BUSY || rc==SQLITE_LOCKED) std::this_thread::sleep_for(std::chrono::milliseconds(20));
      require(std::chrono::steady_clock::now()<deadline,"数据库持续被写入，无法完成快照，请稍后重试");
    } while(rc==SQLITE_OK || rc==SQLITE_BUSY || rc==SQLITE_LOCKED);
    require(rc==SQLITE_DONE,"数据库快照失败："+from.filename().string());
    int finished=sqlite3_backup_finish(backup); backup=nullptr;
    require(finished==SQLITE_OK,"数据库快照提交失败");
    sqlite3_stmt* check=nullptr;
    bool valid=sqlite3_prepare_v2(destination,"PRAGMA quick_check",-1,&check,nullptr)==SQLITE_OK &&
               sqlite3_step(check)==SQLITE_ROW &&
               std::strcmp(reinterpret_cast<const char*>(sqlite3_column_text(check,0)),"ok")==0;
    sqlite3_finalize(check);
    require(valid,"数据库完整性检查失败："+from.filename().string());
    cleanup(); source=nullptr; destination=nullptr;
  } catch(...) { cleanup(); throw; }
}
void snapshotFile(const fs::path& from, const fs::path& to, const std::string& relative) {
  fs::create_directories(to.parent_path());
  if(isDatabase(from)) { databaseSnapshot(from,to); return; }
  struct stat before{}, after{};
  int fd=open(from.c_str(),O_RDONLY|O_NOFOLLOW);
  require(fd>=0,"无法打开源文件："+relative);
  try {
    require(fstat(fd,&before)==0 && S_ISREG(before.st_mode) && before.st_size>=0,
            "源文件不是普通文件："+relative);
    std::ofstream output(to,std::ios::binary);
    require(bool(output),"无法写入源文件快照");
    std::array<char,128*1024> bytes;
    uint64_t remaining=uint64_t(before.st_size);
    while(remaining) {
      ssize_t n=read(fd,bytes.data(),std::min<uint64_t>(bytes.size(),remaining));
      if(n<0 && errno==EINTR) continue;
      require(n>0,"读取源文件不完整："+relative);
      output.write(bytes.data(),n); require(bool(output),"写入快照失败"); remaining-=uint64_t(n);
    }
    output.close(); require(bool(output),"关闭快照失败");
    require(fstat(fd,&after)==0,"无法核对源文件");
    bool stable=before.st_size==after.st_size && before.st_mtim.tv_sec==after.st_mtim.tv_sec &&
                before.st_mtim.tv_nsec==after.st_mtim.tv_nsec;
    require(stable || (relative.rfind("logs/",0)==0 && after.st_size>=before.st_size),
            "同步准备期间源文件发生变化，请重试："+relative);
    close(fd);
  } catch(...) { close(fd); throw; }
}
struct Workspace {
  fs::path path;
  ~Workspace() { std::error_code e; if(!path.empty()) fs::remove_all(path,e); }
};
Json::Value callPeer(const std::string& ip, const std::string& endpoint,
                     const Json::Value& body, int timeout=kPostTimeoutSeconds) {
  std::string lastError;
  for(int attempt=0;attempt<=kBatchRetryCount;++attempt) {
    int status=0;
    std::string raw=httpPostJson("http://"+ip+":"+std::to_string(kHttpPort)+endpoint,
                                JsonUtils::toString(body),timeout,"",&status);
    Json::Value envelope;
    std::string parseError;
    if(status>=200 && status<300 && JsonUtils::parseJson(raw,envelope,parseError) &&
       envelope["ok"].isBool() && envelope["ok"].asBool() && envelope["error"].isNull() &&
       envelope["data"].isObject()) return envelope["data"];
    lastError=envelope["error"]["message"].isString() ? envelope["error"]["message"].asString()
                                                           : "连接中断或对端响应无效";
    if(status>=400 && status<500) break;
  }
  throw std::runtime_error(ip+"："+lastError);
}
} // namespace

Json::Value getDeviceFileSyncStatus(const std::string& rootPath) {
  Json::Value status(Json::objectValue);
  status["protocol"] = kProtocolVersion; status["restart_required"] = false;
  status["state"] = "idle";
  try {
    if (rootPath.empty()) { status["state"] = "not_ready"; return status; }
    const fs::path root = fs::canonical(rootPath);
    std::unique_lock<std::mutex> lock(receiveMutex, std::try_to_lock);
    if (lock.owns_lock()) {
      cleanupExpiredTransfer(root);
      if (dataOperationActive && dataOperationPurpose != DeviceDataGuard::Purpose::Pull)
        status["state"] = dataOperationPurpose == DeviceDataGuard::Purpose::Backup ? "backup" : "sending";
    }
    const fs::path pending = root / ".hsvj-sync-pending";
    if (fs::exists(pending)) {
      status["state"] = "pending_restart"; status["restart_required"] = true;
      const std::string id = readText(pending);
      if (safeId(id)) status["transfer_id"] = id;
    } else if (fs::exists(root / ".hsvj-sync-active")) status["state"] = "receiving";
    if (fs::exists(root / ".hsvj-sync-result")) status["last_result"] = readText(root / ".hsvj-sync-result");
  } catch (const std::exception&) { status["state"] = "unavailable"; }
  return status;
}

DeviceDataGuard::DeviceDataGuard(const std::string& rootPath, Purpose purpose,
                                 std::string& error, const std::string& pullPeer) {
  std::unique_lock<std::mutex> lock(receiveMutex, std::try_to_lock);
  if (!lock.owns_lock()) { error = "同步数据正在处理，请稍后重试"; return; }
  try {
    require(!dataOperationActive, "正在同步或导入导出配置，请稍后重试");
    const fs::path root = fs::canonical(rootPath);
    cleanupExpiredTransfer(root);
    cleanupOrphanTransfers(root);
    require(!fs::exists(root / ".hsvj-sync-active"), "设备正在接收同步，请稍后重试");
    require(!fs::exists(root / ".hsvj-sync-pending") && !fs::exists(root / ".hsvj-backup-pending"),
            "已有完整数据等待应用，请先重启系统");
    require(purpose != Purpose::Pull || isLanIpv4(pullPeer), "拉取源 IP 无效");
    dataOperationPurpose = purpose;
    reservedPullPeer = pullPeer;
    dataOperationActive = acquired_ = true;
  } catch (const std::exception& failure) { error = failure.what(); }
}
DeviceDataGuard::~DeviceDataGuard() {
  if (!acquired_) return;
  std::lock_guard<std::mutex> lock(receiveMutex);
  dataOperationActive = false;
  reservedPullPeer.clear();
}

bool verifyDeviceSyncPeer(const std::string& ip, const std::string& serial,
                          const std::string& model, std::string& error) {
  try {
    require(isLanIpv4(ip) && safeSerial(Json::Value(serial)), "设备 IP 或序列号缺失，请重新扫描");
    int status = 0;
    const std::string raw = httpGet("http://" + ip + ":" + std::to_string(kHttpPort) +
                                   "/api/v1/system/discovery", 10, "", &status);
    Json::Value envelope;
    std::string parseError;
    require(status == 200 && JsonUtils::parseJson(raw, envelope, parseError) &&
            envelope.isObject() && envelope["ok"].isBool() && envelope["ok"].asBool() &&
            envelope["error"].isNull() && envelope["data"].isObject(), "设备不可达或发现响应无效");
    const auto& peer = envelope["data"];
    require(peer["app_id"].isString() && peer["app_id"].asString() == "com.hsvj.engine" &&
            peer["model"].isString() && peer["model"].asString() == model &&
            peer["config_sync"].isObject() && peer["config_sync"]["protocol"].isInt() &&
            peer["config_sync"]["protocol"].asInt() == kProtocolVersion,
            "同步协议或型号不匹配，请将双方升级到相同新版播放器");
    require(peer["serial"].isString() && peer["serial"].asString() == serial,
            "设备序列号已变化，IP 可能已分配给其他设备，请重新扫描");
    require(peer["config_sync"].get("state", "idle").asString() == "idle",
            "接收设备正在同步或已有数据待应用，请检查状态并重启系统");
    return true;
  } catch (const std::exception& failure) { error = ip + "：" + failure.what(); return false; }
}

bool receiveDeviceFiles(const std::string& rootPath, const Json::Value& request,
                        const std::string& peerIp, const std::string& localSerial,
                        const std::string& model, Json::Value& result, std::string& error) {
  result = Json::Value(Json::objectValue);
  std::unique_lock<std::mutex> lock(receiveMutex, std::try_to_lock);
  if (!lock.owns_lock()) { error = "正在处理同步数据，请稍后重试"; return false; }
  try {
    require(isLanIpv4(peerIp), "同步仅接受局域网设备");
    require(request.isObject() && request["protocol"].isInt() &&
            request["protocol"].asInt() == kProtocolVersion,
            "同步协议不匹配，请将两台设备升级到相同新版播放器");
    require(request["base_path"].isString() && request["base_path"].asString() == kBasePath,
            "base_path must be current");
    require(safeSerial(request["expected_serial"]) && request["expected_serial"].asString() == localSerial &&
            request["expected_model"].isString() && request["expected_model"].asString() == model,
            "接收设备身份已变化，请重新扫描后同步");
    require(request["transfer_id"].isString() && safeId(request["transfer_id"].asString()), "任务标识无效");
    require(request["operation"].isString(), "缺少同步操作");
    require(!dataOperationActive ||
            (dataOperationPurpose == DeviceDataGuard::Purpose::Pull && peerIp == reservedPullPeer),
            "设备正在同步或导入导出配置，请稍后重试");
    const std::string id = request["transfer_id"].asString();
    const std::string operation = request["operation"].asString();
    const fs::path root = fs::canonical(rootPath), work = root / (".hsvj-sync-" + id);
    const fs::path active = root / ".hsvj-sync-active", pending = root / ".hsvj-sync-pending";
    require(!fs::is_symlink(work) && !fs::is_symlink(active) && !fs::is_symlink(pending),
            "同步记录不能是符号链接");
    if (fs::exists(pending)) {
      require(readText(pending) == id && operation == "commit", "已有完整同步待应用，请先重启系统");
      const auto& plan = loadPlan(root, id);
      require(plan["peer_ip"].asString() == peerIp, "同步来源不匹配");
      result["files_count"] = plan["files"].size(); result["committed"] = true;
      result["restart_required"] = true; result["transfer_id"] = id;
      return true;
    }
    require(!fs::exists(root / ".hsvj-backup-pending"), "已有备份待还原，请先重启系统");
    cleanupExpiredTransfer(root);
    if (operation == "begin") {
      cleanupOrphanTransfers(root);
      require(!fs::exists(active) || readText(active) == id, "另一台设备正在同步，请稍后重试");
      const Json::Value& manifest = request["manifest"];
      require(manifest.isObject() && manifest["files"].isArray() && manifest["directories"].isArray(),
              "同步清单无效");
      require(JsonUtils::toString(request).size() <= kMaxReceiveBodyBytes &&
              manifest["files"].size() <= kMaxTotalFiles && manifest["directories"].size() <= kMaxTotalFiles,
              "同步清单过大");
      require(!manifest["files"].empty() || !manifest["directories"].empty(), "没有可同步内容");
      uint64_t total = 0;
      std::set<std::string> names, fileNames;
      for (const auto& directory : manifest["directories"]) {
        require(directory.isString() && isSafeRelativePath(directory.asString()) &&
                names.insert(directory.asString()).second, "目录路径无效或重复");
        const std::string name = directory.asString();
        noLinks(root, name);
        require(!fs::exists(root / name) || fs::is_directory(root / name), "同步目录目标不是目录：" + name);
      }
      for (const auto& file : manifest["files"]) {
        require(file.isObject() && file["path"].isString() && isSafeRelativePath(file["path"].asString()) &&
                file["path"].asString().find('/') != std::string::npos && file["size"].isUInt64() &&
                safeHash(file["sha256"]) && names.insert(file["path"].asString()).second,
                "文件清单路径、大小或哈希无效");
        const std::string name = file["path"].asString();
        require(!transientFile(name), "不能同步数据库临时文件或运行临时文件");
        require(file["size"].asUInt64() <= kMaxTransferBytes - total, "同步内容超过 32 GB");
        total += file["size"].asUInt64();
        noLinks(root, name);
        require(!fs::exists(root / name) || fs::is_regular_file(root / name), "同步文件目标不是普通文件：" + name);
        fileNames.insert(name);
      }
      for (const auto& name : names) {
        size_t slash = name.find('/');
        while (slash != std::string::npos) {
          require(!fileNames.count(name.substr(0, slash)), "清单中文件与父目录冲突：" + name);
          slash = name.find('/', slash + 1);
        }
      }
      Json::Value plan = manifest;
      plan["peer_ip"] = peerIp; plan["boot_id"] = readText("/proc/sys/kernel/random/boot_id");
      plan["owner"] = processIdentity(std::to_string(getpid()));
      require(!plan["owner"].asString().empty(), "无法建立接收任务租约");
      if (fs::exists(work)) {
        require(loadPlan(root, id) == plan, "重复任务清单不一致");
      } else {
        checkSpace(root, total);
        fs::create_directory(work); fs::create_directory(work / "stage");
        durableWrite(work / "plan.json", JsonUtils::toString(plan));
        syncDirectory(root);
      }
      cachedRoot = root.string(); cachedId = id; cachedPlan = plan;
      lastChunkAt = std::chrono::steady_clock::now();
      durableWrite(active, id); result["ready"] = true;
    } else {
      // Repeating an already completed abort is harmless.
      if (operation == "abort" && !fs::exists(active) && !fs::exists(work)) {
        result["aborted"] = true; result["transfer_id"] = id; return true;
      }
      require(fs::exists(active) && readText(active) == id, "同步任务不存在或已过期");
      const auto& plan = loadPlan(root, id);
      require(plan["peer_ip"].asString() == peerIp, "同步来源不匹配");
      lastChunkAt = std::chrono::steady_clock::now();
      if (operation == "abort") {
        fs::remove(active); syncDirectory(root);
        fs::remove_all(work); syncDirectory(root); clearCache(); result["aborted"] = true;
      } else if (operation == "chunk") {
        require(request["file_index"].isUInt() && request["file_index"].asUInt() < plan["files"].size() &&
                request["offset"].isUInt64() && request["content_base64"].isString() && safeHash(request["sha256"]),
                "数据块参数无效");
        const auto& file = plan["files"][request["file_index"].asUInt()];
        const uint64_t offset = request["offset"].asUInt64(), size = file["size"].asUInt64();
        const std::string bytes = decode(request["content_base64"].asString());
        require(offset <= size && bytes.size() <= size - offset && (!bytes.empty() || size == 0),
                "数据块偏移或大小不正确");
        require(hashBytes(bytes) == request["sha256"].asString(), "数据块 SHA256 校验失败，原数据未修改");
        const fs::path stage = work / "stage", path = stage / file["path"].asString();
        noLinks(stage, file["path"].asString()); fs::create_directories(path.parent_path());
        int fd = open(path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW, 0600);
        require(fd >= 0, "无法暂存数据块");
        try {
          struct stat state{};
          require(fstat(fd, &state) == 0 && S_ISREG(state.st_mode) && state.st_size >= 0, "暂存文件无效");
          const uint64_t stored = uint64_t(state.st_size);
          if (stored == offset) {
            size_t n = 0;
            while (n < bytes.size()) {
              const ssize_t written = pwrite(fd, bytes.data() + n, bytes.size() - n, off_t(offset + n));
              if (written < 0 && errno == EINTR) continue;
              require(written > 0, "暂存写入失败，请检查剩余空间"); n += size_t(written);
            }
            require(fsync(fd) == 0, "数据块持久化失败");
          } else {
            require(stored >= offset + bytes.size(), "数据块顺序不正确");
            std::string previous(bytes.size(), '\0'); size_t n = 0;
            while (n < previous.size()) {
              const ssize_t count = pread(fd, previous.data() + n, previous.size() - n, off_t(offset + n));
              if (count < 0 && errno == EINTR) continue;
              require(count > 0, "无法核对重试数据块"); n += size_t(count);
            }
            require(previous == bytes, "重试的数据块与已保存内容不一致");
          }
          require(close(fd) == 0, "关闭暂存文件失败"); fd = -1;
        } catch (...) { if (fd >= 0) close(fd); throw; }
        result["offset"] = Json::UInt64(offset + bytes.size()); result["sha256"] = request["sha256"];
      } else if (operation == "commit") {
        for (const auto& file : plan["files"]) {
          const std::string relative = file["path"].asString();
          const fs::path path = work / "stage" / relative;
          noLinks(work / "stage", relative);
          require(fs::is_regular_file(path) && fs::file_size(path) == file["size"].asUInt64() &&
                  hashFile(path) == file["sha256"].asString(), "文件不完整或 SHA256 不一致：" + relative);
          if (relative == "config/config.json") require(readJson(path).isObject(), "系统配置不是有效 JSON 对象");
        }
        for (const auto& directory : plan["directories"]) {
          noLinks(work / "stage", directory.asString());
          fs::create_directories(work / "stage" / directory.asString());
        }
        syncStageDirectories(work / "stage"); syncDirectory(work);
        durableWrite(pending, id); fs::remove(active); syncDirectory(root);
        result["committed"] = true; result["restart_required"] = true;
        result["files_count"] = plan["files"].size();
      } else throw std::runtime_error("未知同步操作");
    }
    result["transfer_id"] = id;
    return true;
  } catch (const std::exception& failure) { error = failure.what(); return false; }
}

bool sendDeviceFiles(const std::string& rootPath, const Json::Value& directories,
                     const Json::Value& targetIps, const Json::Value& targetSerials,
                     const std::string& model, Json::Value& result, std::string& error) {
  result = Json::Value(Json::objectValue);
  DeviceDataGuard guard(rootPath, DeviceDataGuard::Purpose::Source, error);
  if (!guard.acquired()) return false;
  Workspace snapshot;
  try {
    const std::string id = randomId();
    require(targetIps.isArray() && !targetIps.empty() && targetIps.size() <= kMaxTargetDevices,
            "同步目标数量无效");
    require(targetSerials.isObject() && targetSerials.size() == targetIps.size(),
            "缺少目标设备序列号，请重新扫描");
    std::set<std::string> unique;
    for (const auto& ip : targetIps) {
      require(ip.isString() && isLanIpv4(ip.asString()) && unique.insert(ip.asString()).second &&
              safeSerial(targetSerials[ip.asString()]), "同步目标 IP 或序列号无效或重复");
      std::string verification;
      require(verifyDeviceSyncPeer(ip.asString(), targetSerials[ip.asString()].asString(), model, verification), verification);
    }
    std::vector<std::string> selected;
    require(directories.isArray(), "dirs must be an array");
    for (const auto& dir : directories) { require(dir.isString(), "目录名称无效"); selected.push_back(dir.asString()); }
    std::string validation;
    require(validateDirectories(selected, validation), validation);
    const fs::path root = fs::canonical(rootPath);
    snapshot.path = root / (".hsvj-sync-source-" + id); fs::create_directory(snapshot.path);
    durableWrite(snapshot.path / "owner", processIdentity(std::to_string(getpid())));
    Json::Value manifest(Json::objectValue);
    manifest["files"] = Json::Value(Json::arrayValue); manifest["directories"] = Json::Value(Json::arrayValue);
    std::vector<std::string> paths, folders;
    for (const std::string& directory : selected) {
      noLinks(root, directory);
      if (!fs::exists(root / directory)) continue;
      require(fs::is_directory(root / directory), "选中的文件夹不是目录：" + directory);
      folders.push_back(directory);
      for (auto iterator = fs::recursive_directory_iterator(root / directory);
           iterator != fs::recursive_directory_iterator(); ++iterator) {
        const auto& entry = *iterator;
        const std::string relative = entry.path().lexically_relative(root).generic_string();
        require(isSafeRelativePath(relative), "源路径无效或层级过深：" + relative);
        require(!entry.is_symlink(), "同步目录包含符号链接：" + relative);
        if (entry.is_directory()) {
          if (transientFile(relative)) iterator.disable_recursion_pending();
          else folders.push_back(relative);
        } else {
          require(entry.is_regular_file(), "同步目录包含非普通文件：" + relative);
          if (!transientFile(relative)) paths.push_back(relative);
        }
        require(paths.size() <= kMaxTotalFiles && folders.size() <= kMaxTotalFiles, "同步文件数过多");
      }
    }
    std::sort(paths.begin(), paths.end()); std::sort(folders.begin(), folders.end());
    for (const auto& folder : folders) manifest["directories"].append(folder);
    uint64_t total = 0;
    for (const auto& relative : paths) {
      uint64_t size = fs::file_size(root / relative);
      require(size <= kMaxTransferBytes - total, "同步内容超过 32 GB"); checkSpace(root, size);
      noLinks(root, relative); snapshotFile(root / relative, snapshot.path / relative, relative);
      Json::Value file(Json::objectValue); file["path"] = relative;
      size = fs::file_size(snapshot.path / relative); require(size <= kMaxTransferBytes - total, "同步内容超过 32 GB");
      total += size; file["size"] = Json::UInt64(size); file["sha256"] = hashFile(snapshot.path / relative);
      manifest["files"].append(file);
    }
    require(!paths.empty() || !folders.empty(), "所选目录不存在或没有可同步内容");
    Json::Value base(Json::objectValue);
    base["protocol"] = kProtocolVersion; base["base_path"] = kBasePath; base["transfer_id"] = id;
    base["expected_model"] = model;
    Json::Value details(Json::arrayValue);
    int successful = 0, unconfirmed = 0; size_t chunks = 0;
    std::string failures;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kOrchestrationTimeoutSeconds);
    // Begin each target immediately before sending. Other targets never hold an idle lease.
    for (const auto& ip : targetIps) {
      Json::Value detail(Json::objectValue); detail["ip"] = ip;
      Json::Value request = base;
      request["expected_serial"] = targetSerials[ip.asString()];
      bool began = false, committing = false;
      try {
        request["operation"] = "begin"; request["manifest"] = manifest;
        require(JsonUtils::toString(request).size() <= kMaxReceiveBodyBytes, "同步清单过大，请分开选择目录");
        began = true;
        const auto ready = callPeer(ip.asString(), "/api/v1/sync/receive", request);
        require(ready["ready"].isBool() && ready["ready"].asBool() && ready["transfer_id"].asString() == id,
                "接收设备没有确认任务清单");
        request.removeMember("manifest");
        for (Json::ArrayIndex i = 0; i < manifest["files"].size(); ++i) {
          const auto& file = manifest["files"][i];
          std::ifstream input(snapshot.path / file["path"].asString(), std::ios::binary);
          require(bool(input), "无法读取源快照");
          uint64_t offset = 0; const uint64_t size = file["size"].asUInt64();
          do {
            require(std::chrono::steady_clock::now() < deadline, "同步任务超过两小时，请减少目录后重试");
            std::string bytes(size_t(std::min<uint64_t>(kChunkBytes, size - offset)), '\0');
            input.read(bytes.data(), bytes.size());
            require(size_t(input.gcount()) == bytes.size(), "快照读取不完整");
            request["operation"] = "chunk"; request["file_index"] = i; request["offset"] = Json::UInt64(offset);
            request["content_base64"] = encode(bytes); request["sha256"] = hashBytes(bytes);
            const auto ack = callPeer(ip.asString(), "/api/v1/sync/receive", request);
            require(ack["transfer_id"].asString() == id && ack["offset"].isUInt64() &&
                    ack["offset"].asUInt64() == offset + bytes.size() && ack["sha256"] == request["sha256"],
                    "接收端数据块确认不一致");
            offset += bytes.size(); ++chunks;
          } while (offset < size);
        }
        request.removeMember("content_base64"); request.removeMember("sha256");
        request.removeMember("offset"); request.removeMember("file_index");
        request["operation"] = "commit"; committing = true;
        const auto ack = callPeer(ip.asString(), "/api/v1/sync/receive", request, kOrchestrationTimeoutSeconds);
        require(ack["transfer_id"].asString() == id && ack["committed"].isBool() && ack["committed"].asBool() &&
                ack["restart_required"].isBool() && ack["restart_required"].asBool() &&
                ack["files_count"].isUInt64() && ack["files_count"].asUInt64() == paths.size(),
                "接收端未确认完整提交");
        detail["ok"] = true; detail["state"] = "pending_restart";
        detail["files"] = Json::UInt64(paths.size()); ++successful;
      } catch (const std::exception& failure) {
        const std::string message = ip.asString() + "：" + failure.what() +
            (committing ? "；提交状态未确认，请检查接收设备是否已等待重启" : "；未提交，原数据未修改");
        detail["ok"] = false; detail["state"] = committing ? "unconfirmed" : "failed";
        detail["error"] = message; if (committing) ++unconfirmed;
        if (!failures.empty()) failures += "；";
        failures += message;
        if (began && !committing) {
          request.removeMember("manifest"); request.removeMember("content_base64"); request["operation"] = "abort";
          try { callPeer(ip.asString(), "/api/v1/sync/receive", request, 10); } catch (...) {}
        }
      }
      details.append(detail);
    }
    result["synced_count"] = successful; result["failed_count"] = int(targetIps.size()) - successful;
    result["unconfirmed_count"] = unconfirmed; result["details"] = details;
    result["files_count"] = Json::UInt64(paths.size()); result["batch_count"] = Json::UInt64(chunks);
    result["bytes_count"] = Json::UInt64(total); result["restart_required"] = successful > 0;
    result["transfer_id"] = id;
    if (successful != int(targetIps.size())) {
      error = "已完整校验 " + std::to_string(successful) + " 台，未完成或未确认 " +
              std::to_string(int(targetIps.size()) - successful) + " 台。" + failures;
      if (successful) error += "。成功设备需重启系统后应用";
      return false;
    }
    return true;
  } catch (const std::exception& failure) { error = failure.what(); return false; }
}
} // namespace hsvj
