"""Exercise the production material index and material query handlers (WSL/Linux)."""
from pathlib import Path
import argparse
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
route_source = (ROOT / 'src/network/HttpServer_Material.cpp').read_text(encoding='utf-8')
routes = route_source[route_source.index('void HttpServer::registerMaterialRoutes()'):route_source.index('  // 文件删除 API')] + '}\n'
HARNESS = r'''
#include "database/MaterialIndex.h"
#include "foundation/VideoStorage.h"
#include "utils/FileUtils.h"
#include "utils/Logger.h"
#include "utils/MediaUtils.h"
#include <sqlite3.h>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <functional>
#include <map>
namespace fs = std::filesystem;
namespace hsvj {
Logger::Level Logger::getLevel() { return Logger::FATAL_LEVEL; }
void Logger::log(Level, const char*, const char*, ...) {}
bool FileUtils::createDirectory(const std::string& path) { fs::create_directories(path); return true; }
std::string FileUtils::getDirectory(const std::string& path) { return fs::path(path).parent_path().string(); }
std::string FileUtils::calculateMD5(const std::string& path) { std::ifstream f(path); return std::string(std::istreambuf_iterator<char>(f),{}); }
bool MediaUtils::checkVideoFormatSupport(const std::string&, std::string&, std::string&) { return true; }
}
struct HttpRequest {
 std::map<std::string,std::string> query;
 std::string getQueryParam(const std::string& key) const { auto i=query.find(key); return i==query.end()?"":i->second; }
};
struct HttpResponse { int status=200; Json::Value data; };
struct HttpServer {
 hsvj::MaterialIndex* materialIndex_;
 using Handler=std::function<void(const HttpRequest&,HttpResponse&)>;
 std::map<std::string,Handler> routes;
 void get(const std::string& path, Handler h) { routes[path]=h; }
 void post(const std::string& path, Handler h) { routes[path]=h; }
 void setJsonErrorResponse(HttpResponse& r,int code,const std::string&) { r.status=code; }
 void setJsonDataResponse(HttpResponse& r,const Json::Value& data,const std::string&) { r.data=data; }
 void setJsonSuccessResponse(HttpResponse& r,const std::string&,const Json::Value& data) { r.data=data; }
 void registerMaterialRoutes();
};
''' + routes + r'''
void write(const std::string& path) { fs::create_directories(fs::path(path).parent_path()); std::ofstream(path)<<"test"; }
int main(int argc,char** argv) {
 if(argc==3 && std::string(argv[2])=="--device-db") {
   hsvj::MaterialIndex index;assert(index.initialize(argv[1],"/huoshan"));
   HttpServer server{&index};server.registerMaterialRoutes();
   auto query=[&](const std::string& endpoint, std::map<std::string,std::string> q) {HttpResponse r;server.routes.at(endpoint)(HttpRequest{q},r);assert(r.status==200);return r.data;};
   Json::Value result;
   result["all"]=query("/api/v1/materials",{{"source","all"}});
   result["cloud"]=query("/api/v1/materials",{{"source","cloud"}});
   result["local"]=query("/api/v1/materials",{{"source","local"}});
   result["folders"]=query("/api/v1/materials/folders",{{"source","cloud"}});
   for(const auto& folder:result["folders"]) {
     result["byCategory"][folder["categoryId"].asString()]=query("/api/v1/materials",{{"source","cloud"},{"categoryId",folder["categoryId"].asString()}});
   }
   std::cout<<Json::writeString(Json::StreamWriterBuilder(),result)<<"\n";
   return 0;
 }
 assert(argc==2);const std::string root=std::string(argv[1])+"/media",db=std::string(argv[1])+"/material.db";
 sqlite3* old=nullptr;assert(sqlite3_open(db.c_str(),&old)==SQLITE_OK);
 assert(sqlite3_exec(old,"CREATE TABLE materials(path TEXT PRIMARY KEY,type TEXT NOT NULL,name TEXT NOT NULL,folder TEXT,ext TEXT,size INTEGER NOT NULL DEFAULT 0,mtime INTEGER NOT NULL DEFAULT 0,compatible INTEGER NOT NULL DEFAULT 1,incompatibleReason TEXT,createdAt INTEGER NOT NULL DEFAULT 0,updatedAt INTEGER NOT NULL DEFAULT 0)",nullptr,nullptr,nullptr)==SQLITE_OK);sqlite3_close(old);
 hsvj::MaterialIndex index;assert(index.initialize(db,root));
 const std::string local=root+"/video/mixed/local.mp4",cloud=root+"/video/mixed/%E7%8E%8B.mp4";
 write(local);write(cloud);assert(index.upsertFile(local));
 // The download is playable on disk, but absent from the index until notification.
 assert(index.listMaterials("video","").size()==1);
 assert(index.upsertCloudFile(cloud,"王杰 - 英雄泪.mp4", {"song-id","common-id","通用素材",""}));
 auto all=index.listMaterials("video","");assert(all.size()==2);
 auto remote=index.listMaterials("video","",500,"internal","cloud");
 assert(remote.size()==1 && remote[0]["source"]=="cloud");
 assert(remote[0]["name"]=="王杰 - 英雄泪.mp4" && remote[0]["path"]==cloud && remote[0]["folder"]=="mixed");
 assert(index.listMaterials("video","",500,"internal","local").size()==1);
 auto folders=index.listFolders("video","internal","cloud");assert(folders.size()==1 && folders[0]["count"]==1);
 assert(folders[0]["path"]=="" && folders[0]["name"]=="通用素材" && folders[0]["kind"]=="cloud-category" && folders[0]["key"]=="cloud-category:common-id");
 assert(index.listFolders("video","internal").size()==2);
 // Filter before LIMIT: local names sort ahead of the cloud file.
 for(int i=0;i<505;++i) { auto p=root+"/video/aaa"+std::to_string(i)+".mp4";write(p);assert(index.upsertFile(p)); }
 assert(index.listMaterials("video","",1,"internal","cloud").size()==1);
 write(root+"/video/legacy/cover.png");write(root+"/video/legacy/music.mp3");
 assert(index.upsertCloudFile(root+"/video/legacy/cover.png","云端封面.png"));
 assert(index.upsertCloudFile(root+"/video/legacy/music.mp3","云端音乐.mp3"));
 write(root+"/video/new.ts");write(root+"/video/.hidden/secret.mp4");write(root+"/video/unfinished.mp4.download");
 index.runFullScanSync();
 assert(index.listMaterials("video","",500,"internal","cloud")[0]["name"]=="王杰 - 英雄泪.mp4");
 assert(index.listMaterials("image","",500,"internal","cloud").size()==1);
 assert(index.listMaterials("audio","",500,"internal","cloud").size()==1);
 assert(index.listFolders("image","internal","cloud").empty()); // Unknown category never exposes a physical cloud directory.
 assert(index.listMaterials("video",".hidden").empty());
 assert(!index.upsertCloudFile(root+"/video/missing.mp4","missing"));
 assert(!index.upsertCloudFile(root+"/outside.mp4","outside"));
 HttpServer server{&index};server.registerMaterialRoutes();
 auto query=[&](const std::string& endpoint, std::map<std::string,std::string> q) {HttpResponse r;server.routes.at(endpoint)(HttpRequest{q},r);return r;};
 auto response=query("/api/v1/materials",{{"source","cloud"},{"storage","internal"},{"categoryId","common-id"}});
 assert(response.status==200 && response.data.size()==1 && response.data[0]["path"]==cloud);
 for(const auto& endpoint:{"/api/v1/materials","/api/v1/materials/folders"}) {
   assert(query(endpoint,{{"source","invalid"}}).status==400);
   assert(query(endpoint,{{"storage","usb"}}).status==400);
   assert(query(endpoint,{{"storage","sata"},{"type","audio"}}).status==400);
   assert(query(endpoint,{{"source","all"}}).status==200);
 }
 assert(query("/api/v1/materials",{{"folder","../mixed"}}).status==400);

 assert(query("/api/v1/materials",{{"source","local"},{"categoryId","common-id"}}).status==400);
 assert(query("/api/v1/materials",{{"folder","mixed"},{"categoryId","common-id"}}).status==400);
 assert(index.listMaterials("video","mixed",500,"internal","local").size()==1);
 // Identical historical cloud copies collapse by verified cloud identity, not by title or size.
 const std::string duplicate=root+"/video/internal-uuid/copy.mp4";
 const std::string collision=root+"/video/internal-uuid/other.mp4";
 const std::string sameLocal=root+"/video/localcopy/copy.mp4";
 std::ofstream(cloud)<<std::string(32,'a');
 write(duplicate);std::ofstream(duplicate)<<std::string(32,'a');
 write(collision);std::ofstream(collision)<<std::string(32,'b');
 write(sameLocal);std::ofstream(sameLocal)<<std::string(32,'a');
 assert(index.upsertFile(sameLocal));
 assert(index.upsertCloudFile(duplicate,"王杰 - 英雄泪.mp4"));
 assert(index.upsertCloudFile(collision,"王杰 - 英雄泪.mp4"));
 assert(index.upsertCloudFile(cloud,"王杰 - 英雄泪.mp4",{"song-id","common-id","通用素材",std::string(32,'a')}));
 auto dedup=index.listMaterials("video","",500,"internal","cloud","common-id");
 assert(dedup.size()==1 && dedup[0]["path"]==cloud && dedup[0]["categoryName"]=="通用素材");
 assert(index.listFolders("video","internal","cloud")[0]["count"]==1);
 assert(index.listMaterials("video","",500,"internal","cloud").size()==2); // Different content remains.
 assert(fs::exists(duplicate)); // Existing playlist paths are preserved.
 assert(index.upsertCloudFile(cloud,"王杰 - 英雄泪.mp4",{"song-id","common-id","重命名分类",std::string(32,'a')}));
 assert(index.listFolders("video","internal","cloud")[0]["name"]=="重命名分类");
 fs::remove(duplicate);assert(index.removeFile(duplicate));
 fs::remove(collision);assert(index.removeFile(collision));
 const std::string renamed=root+"/video/mixed/renamed.mp4";fs::rename(cloud,renamed);
 assert(index.renameFile(cloud,renamed));remote=index.listMaterials("video","",500,"internal","cloud");
 assert(remote.size()==1 && remote[0]["path"]==renamed && remote[0]["name"]=="renamed.mp4" && remote[0]["categoryId"]=="common-id");
 index.shutdown();assert(index.initialize(db,root));index.runFullScanSync();
 assert(index.listMaterials("video","",500,"internal","cloud").size()==1);
 fs::remove(renamed);index.runFullScanSync();assert(index.listMaterials("video","",500,"internal","cloud").empty());
 std::cout<<"PASS: real SQLite migration, download indexing, persistent names and sources, filter before limit, cloud categories, verified identity deduplication, category rename, folder counts, media types, hidden files, rename/delete, restart, real query validation\n";
}
'''

