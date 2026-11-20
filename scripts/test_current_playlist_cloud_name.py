"""Compile the production routing, resume and cloud naming branches (Linux/WSL)."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
video = (ROOT / "src/core/CommandRouter_VideoPlayback.cpp").read_text(encoding="utf-8")
route = video[video.index("  std::string playlistId;"):video.index("  if (!traceId.empty()) {", video.index("  std::string playlistId;"))]
resume = video[video.index("    const bool hasExplicitIndex =", video.index("// 常规模式")):video.index("    std::vector<PlaylistItem> layerItems =", video.index("// 常规模式"))]
cloud = (ROOT / "src/network/CloudSyncService.cpp").read_text(encoding="utf-8")
download = cloud[cloud.index("bool CloudSyncService::downloadMaterial("):cloud.index("void CloudSyncService::reportDownloadSuccess(")]

HARNESS = r'''
#include <json/json.h>
#include "network/CloudMaterialFilename.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <vector>
#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
namespace hsvj {
std::string FileUtils::getFilename(const std::string& p){auto i=p.find_last_of('/');return i==p.npos?p:p.substr(i+1);}
std::string FileUtils::getExtension(const std::string& p){auto n=getFilename(p);auto i=n.find_last_of('.');return i==n.npos?"":n.substr(i+1);}
std::string FileUtils::getDirectory(const std::string& p){return std::filesystem::path(p).parent_path().string();}
std::string FileUtils::joinPath(const std::string& a,const std::string& b){return (std::filesystem::path(a)/b).string();}
bool FileUtils::exists(const std::string& p){return std::filesystem::exists(p);}
bool FileUtils::isFile(const std::string& p){return std::filesystem::is_regular_file(p);}
bool FileUtils::createDirectory(const std::string& p){std::filesystem::create_directories(p);return true;}
bool FileUtils::removeFile(const std::string& p){return std::filesystem::remove(p);}
int64_t FileUtils::getFileSize(const std::string& p){return std::filesystem::file_size(p);}
bool hashUnavailable=false;
std::string FileUtils::calculateMD5(const std::string& p){if(hashUnavailable)return {};std::ifstream f(p);return std::string(std::istreambuf_iterator<char>(f),{});}
namespace MediaUtils {
void generateThumbnailAsync(const std::string&,std::nullptr_t){}
bool checkVideoFormatSupport(const std::string&,std::string&,std::string&){return true;}
}
namespace JsonUtils { std::string toString(const Json::Value& j){return Json::writeString(Json::StreamWriterBuilder(),j);} }
int downloads=0;
std::string content(32,'a');
bool downloadCloudFile(const std::string&,const std::string& p){++downloads;std::ofstream(p)<<content;return true;}
struct CloudSyncService {
 struct Config {std::string materialRootPath;} config_;
 bool downloadMaterial(const std::string&,const std::string&,const std::string&,const std::string&,int64_t,std::string&);
};
'''
HARNESS += download
HARNESS += r'''
struct Item {std::string uri,title,tags;};
struct Playlist {std::string id;int targetLayerId=1;};
struct Manager {
 std::map<int,std::string> active{{1,"current"},{2,"second"}},defaults{{-1,"default"},{1,"default"},{2,"default2"}};
 std::string getActivePlaylistId(int l){return active[l];}
 std::string getDefaultPlaylistId(int l){return defaults[l];}
 int getPlaylistTargetLayer(const std::string& p){return p=="second"||p=="default2"?2:1;}
 Manager* getDatabase(){return this;}
 std::vector<Playlist> listPlaylists(){return {{"cloud"}};}
 std::vector<Item> getPlaylistItems(const std::string&,int){return {{"/video/%E7%8E%8B.mp4","王杰 - 英雄泪","cloud-id"},{"/video/local.mp4","local title",""}};}
};
struct Config {int getAudioOutputLayerId(){return 1;}};
struct LayerVideo {
 enum class PlayState {STOPPED,PLAYING,PAUSED};
 PlayState state=PlayState::PAUSED;
 int resumes=0;
 bool visible=false;
 void setVisible(bool value){visible=value;}
 PlayState getState(){return state;}
 void resume(){state=PlayState::PLAYING;++resumes;}
};
enum class HintType {PLAY};
struct Response {bool ok=false;int error=0;std::string message,dataJson;};
struct Fixture {
 Manager manager;Manager* playlistManager_=&manager;Config config;Config* systemConfig_=&config;
 LayerVideo layer;LayerVideo* videoLayer=&layer;
 void showLayer41Hint(int){}
 Response run(Json::Value param) {
 Response response;Json::Value data;std::string action=param["action"].asString();
'''
HARNESS += route + '\n data["playlistId"]=playlistId;data["layerId"]=layerId;\n'
HARNESS += resume
HARNESS += r'''
 response.ok=true;response.dataJson=JsonUtils::toString(data);return response;
 }
};
Json::Value data(const Response& r){Json::Value d;Json::Reader reader;assert(reader.parse(r.dataJson,d));assert(r.ok);return d;}
int main(int argc,char** argv){
 Fixture f;Json::Value p;p["action"]="next";
 assert(data(f.run(p))["playlistId"]=="current");
 p["action"]="previous";assert(data(f.run(p))["playlistId"]=="current");
 p["layerId"]=2;assert(data(f.run(p))["playlistId"]=="second");
 p["playlistId"]="default";assert(data(f.run(p))["playlistId"]=="default");
 p.removeMember("playlistId");p["layerId"]=1;p["action"]="play";
 assert(data(f.run(p))["playlistId"]=="current");assert(f.layer.resumes==1);
 assert(f.layer.visible);
 assert(data(f.run(p))["state"]=="playing");assert(f.layer.resumes==1);
 p["index"]=-1;assert(!f.run(p).ok);p.removeMember("index");
 p["action"]="pause";assert(data(f.run(p))["playlistId"]=="current");
 p["action"]="replay";assert(data(f.run(p))["playlistId"]=="current");
 f.manager.active.clear();p["action"]="next";assert(data(f.run(p))["playlistId"]=="default");
 const std::string url="https://cdn.test/%E7%8E%8B%E6%9D%B0_-_%E8%8B%B1%E9%9B%84%E6%B3%AA.mp4?token=x#frag";
 assert(cloudMaterialFilename("id","王杰 - 英雄泪",url)=="王杰 - 英雄泪.mp4");
 assert(cloudMaterialFilename("id","云端中文.MP4",url)=="云端中文.MP4");
 assert(cloudMaterialFilename("id","",url)=="王杰_-_英雄泪.mp4");
 assert(cloudMaterialFilename("id","","https://cdn/A+B%20C.mp4")=="A+B C.mp4");
 assert(cloudMaterialFilename("id","","https://cdn/%2520.mp4")=="%20.mp4");
 assert(cloudMaterialFilename("id",std::string("\xef\xbf\xbd")+".mp4",url)=="王杰_-_英雄泪.mp4");
 assert(cloudMaterialFilename("id","片段.第一版",url)=="片段.第一版.mp4");
 assert(cloudMaterialFilename("id","../../坏/名字",url).find('/')==std::string::npos);
 assert(cloudMaterialFilename("id","","https://cdn/")=="id.mp4");
 assert(decodeCloudPathSegment("bad%XZ%") == "bad%XZ%");
 std::string longName;for(int i=0;i<100;++i)longName+="中";
 assert(cloudMaterialFilename("id",longName,url).size()<=255);
 assert(argc==2);CloudSyncService cloud;cloud.config_.materialRootPath=argv[1];std::string out;
 assert(cloud.downloadMaterial("id","云端中文",url,content,32,out));
 assert(FileUtils::getFilename(out)=="云端中文.mp4");assert(downloads==1);
 assert(cloud.downloadMaterial("id","云端中文",url,content,32,out));assert(downloads==1);
 hashUnavailable=true;
 assert(!cloud.downloadMaterial("id","云端中文",url,content,32,out));assert(downloads==1);
 hashUnavailable=false;
 std::string other(32,'b');content=other;
 assert(cloud.downloadMaterial("other-id","云端中文",url,other,32,out));assert(downloads==2);
 assert(out.find("other-id/云端中文.mp4")!=std::string::npos);
 const std::string legacy=FileUtils::joinPath(argv[1],"%E7%8E%8B.mp4");std::ofstream(legacy)<<other;
 assert(cloud.downloadMaterial("legacy-id","王杰","https://cdn/%E7%8E%8B.mp4",other,32,out));
 assert(out==legacy);assert(downloads==2);assert(FileUtils::exists(legacy));
 hashUnavailable=true;
 assert(!cloud.downloadMaterial("unreadable","new",url,other,32,out));
 assert(downloads==3 && !FileUtils::exists(out));
 hashUnavailable=false;
 std::cout<<"PASS: active/default/explicit/layer routing, pause/play resume, legacy URI preservation, UTF-8 and percent filenames, download reuse and same-name collision\n";
 return 0;
}
} // namespace hsvj
int main(int argc,char** argv){return hsvj::main(argc,argv);}
'''

with tempfile.TemporaryDirectory(prefix="hplayer-playlist-cloud-") as folder:
    path = Path(folder)
    (path / "test.cpp").write_text(HARNESS, encoding="utf-8")
    subprocess.run(["g++", "-std=c++17", "-I" + str(ROOT / "include"),
                    "-I" + str(ROOT / "vendor/jsoncpp/include"), str(path / "test.cpp"),
                    *map(str, (ROOT / "vendor/jsoncpp/src/lib_json").glob("*.cpp")),
                    "-o", str(path / "test")], check=True)
    media = path / "media"
    media.mkdir()
    subprocess.run([str(path / "test"), str(media)], check=True)
