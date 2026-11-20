#!/usr/bin/env python3
"""Static checks for the single public HTTP API contract."""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import defaultdict
from os.path import relpath
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NETWORK = ROOT / "src" / "network"
WEB = NETWORK / "web"
FLUTTER = ROOT.parent / "Fultter_pad" / "lib"
EXPECTED_ACTION_MODULES = {
    "/api/v1/system-config", "/api/v1/layers", "/api/v1/playback",
    "/api/v1/rendering", "/api/v1/sync", "/api/v1/playlists",
    "/api/v1/scenes", "/api/v1/regions", "/api/v1/lyrics",
    "/api/v1/system", "/api/v1/peripherals", "/api/v1/peripheral-events",
    "/api/v1/stage-control",
}
FORBIDDEN_PUBLIC_PATHS = ("/api/command", "/api/v1/command", "/api/v1/playlist/")
FORBIDDEN_ACTION_FIELDS = ("type", "code", "param", "action")
ROUTE_RE = re.compile(r'(?m)^\s*(get|post|put|del)\s*\(\s*"([^"]+)"')


def strip_comments(text: str) -> str:
    """Remove C/C++/JavaScript comments while preserving strings and newlines."""
    out: list[str] = []
    i, state, quote = 0, "code", ""
    while i < len(text):
        ch = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""
        if state == "line":
            if ch == "\n":
                out.append(ch)
                state = "code"
            else:
                out.append(" ")
            i += 1
            continue
        if state == "block":
            if ch == "*" and nxt == "/":
                out.extend("  ")
                i += 2
                state = "code"
            else:
                out.append("\n" if ch == "\n" else " ")
                i += 1
            continue
        if state == "string":
            out.append(ch)
            if ch == "\\" and i + 1 < len(text):
                out.append(text[i + 1])
                i += 2
                continue
            if ch == quote:
                state = "code"
            i += 1
            continue
        if ch in ('"', "'", "`"):
            quote = ch
            state = "string"
            out.append(ch)
            i += 1
        elif ch == "/" and nxt == "/":
            out.extend("  ")
            i += 2
            state = "line"
        elif ch == "/" and nxt == "*":
            out.extend("  ")
            i += 2
            state = "block"
        else:
            out.append(ch)
            i += 1
    return "".join(out)