parser = argparse.ArgumentParser()
parser.add_argument('--device-db', type=Path, help='Replay production read handlers against an already copied device database snapshot')
parser.add_argument('--device-result', type=Path)
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='hplayer-material-index-') as directory:
    temp = Path(directory)
    (temp / 'main.cpp').write_text(HARNESS, encoding='utf-8')
    subprocess.run(['gcc', '-c', str(ROOT / 'vendor/sqlite/sqlite3.c'), '-o', str(temp / 'sqlite.o'), '-DSQLITE_THREADSAFE=1'], check=True)
    sources = [ROOT / 'src/database/MaterialIndex.cpp', *sorted((ROOT / 'vendor/jsoncpp/src/lib_json').glob('*.cpp'))]
    subprocess.run(['g++', '-std=c++17', '-pthread', '-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'src'),
                    '-I' + str(ROOT / 'vendor/jsoncpp/include'), '-I' + str(ROOT / 'vendor/sqlite'),
                    str(temp / 'main.cpp'), *map(str, sources), str(temp / 'sqlite.o'), '-ldl', '-o', str(temp / 'test')], check=True)
    subprocess.run([str(temp / 'test'), str(temp)], check=True, timeout=30)
    if args.device_db:
        result = subprocess.check_output([str(temp / 'test'), str(args.device_db.resolve()), '--device-db'], text=True)
        if args.device_result:
            args.device_result.write_text(result, encoding='utf-8')
        else:
            print(result)
