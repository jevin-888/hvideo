"""Exercise production media-ctl pad format readback parsing on host C++."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "src/utils/MediaController.cpp").read_text(encoding="utf-8")
signature = "bool padHasFormat("
start = source.index(signature)
end = source.index("{", start) + 1
depth = 1
while depth:
    depth += (source[end] == "{") - (source[end] == "}")
    end += 1
parser = source[start:end]
program = r'''
#include <cassert>
#include <sstream>
#include <string>
std::string trim(const std::string &s) {
  const auto first=s.find_first_not_of(" \t\r\n");
  if(first==std::string::npos)return {};
  return s.substr(first,s.find_last_not_of(" \t\r\n")-first+1);
}
std::string entityNameFromLine(const std::string &line) {
  const auto colon=line.find(':');const auto paren=line.find('(',colon);
  return trim(line.substr(colon+1,paren-colon-1));
}
''' + parser + r'''
int main() {
  const std::string graph =
      "- entity 1: rk628 (1)\n pad0: Source\n   [fmt:UYVY8_2X8/1920x1080 field:none]\n"
      "- entity 2: dphy (2)\n pad0: Sink\n   [fmt:UYVY2X8/1920x1080 field:none]\n"
      " pad1: Source\n   [fmt:UYVY2X8/1920x1080 field:none]\n";
  assert(padHasFormat(graph, "rk628", "0", 1920, 1080));
  assert(padHasFormat(graph, "dphy", "1", 1920, 1080));
  assert(!padHasFormat(graph, "dphy", "0", 1280, 720));
  assert(!padHasFormat(graph, "dphy", "2", 1920, 1080));
}
'''
with tempfile.TemporaryDirectory(prefix="media-format-readback-") as directory:
    cpp = Path(directory) / "test.cpp"
    binary = Path(directory) / "test"
    cpp.write_text(program, encoding="utf-8")
    subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                   str(cpp), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=5)
print("PASS: pad-scoped readback accepts both UYVY bus-code spellings and rejects wrong dimensions/pads")