def line_number(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def fail(errors: list[str], path: Path, text: str, offset: int, message: str) -> None:
    errors.append(f"{relpath(path, ROOT)}:{line_number(text, offset)}: {message}")


def backend_routes(errors: list[str]) -> tuple[set[tuple[str, str]], set[str]]:
    routes: dict[tuple[str, str], list[tuple[Path, int]]] = defaultdict(list)
    forbidden_json_re = re.compile(r'\[\s*"(result|error_code|warning_code|isSuccess|dataJson)"\s*\]')
    for path in sorted(NETWORK.glob("HttpServer*.cpp")):
        raw = path.read_text(encoding="utf-8-sig")
        code = strip_comments(raw)
        for match in ROUTE_RE.finditer(code):
            method = "DELETE" if match.group(1) == "del" else match.group(1).upper()
            route = match.group(2)
            if route.startswith("/api/") and not route.startswith("/api/v1/"):
                fail(errors, path, raw, match.start(), f"public route must use /api/v1: {method} {route}")
            if any(route == bad or route.startswith(bad) for bad in FORBIDDEN_PUBLIC_PATHS):
                fail(errors, path, raw, match.start(), f"forbidden legacy route: {method} {route}")
            routes[(method, route)].append((path, line_number(raw, match.start())))
        for match in forbidden_json_re.finditer(code):
            fail(errors, path, raw, match.start(), f"legacy HTTP JSON field is forbidden: {match.group(1)}")

    for (method, route), locations in sorted(routes.items()):
        if len({path for path, _ in locations}) > 1:
            joined = ", ".join(f"{p.relative_to(ROOT)}:{line}" for p, line in locations)
            errors.append(f"duplicate public route {method} {route}: {joined}")

    command_path = NETWORK / "HttpServer_Command.cpp"
    raw = command_path.read_text(encoding="utf-8-sig")
    code = strip_comments(raw)
    if 'post(modulePath + "/actions/{action}"' not in code:
        errors.append("src/network/HttpServer_Command.cpp: missing canonical module action route")
    modules = set(re.findall(r'registerModuleAction\("([^"]+)"\s*,', code))
    if modules != EXPECTED_ACTION_MODULES:
        errors.append(
            "src/network/HttpServer_Command.cpp: module action registry mismatch; "
            f"expected={sorted(EXPECTED_ACTION_MODULES)}, actual={sorted(modules)}"
        )
    for module in modules:
        routes[("POST", module + "/actions/{action}")].append((command_path, 0))

    helpers_path = NETWORK / "HttpServer_Helpers.cpp"
    helpers_raw = helpers_path.read_text(encoding="utf-8-sig")
    helpers_code = strip_comments(helpers_raw)
    # Frontend apiRequest reserves null for failure, so a successful no-content
    # response must be normalized to a non-null object by the canonical helper.
    for marker in ("data.isNull()", "Json::Value(Json::objectValue)"):
        if marker not in helpers_code:
            errors.append(
                "src/network/HttpServer_Helpers.cpp: successful null data must "
                f"normalize to an object; missing {marker}"
            )
    video_actions = re.search(
        r"videoActions\s*=\s*\{(.*?)\};", helpers_code, re.S
    )
    if not video_actions:
        errors.append("src/network/HttpServer_Helpers.cpp: video action route registry missing")
    else:
        for route in re.findall(r'\{\s*"(/api/v1/[^"]+)"\s*,\s*"[^"]+"\s*\}', video_actions.group(1)):
            routes[("POST", route)].append((helpers_path, 0))
    for field in FORBIDDEN_ACTION_FIELDS:
        if f'"{field}"' not in code:
            errors.append(f"src/network/HttpServer_Command.cpp: missing rejection for body field {field}")
    return set(routes), modules


def check_single_protocol_edges(errors: list[str]) -> None:
    """Check socket/UDP paths that bypass the normal HTTP route helpers."""
    internal_path = NETWORK / "HttpServer_Internal.h"
    internal = strip_comments(internal_path.read_text(encoding="utf-8-sig"))
    if 'response.setJson(hsvj::JsonUtils::toString(root));' not in internal:
        errors.append("src/network/HttpServer_Internal.h: socket errors must use the JSON API envelope")
    if "response.setText(message);" in internal:
        errors.append("src/network/HttpServer_Internal.h: plain-text socket error protocol is forbidden")
    for key in ('root["ok"] = false', 'root["data"] = Json::Value(Json::nullValue)',
                'root["error"]["code"]', 'root["error"]["message"]'):
        if key not in internal:
            errors.append(f"src/network/HttpServer_Internal.h: incomplete socket error envelope: {key}")

    connection_path = NETWORK / "HttpServer_Connection.cpp"
    connection = strip_comments(connection_path.read_text(encoding="utf-8-sig"))
    required_preview_contract = (
        'request.getQueryParam("enabled")',
        'enabledParam != "0" && enabledParam != "1"',
        'request.getQueryParam("playlistId")',
        'if (playlistId.empty())',
    )
    for marker in required_preview_contract:
        if marker not in connection:
            errors.append(f"src/network/HttpServer_Connection.cpp: preview contract check missing: {marker}")
    if "Missing playlistId or layerId parameter" in connection:
        errors.append("src/network/HttpServer_Connection.cpp: layerId-only preview compatibility is forbidden")
    if 'enabledParam == "false"' in connection:
        errors.append("src/network/HttpServer_Connection.cpp: alternate boolean preview protocol is forbidden")

    discovery_paths = (
        ROOT / "src/network/DeviceDiscoveryService.cpp",
        ROOT / "include/network/DeviceDiscoveryService.h",
    )
    for path in discovery_paths:
        raw = path.read_text(encoding="utf-8-sig")
        if "DISCOVER_HVIDEO" in raw:
            errors.append(f"{relpath(path, ROOT)}: legacy text discovery protocol is forbidden")
    discovery = strip_comments(discovery_paths[0].read_text(encoding="utf-8-sig"))
    required_discovery_request_contract = (
        "members.size() != 4",
        'request.isMember("type")',
        'request.isMember("query")',
        'request.isMember("protocol_version")',
        'request.isMember("challenge")',
        'request["type"].asString() == "discover"',
        'request["query"].asString() == "hvideo_player"',
    )
    for marker in required_discovery_request_contract:
        if marker not in discovery:
            errors.append(
                "src/network/DeviceDiscoveryService.cpp: "
                f"discovery request contract check missing: {marker}"
            )


def normalize_frontend_path(value: str, absolute: bool) -> str | None:
    value = value.strip()
    if not value.startswith("/"):
        return None
    value = value.split("#", 1)[0].split("?", 1)[0]
    value = re.sub(r"\$\{[^}]+\}", "{param}", value)
    value = re.sub(r"/+$", "/", value) if value != "/" else value
    if absolute:
        return value
    return "/api/v1" + value


def route_matches(frontend: str, backend: str) -> bool:
    front_parts = frontend.strip("/").split("/")
    back_parts = backend.strip("/").split("/")
    if len(front_parts) != len(back_parts):
        return False
    for front, back in zip(front_parts, back_parts):
        if (front.startswith("{") and front.endswith("}")) or (back.startswith("{") and back.endswith("}")):
            continue
        if front != back:
            return False
    return True


def frontend_calls() -> list[tuple[str, str, Path, int]]:
    calls: list[tuple[str, str, Path, int]] = []
    wrapper_re = re.compile(
        r"\b(apiGet|apiPost|apiPut|apiDelete)\s*\(\s*([`\"'])(.*?)\2", re.S
    )
    fetch_re = re.compile(r"\bfetch\s*\(\s*([`\"'])(/api/v1(?:/.*?)?)\1", re.S)
    methods = {"apiGet": "GET", "apiPost": "POST", "apiPut": "PUT", "apiDelete": "DELETE"}
    for path in sorted(WEB.rglob("*.js")):
        if "node_modules" in path.parts or path.name.endswith(".min.js"):
            continue
        raw = path.read_text(encoding="utf-8-sig")
        code = strip_comments(raw)
        for match in wrapper_re.finditer(code):
            if re.match(r"\s*\+", code[match.end():]):
                continue
            route = normalize_frontend_path(match.group(3), absolute=False)
            if route:
                calls.append((methods[match.group(1)], route, path, line_number(raw, match.start())))
        for match in fetch_re.finditer(code):
            route = normalize_frontend_path(match.group(2), absolute=True)
            if not route or route == "/api/v1/" or re.match(r"\s*\+", code[match.end():]):
                continue
            call_tail = code[match.end():match.end() + 600]
            method_match = re.search(r"\bmethod\s*:\s*['\"](GET|POST|PUT|DELETE)['\"]", call_tail, re.I)
            method = method_match.group(1).upper() if method_match else "GET"
            calls.append((method, route, path, line_number(raw, match.start())))
    return calls


def flutter_calls(errors: list[str]) -> list[tuple[str, str, Path, int]]:
    calls: list[tuple[str, str, Path, int]] = []
    client_call_re = re.compile(
        r"\bclient\.(get|post|put|delete)\s*\(\s*([\"'])(.*?)\2", re.S
    )
    any_client_call_re = re.compile(r"\bclient\.(get|post|put|delete)\s*\(")
    action_re = re.compile(
        r"\bclient\.moduleAction\s*\(\s*([\"'])(.*?)\1\s*,\s*([\"'])(.*?)\3",
        re.S,
    )
    any_action_re = re.compile(r"\bclient\.moduleAction\s*\(")
    methods = {"get": "GET", "post": "POST", "put": "PUT", "delete": "DELETE"}

    for path in sorted(FLUTTER.rglob("*.dart")):
        raw = path.read_text(encoding="utf-8-sig")
        code = strip_comments(raw)
        literal_calls = list(client_call_re.finditer(code))
        if len(literal_calls) != len(list(any_client_call_re.finditer(code))):
            errors.append(
                f"{relpath(path, ROOT)}: every client.get/post/put/delete path "
                "must be a statically checkable string literal"
            )
        for match in literal_calls:
            route = normalize_frontend_path(match.group(3), absolute=False)
            if not route:
                fail(errors, path, raw, match.start(), "Flutter API path must start with /")
                continue
            calls.append(
                (methods[match.group(1)], route, path, line_number(raw, match.start()))
            )

        literal_actions = list(action_re.finditer(code))
        if len(literal_actions) != len(list(any_action_re.finditer(code))):
            errors.append(
                f"{relpath(path, ROOT)}: every client.moduleAction module/action "
                "must be statically checkable string literals"
            )
        for match in literal_actions:
            module, action = match.group(2), match.group(4)
            calls.append(
                (
                    "POST",
                    f"/api/v1/{module}/actions/{action}",
                    path,
                    line_number(raw, match.start()),
                )
            )

    api_client_path = FLUTTER / "data/api_client.dart"
    raw = api_client_path.read_text(encoding="utf-8-sig")
    code = strip_comments(raw)
    # ApiClient's own authentication calls also use literal routes; generic
    # Dio syntax and unqualified wrapper calls bypass the repository matcher.
    internal_call_re = re.compile(
        r"(?<![\w.])(?:_dio\.)?(get|post|put|delete)(?:<[^>]+>)?\s*\(\s*([\"'])(.*?)\2",
        re.S,
    )
    for match in internal_call_re.finditer(code):
        route = normalize_frontend_path(match.group(3), absolute=False)
        if route:
            calls.append((methods[match.group(1)], route, api_client_path,
                          line_number(raw, match.start())))
    thumbnail_re = re.compile(
        r"\bUri\.http\s*\(\s*config\.authority\s*,\s*([\"'])(/api/v1/.*?)\1",
        re.S,
    )
    for match in thumbnail_re.finditer(code):
        route = normalize_frontend_path(match.group(2), absolute=True)
        if route:
            calls.append(("GET", route, api_client_path, line_number(raw, match.start())))
    return calls


def check_calls_have_routes(
    errors: list[str],
    routes: set[tuple[str, str]],
    calls: list[tuple[str, str, Path, int]],
    client_name: str,
) -> None:
    for method, frontend_route, path, line in calls:
        candidates = [backend for backend_method, backend in routes if backend_method == method]
        if not any(route_matches(frontend_route, backend) for backend in candidates):
            errors.append(
                f"{relpath(path, ROOT)}:{line}: {client_name} route has no backend match: "
                f"{method} {frontend_route}"
            )


def check_frontend(
    errors: list[str], routes: set[tuple[str, str]], action_modules: set[str]
) -> None:
    legacy_patterns = [
        (re.compile(r"\bresponse\.result\b"), "response.result compatibility is forbidden"),
        (re.compile(r"\bresult\.result\b"), "result.result compatibility is forbidden"),
        (re.compile(r"\.dataJson\b"), "dataJson compatibility is forbidden"),
        (re.compile(r"\.isSuccess\b"), "isSuccess compatibility is forbidden"),
        (re.compile(r"\b(?:apiActionWithError|apiPostWithError)\b"), "secondary frontend response protocol is forbidden"),
        (re.compile(r"\bapiAction\s*\(\s*(?:0x[0-9a-fA-F]+|\d+|[^,\n]+\.code)"), "numeric command code passed to apiAction"),
    ]
    absolute_api_re = re.compile(r'(?:"|\'|`)(/api/[^"\'`\s?]*)')
    for path in sorted(WEB.rglob("*.js")):
        if "node_modules" in path.parts or path.name.endswith(".min.js"):
            continue
        raw = path.read_text(encoding="utf-8-sig")
        code = strip_comments(raw)
        for bad in FORBIDDEN_PUBLIC_PATHS:
            for match in re.finditer(re.escape(bad), code):
                fail(errors, path, raw, match.start(), f"forbidden legacy API path: {bad}")
        for pattern, message in legacy_patterns:
            for match in pattern.finditer(code):
                fail(errors, path, raw, match.start(), message)
        for match in absolute_api_re.finditer(code):
            route = match.group(1)
            if route != "/api/v1" and not route.startswith("/api/v1/"):
                fail(errors, path, raw, match.start(1), f"absolute API path must use /api/v1: {route}")

    api_client = (WEB / "shared/apiClient.js").read_text(encoding="utf-8-sig")
    fusion_api = (WEB / "fusion-mobile/modules/api.js").read_text(encoding="utf-8-sig")
    if "const API_BASE = '/api/v1';" not in api_client:
        errors.append("shared API base must be exactly /api/v1")
    for relative in ("modules/core/api.js", "mobile/modules/core/api.js"):
        source = (WEB / relative).read_text(encoding="utf-8-sig")
        if "from '/shared/apiClient.js'" not in source:
            errors.append(f"{relative}: must use the shared API client")
    if "const API_BASE = '/api/v1';" not in fusion_api:
        errors.append("fusion-mobile API base must be exactly /api/v1")

    actions = (WEB / "shared/moduleActions.js").read_text(encoding="utf-8-sig")
    if "['type', 'code', 'param', 'action']" not in actions:
        errors.append("shared/moduleActions.js must reject type/code/param/action")
    expected_action_path = "/${moduleName}/actions/${encodeURIComponent(action)}"
    if expected_action_path not in actions:
        errors.append("shared/moduleActions.js must build /{module}/actions/{action}")
    api_module_block = re.search(
        r"export\s+const\s+ApiModule\s*=\s*Object\.freeze\s*\(\s*\{(.*?)\}\s*\)",
        strip_comments(actions),
        re.S,
    )
    web_modules = (
        set(re.findall(r":\s*['\"]([^'\"]+)['\"]", api_module_block.group(1)))
        if api_module_block
        else set()
    )
    backend_module_names = {path.rsplit("/", 1)[-1] for path in action_modules}
    if web_modules != backend_module_names:
        errors.append(
            "shared/moduleActions.js: module whitelist mismatch; "
            f"backend={sorted(backend_module_names)}, web={sorted(web_modules)}"
        )

    parser = (WEB / "shared/apiResponseParser.js").read_text(encoding="utf-8-sig")
    if "hasExactKeys(response, ['ok', 'data', 'error'])" not in parser:
        errors.append("shared/apiResponseParser.js must enforce exact ok/data/error keys")
    if "hasExactKeys(response.error, ['code', 'message'])" not in parser:
        errors.append("shared/apiResponseParser.js must enforce exact error code/message keys")

    mobile_video_path = WEB / "mobile/modules/pages/mobileVideoControl.js"
    mobile_video = strip_comments(mobile_video_path.read_text(encoding="utf-8-sig"))
    for marker in ("createResult.playlistId", "createResult.itemCount"):
        if marker in mobile_video:
            errors.append(f"{relpath(mobile_video_path, ROOT)}: legacy playlist response field is forbidden: {marker}")

    peripheral_path = WEB / "modules/pages/peripheralManagement.js"
    peripheral = strip_comments(peripheral_path.read_text(encoding="utf-8-sig"))
    for marker in ("else if (m.action)", "m.action = f.action"):
        if marker in peripheral:
            errors.append(f"{relpath(peripheral_path, ROOT)}: legacy network mapping schema is forbidden: {marker}")

    check_calls_have_routes(errors, routes, frontend_calls(), "Web")


def check_flutter(
    errors: list[str], routes: set[tuple[str, str]], action_modules: set[str]
) -> list[tuple[str, str, Path, int]]:
    config_path = FLUTTER / "core/app_config.dart"
    config = strip_comments(config_path.read_text(encoding="utf-8-sig"))
    required_config = (
        "static const int port = 8081;",
        "String get authority => '$serverIp:$port';",
        "String get origin => 'http://$authority';",
        "String get baseUrl => '$origin/api/v1';",
    )
    for marker in required_config:
        if marker not in config:
            errors.append(f"{relpath(config_path, ROOT)}: missing fixed API config: {marker}")

    api_client_path = FLUTTER / "data/api_client.dart"
    api_client = strip_comments(api_client_path.read_text(encoding="utf-8-sig"))
    if not re.search(r"BaseOptions\s*\(\s*baseUrl:\s*config\.baseUrl\b", api_client, re.S):
        errors.append(
            f"{relpath(api_client_path, ROOT)}: Dio BaseOptions must use config.baseUrl"
        )

    actions_path = FLUTTER / "data/module_actions.dart"
    actions = strip_comments(actions_path.read_text(encoding="utf-8-sig"))
    module_block = re.search(
        r"static\s+const\s+modules\s*=\s*<String>\s*\{(.*?)\};", actions, re.S
    )
    flutter_modules = (
        set(re.findall(r"['\"]([^'\"]+)['\"]", module_block.group(1)))
        if module_block
        else set()
    )
    backend_module_names = {path.rsplit("/", 1)[-1] for path in action_modules}
    if flutter_modules != backend_module_names:
        errors.append(
            f"{relpath(actions_path, ROOT)}: module whitelist mismatch; "
            f"backend={sorted(backend_module_names)}, flutter={sorted(flutter_modules)}"
        )
    required_action_markers = (
        "static const legacyFields = <String>{'type', 'code', 'param', 'action'};",
        "path: '/$module/actions/${Uri.encodeComponent(action)}',",
    )
    for marker in required_action_markers:
        if marker not in actions:
            errors.append(
                f"{relpath(actions_path, ROOT)}: incomplete module action contract: {marker}"
            )

    calls = flutter_calls(errors)
    check_calls_have_routes(errors, routes, calls, "Flutter")
    return calls


def check_package(errors: list[str]) -> None:
    package_path = ROOT / "package.json"
    if not package_path.exists():
        # The native Android app packages the static Web tree directly through Gradle.
        gradle = (ROOT / "app/build.gradle").read_text(encoding="utf-8-sig")
        if "from('../src/network/web')" not in gradle:
            errors.append("app/build.gradle: missing canonical static Web asset source")
        return
    package = json.loads(package_path.read_text(encoding="utf-8-sig"))
    scripts = package.get("scripts", {})
    if scripts.get("check:api-contract") != "python scripts/check_api_contract.py":
        errors.append("package.json: missing canonical check:api-contract script")
    forbidden_build_tokens = ("webpack", "post-build.js", "shared/App.js")
    serialized = json.dumps(package)
    for token in forbidden_build_tokens:
        if token in serialized:
            errors.append(f"package.json: stale build declaration is forbidden: {token}")


def check_online_vod_admission(errors: list[str]) -> None:
    path = ROOT / "src/core/Engine.cpp"
    code = strip_comments(path.read_text(encoding="utf-8-sig"))
    start = code.find("OnlineVodAdmissionResult requestOnlineVodAdmission(")
    end = code.find("\nbool reportDeviceToLicenseServer(", start)
    if start < 0 or end < 0:
        errors.append("src/core/Engine.cpp: Online VOD admission implementation not found")
        return
    admission = code[start:end]
    # HVideo server TerminalAdmissionRequest rejects every unknown JSON field.
    expected = {
        "protocolVersion", "deviceRole", "appId", "deviceName", "macAddress",
        "serial", "model", "softwareVersion", "httpPort", "network",
    }
    actual = set(re.findall(r'request\["([^"\n]+)"\]\s*=', admission))
    if actual != expected:
        errors.append(
            "src/core/Engine.cpp: terminal admission fields differ from server contract; "
            f"missing={sorted(expected - actual)}, unexpected={sorted(actual - expected)}"
        )
    if '"/api/v1/terminals/admission"' not in admission:
        errors.append("src/core/Engine.cpp: missing canonical terminal admission endpoint")
    system = strip_comments((ROOT / "src/utils/SystemUtils.cpp").read_text(encoding="utf-8-sig"))
    start = system.find("Json::Value SystemUtils::getNetworkReport(")
    end = system.find("std::string SystemUtils::getDeviceName(", start)
    network_fields = set(re.findall(r'report\["([^"\n]+)"\]\s*=', system[start:end]))
    if network_fields != {"networkType", "interfaceName", "macAddress"}:
        errors.append("SystemUtils network report must match TerminalNetworkReport fields exactly")


def check_qr_vod_config_response(errors: list[str]) -> None:
    path = ROOT / "src/core/CommandRouter_System.cpp"
    code = strip_comments(path.read_text(encoding="utf-8-sig"))
    start = code.find('param["action"].asString() == "status"')
    end = code.find("return response;", start)
    status = code[start:end] if start >= 0 and end >= 0 else ""
    # Both QR dashboards consume this response, not the persisted config.json.
    fields = {
        "vodMode": "getVodMode",
        "enableVod": "isVodEnabled",
        "onlineVodHost": "getOnlineVodHost",
        "onlineVodRoomId": "getOnlineVodRoomId",
    }
    for field, getter in fields.items():
        pattern = rf'data\["{field}"\]\s*=\s*systemConfig_->{getter}\(\)\s*;'
        if not re.search(pattern, status):
            errors.append(
                f"{relpath(path, ROOT)}: system/config response must expose "
                f"{field} from runtime config ({getter})"
            )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--player-only", action="store_true",
                        help="validate a standalone Hplayer checkout without the sibling Flutter project")
    args = parser.parse_args()
    errors: list[str] = []
    routes, action_modules = backend_routes(errors)
    check_single_protocol_edges(errors)
    check_frontend(errors, routes, action_modules)
    dart_calls = []
    if not args.player_only:
        if FLUTTER.is_dir():
            dart_calls = check_flutter(errors, routes, action_modules)
        else:
            errors.append(f"Flutter source directory missing: {FLUTTER} (use --player-only for Hplayer CI)")
    check_package(errors)
    check_online_vod_admission(errors)
    check_qr_vod_config_response(errors)
    peripheral = (ROOT / 'src/core/PeripheralManager.cpp').read_text(encoding='utf-8-sig')
    if peripheral.count('action == "import_templates"') != 1:
        errors.append('PeripheralManager: expected exactly one import_templates handler')
    relative = 'modules/pages/peripheralManagement.js'
    source = (WEB / relative).read_text(encoding='utf-8-sig')
    if "apiAction('peripherals', 'import_templates'," not in source or 'initSerialTemplateImport({' not in source:
        errors.append(relative + ': missing canonical serial import action or shared UI')
    if errors:
        print("API contract check failed:")
        for error in errors:
            print(f"  - {error}")
        return 1
    client_counts = f"{len(frontend_calls())} Web"
    client_counts += " (standalone Hplayer)" if args.player_only else f" + {len(dart_calls)} Flutter"
    print(
        f"API contract check passed: {len(routes)} backend method/routes and "
        f"{client_counts} literal calls "
        "are one-to-one compatible."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
