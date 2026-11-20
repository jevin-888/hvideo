"""Run the production HTTP route wrapper with a stub transport/license provider.

Linux/WSL: python3 scripts/test_scene_route_license.py (requires g++).
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "src/network/HttpServer.cpp").read_text(encoding="utf-8-sig")
start = source.index("void HttpServer::addRoute(")
end = source.index("bool HttpServer::checkModuleLicense(", start)
wrapper = source[start:end]
harness = r'''
#include <cassert>
#include <functional>
#include <iostream>
#include <string>
struct HttpRequest {};
struct HttpResponse {};
using Handler = std::function<void(const HttpRequest&, HttpResponse&)>;
struct Transport {
  Handler handler;
  void addRoute(const std::string&, const std::string&, Handler next) { handler = next; }
};
struct HttpServer {
  Transport transport;
  Transport *impl_ = &transport;
  bool licensed = false;
  std::string checkedModule;
  bool checkModuleLicense(const std::string& module, HttpResponse&) {
    checkedModule = module;
    return licensed;
  }
  void addRoute(const std::string&, const std::string&, Handler);
};
'''
harness += wrapper
harness += r'''
int main() {
  HttpServer server;
  auto check = [&](const char *method, const char *path, bool licensed,
                   bool expected, const char *module) {
    server.licensed = licensed;
    server.checkedModule.clear();
    bool called = false;
    server.addRoute(method, path, [&](const HttpRequest&, HttpResponse&) { called = true; });
    HttpRequest request;
    HttpResponse response;
    server.transport.handler(request, response);
    assert(called == expected);
    assert(server.checkedModule == module);
  };
  // Shared action route must reach CommandRouter's per-action permission checks.
  check("POST", "/api/v1/scenes/actions/{action}", false, true, "");
  check("GET", "/api/v1/scenes/current", false, true, "");
  check("GET", "/api/v1/scenes", false, false, "scene");
  check("POST", "/api/v1/scenes", false, false, "scene");
  check("GET", "/api/v1/scenes/{name}", false, false, "scene");
  check("PUT", "/api/v1/scenes/{name}", false, false, "scene");
  check("DELETE", "/api/v1/scenes/{name}", false, false, "scene");
  check("GET", "/api/v1/scenes/actions/{action}", false, false, "scene");
  check("GET", "/api/v1/scenes", true, true, "scene");
  check("POST", "/api/v1/fusion/actions/{action}", false, false, "fusion");
  check("GET", "/api/v1/system/status", false, true, "");
  std::cout << "PASS: production HTTP wrapper, scene action delegation, protected CRUD/fusion\n";
}
'''
with tempfile.TemporaryDirectory(prefix="hsvj-scene-route-") as temporary:
    executable = str(Path(temporary) / "route-test")
    subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-x", "c++", "-", "-o", executable], input=harness, text=True, check=True)
    subprocess.run([executable], check=True)
