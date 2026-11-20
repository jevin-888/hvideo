"""Exercise the production batch branch with real JsonCpp; temporary files self-clean."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / 'src/core/PeripheralManager.cpp').read_text(encoding='utf-8')
start = SOURCE.index('    // Validate the complete batch')
end = SOURCE.index('  } else if (action == "clear_templates")', start)
branch = SOURCE[start:end]
helpers = SOURCE[SOURCE.index('std::string bytesToHexCompact('):SOURCE.index('std::string hexPreview(')]
HEADER = r'''
#include <json/json.h>
#include <algorithm>
#include <cstdint>
#include <string>
#include <sstream>
#include <set>
#include <vector>
#include <iostream>
#include <cassert>
struct SerialTemplate {
 std::string type,code,name,data; Json::Value functions; bool forwardToSerial=false;
};
struct Fixture {
 std::vector<SerialTemplate> serialTemplates_;
 bool writable=true; int saves=0;
 bool saveToDisk(){++saves;return writable;}
 Json::Value run(const Json::Value &param) {
 Json::Value result(Json::objectValue); result["ok"]=true;
'''
MAIN = r'''
int main(){
 Fixture f;
 SerialTemplate old; old.type="rs485";old.code=old.data="AA441120";old.name="old";
 old.functions.append("keep-action");old.forwardToSerial=true;
 f.serialTemplates_.push_back(old);
 SerialTemplate other=old;other.type="rs232";other.name="other-type";f.serialTemplates_.push_back(other);
 Json::Value p; p["peripheral_type"]="rs485";
 p["templates"][0]["data"]="aa:44-11 20";p["templates"][0]["name"]=" 风景 ";
 p["templates"][1]["data"]="AA551102";p["templates"][1]["name"]="暂停";
 auto r=f.run(p);assert(r["ok"].asBool());assert(r["created_count"]==1);assert(r["updated_count"]==1);
 assert(r["imported_count"]==2);assert(f.saves==1);assert(f.serialTemplates_.size()==3);
 assert(f.serialTemplates_[0].name=="风景");assert(f.serialTemplates_[0].functions[0]=="keep-action");
 assert(f.serialTemplates_[0].forwardToSerial);assert(f.serialTemplates_[1].name=="other-type");
 assert(f.serialTemplates_[2].functions.empty());
 r=f.run(p);assert(r["created_count"]==0);assert(r["updated_count"]==2);assert(f.serialTemplates_.size()==3);
 const auto unchanged=f.serialTemplates_;const int saves=f.saves;
 auto bad=p;bad["templates"][1]["data"]="GG";
 assert(!f.run(bad)["ok"].asBool());assert(f.saves==saves);assert(f.serialTemplates_[0].name==unchanged[0].name);
 bad=p;bad["templates"][1]["data"]="AA441120";assert(!f.run(bad)["ok"].asBool());
 bad=p;bad["templates"][1]["name"]=42;assert(!f.run(bad)["ok"].asBool());
 bad=p;bad["templates"][0]["name"]=" \t";assert(!f.run(bad)["ok"].asBool());
 bad=p;bad["templates"][0]["data"]=std::string(1026,'A');assert(!f.run(bad)["ok"].asBool());
 bad=p;bad["peripheral_type"]="udp";assert(!f.run(bad)["ok"].asBool());
 bad=p;bad["templates"]=Json::Value(Json::arrayValue);assert(!f.run(bad)["ok"].asBool());
 bad=p;bad["templates"]=Json::Value(Json::arrayValue);bad["templates"].resize(1001);assert(!f.run(bad)["ok"].asBool());
 f.writable=false;p["templates"][0]["name"]="must rollback";p["templates"][1]["data"]="ABCD";
 assert(!f.run(p)["ok"].asBool());assert(f.serialTemplates_.size()==unchanged.size());
 assert(f.serialTemplates_[0].name==unchanged[0].name);assert(f.serialTemplates_[0].functions==unchanged[0].functions);
 f.writable=true;p["peripheral_type"]="rs232";
 assert(f.run(p)["ok"].asBool());assert(f.serialTemplates_[0].name==unchanged[0].name);
 std::cout<<"PASS: batch validation, duplicate rejection, type isolation, function preservation, repeat import and save rollback\n";
}
'''

with tempfile.TemporaryDirectory(prefix='hplayer-serial-import-', dir='/tmp') as folder:
    path = Path(folder)
    (path / 'test.cpp').write_text(HEADER.replace('struct SerialTemplate', helpers + '\nstruct SerialTemplate', 1) + branch + '\nreturn result;}};\n' + MAIN, encoding='utf-8')
    subprocess.run(['g++','-std=c++17','-I'+str(ROOT/'vendor/jsoncpp/include'),str(path/'test.cpp'),
                    *map(str,(ROOT/'vendor/jsoncpp/src/lib_json').glob('*.cpp')), '-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
