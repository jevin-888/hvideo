"""Compile production RK628 EDID code and test generation and transactions."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
TEST = r'''
#include "capture/Rk628Edid.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
using namespace hsvj;
using Bytes = std::vector<uint8_t>;
const char* fixture = "00ffffffffffff0049738d6200888888081e0103800000780a0dc9a05747982712484c00000001010101010101010101010101010101023a801871382d40582c4500c48e2100001e011d007251d01e206e285500c48e2100001e000000fc00543734392d6648443732300a20000000fd00147801ff1d000a202020202020011802031a71475f902204110201230907018301000065030c001000023a801871382d40582c450020c23100001e011d007251d01e206e28550020c23100001e023a80d072382d40102c458020c23100001e011d801871382d40582c4500c06c00000018011d8018711c1620582c2500c06c000000180000000000000000000000c1";
Bytes original() {
 Bytes b;
 for(size_t i=0;fixture[i];i+=2) b.push_back(std::stoi(std::string(fixture+i,2),nullptr,16));
 return b;
}
void checksum(Bytes& b) { for(size_t o=0;o<b.size();o+=128){unsigned s=0;for(size_t i=o;i<o+127;i++)s+=b[i];b[o+127]=-s;} }
template<class F> void fails(F f) {bool failed=false;try{f();}catch(const std::exception&){failed=true;}assert(failed);}
struct Fake : CaptureEdidDevice {
 Bytes data=original();int reads=0,writes=0;bool reject=false,mismatch=false;
 Bytes read() override {reads++;return data;}
 void write(const Bytes& b) override {
  writes++;
  if(reject){reject=false;throw std::runtime_error("write rejected");}
  data=b;
  if(mismatch){data[12]^=1;checksum(data);mismatch=false;}
 }
 CaptureInputMode inputMode() override {return {1920,1080,60};}
};
int main(int argc,char**argv){
 assert(argc==2);const std::string dir=argv[1];
 auto b=original();Rk628Edid::validate(b);
 for(const auto whf : {std::array<int,3>{2160,1920,30},{2160,1920,60},{1920,1080,60},{3840,2160,30},{1280,720,50}}){
  const auto custom=Rk628Edid::withPreferred(b,whf[0],whf[1],whf[2]);
  Rk628Edid::validate(custom);
  auto d=custom.data()+54;
  assert((d[2]|((d[4]&240)<<4))==whf[0]);assert((d[5]|((d[7]&240)<<4))==whf[1]);
  assert((custom[24]&2) && !(custom[131]&15));
  assert(std::equal(b.begin()+90,b.begin()+108,custom.begin()+90));
  // Every original signal timing remains advertised somewhere.
  for(size_t pos:{54u,72u,154u,172u,190u,208u,226u}){
   bool found=false;
   for(size_t dst:{54u,72u,154u,172u,190u,208u,226u})
    if(std::equal(b.begin()+pos,b.begin()+pos+12,custom.begin()+dst)&&
       std::equal(b.begin()+pos+15,b.begin()+pos+18,custom.begin()+dst+15))found=true;
   assert(found);
  }
  assert(std::equal(b.begin()+140,b.begin()+154,custom.begin()+140)); // audio/vendor unchanged
 }
 auto broken=b;broken[20]^=1;fails([&]{Rk628Edid::validate(broken);});
 auto full=b;
 // Make every DTD unique; no slot may be silently destroyed.
 for(size_t pos:{72u,154u,172u,190u,208u,226u}) full[pos]^=static_cast<uint8_t>(pos);
 checksum(full);fails([&]{Rk628Edid::withPreferred(full,2160,1920,30);});
 auto malformed=b;malformed[130]=5;malformed[132]=0x5f;checksum(malformed);
 fails([&]{Rk628Edid::withPreferred(malformed,2160,1920,30);});
 auto device=std::make_shared<Fake>();Rk628Edid service(dir,device);
 service.restoreSaved();assert(device->reads==0&&device->writes==0);
 auto initial=service.get();assert(!initial.custom&&initial.preferred.width==1920&&initial.applied);
 service.set(false);assert(device->writes==0);
 for(auto whf:{std::array<int,3>{2161,1920,30},{2160,0,30},{2160,1920,0},{3840,2160,60},{4096,2160,30}})
  fails([&]{service.set(true,whf[0],whf[1],whf[2]);});
 assert(device->writes==0);
 auto state=service.set(true,2160,1920,30);
 assert(state.custom&&state.applied&&state.preferred.width==2160&&state.input.width==1920);
 assert(device->writes==1);
 auto custom=device->data;
 service.set(true,2160,1920,30);assert(device->writes==1);
 // New service instance proves durable config is the source of truth.
 Rk628Edid restarted(dir,device);device->data=b;restarted.restoreSaved();assert(device->data==custom);
 const int writes=device->writes;
 device->reject=true;fails([&]{restarted.set(true,2160,1920,60);});
 assert(device->data==custom&&restarted.get().requested.refreshRate==30);
 device->mismatch=true;fails([&]{restarted.set(true,2160,1920,60);});
 assert(device->data==custom&&restarted.get().requested.refreshRate==30&&device->writes>=writes+4);
 auto restored=restarted.set(false);assert(!restored.custom&&restored.applied&&device->data==b);
 const int restoredWrites=device->writes;restarted.restoreSaved();assert(device->writes==restoredWrites);
 device->data=custom;restarted.restoreSaved();assert(device->data==b);
 // Corrupt persistent data must fail before touching receiver state.
 std::ofstream(dir+"/rk628-edid.json")<<"broken";
 fails([&]{restarted.set(true,2160,1920,30);});assert(device->data==b);
 std::cout<<"PASS: EDID checksums/timings/fallbacks/native flags, invalid and full EDIDs, default no write, persistence, restart, idempotence, write/readback rollback, reset and corrupt-config rejection\n";
}
'''

with tempfile.TemporaryDirectory(prefix="hsvj-rk628-test-") as folder:
    temp = Path(folder)
    source, executable = temp / "test.cpp", temp / "test"
    source.write_text(TEST, encoding="utf-8")
    json = ROOT / "vendor/jsoncpp"
    subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
                    "-I" + str(ROOT / "include"), "-I" + str(json / "include"),
                    str(ROOT / "src/capture/Rk628Edid.cpp"), str(source),
                    *[str(json / "src/lib_json" / name) for name in
                      ["json_reader.cpp", "json_value.cpp", "json_writer.cpp"]],
                    "-o", str(executable)], check=True)
    subprocess.run([str(executable), str(temp / "config")], check=True)
