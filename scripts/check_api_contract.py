#!/usr/bin/env python3
"""Static checks for the single public HTTP API contract."""

from __future__ import annotations

import re
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NETWORK = ROOT / "src" / "network"
WEB = NETWORK / "web"
EXPECTED_ACTION_MODULES = {
    "/api/v1/system-config", "/api/v1/layers", "/api/v1/playback",
    "/api/v1/rendering", "/api/v1/sync", "/api/v1/playlists",
    "/api/v1/scenes", "/api/v1/regions", "/api/v1/lyrics",
    "/api/v1/system", "/api/v1/peripherals", "/api/v1/peripheral-events",
}
FORBIDDEN_PUBLIC_PATHS = ("/api/command", "/api/v1/command", "/api/v1/playlist/")
FORBIDDEN_ACTION_FIELDS = ("type", "code", "param", "action")


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
        if ch in ('"', "'", chr(96)):
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
    errors.append(f"{path.relative_to(ROOT)}:{line_number(text, offset)}: {message}")


def check_backend(errors: list[str]) -> None:
    routes: dict[tuple[str, str], list[tuple[Path, int]]] = defaultdict(list)
    route_re = re.compile(r'(?m)^\s*(get|post|put|del)\s*\(\s*"([^"]+)"')
    forbidden_json_re = re.compile(r'\[\s*"(result|error_code|warning_code|isSuccess|dataJson)"\s*\]')
    for path in sorted(NETWORK.glob("HttpServer*.cpp")):
        raw = path.read_text(encoding="utf-8-sig")
        code = strip_comments(raw)
        for match in route_re.finditer(code):
            method, route = match.group(1).upper(), match.group(2)
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
        errors.append("src/network/HttpServer_Command.cpp: module action registry mismatch; " +
                      f"expected={sorted(EXPECTED_ACTION_MODULES)}, actual={sorted(modules)}")
    for field in FORBIDDEN_ACTION_FIELDS:
        if f'"{field}"' not in code:
            errors.append(f"src/network/HttpServer_Command.cpp: missing rejection for body field {field}")


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
        ROOT / "docs/U盘播放与手机控制使用说明.md",
    )
    for path in discovery_paths:
        raw = path.read_text(encoding="utf-8-sig")
        if "DISCOVER_HVIDEO" in raw:
            errors.append(f"{path.relative_to(ROOT)}: legacy text discovery protocol is forbidden")
    discovery = strip_comments(discovery_paths[0].read_text(encoding="utf-8-sig"))
    if "members.size() != 2" not in discovery:
        errors.append("src/network/DeviceDiscoveryService.cpp: discovery request must enforce exact keys")


def check_frontend(errors: list[str]) -> None:
    legacy_patterns = [
        (re.compile(r"\bresponse\.result\b"), "response.result compatibility is forbidden"),
        (re.compile(r"\bresult\.result\b"), "result.result compatibility is forbidden"),
        (re.compile(r"\.dataJson\b"), "dataJson compatibility is forbidden"),
        (re.compile(r"\.isSuccess\b"), "isSuccess compatibility is forbidden"),
        (re.compile(r"\b(?:apiActionWithError|apiPostWithError)\b"), "secondary frontend response protocol is forbidden"),
        (re.compile(r"\bapiAction\s*\(\s*(?:0x[0-9a-fA-F]+|\d+|[^,\n]+\.code)"), "numeric command code passed to apiAction"),
    ]
    absolute_api_re = re.compile(r'(?:"|\'|\x60)(/api/[^"\'\x60\s?]*)')
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

    desktop_api = (WEB / "modules/core/api.js").read_text(encoding="utf-8-sig")
    mobile_api = (WEB / "mobile/modules/core/api.js").read_text(encoding="utf-8-sig")
    fusion_api = (WEB / "fusion-mobile/modules/api.js").read_text(encoding="utf-8-sig")
    if "window.apiBaseUrl = '/api/v1';" not in desktop_api:
        errors.append("desktop API base must be exactly /api/v1")
    if "window.apiBaseUrl = '/api/v1';" not in mobile_api:
        errors.append("mobile API base must be exactly /api/v1")
    if "const API_BASE = '/api/v1';" not in fusion_api:
        errors.append("fusion-mobile API base must be exactly /api/v1")

    actions = (WEB / "shared/moduleActions.js").read_text(encoding="utf-8-sig")
    if "['type', 'code', 'param', 'action']" not in actions:
        errors.append("shared/moduleActions.js must reject type/code/param/action")
    expected_action_path = '/' + '$' + '{moduleName}/actions/' + '$' + '{encodeURIComponent(action)}'
    if expected_action_path not in actions:
        errors.append("shared/moduleActions.js must build /{module}/actions/{action}")

    parser = (WEB / "shared/apiResponseParser.js").read_text(encoding="utf-8-sig")
    if "hasExactKeys(response, ['ok', 'data', 'error'])" not in parser:
        errors.append("shared/apiResponseParser.js must enforce exact ok/data/error keys")
    if "hasExactKeys(response.error, ['code', 'message'])" not in parser:
        errors.append("shared/apiResponseParser.js must enforce exact error code/message keys")

    mobile_video_path = WEB / "mobile/modules/pages/mobileVideoControl.js"
    mobile_video = strip_comments(mobile_video_path.read_text(encoding="utf-8-sig"))
    for marker in ("createResult.playlistId", "createResult.itemCount"):
        if marker in mobile_video:
            errors.append(f"{mobile_video_path.relative_to(ROOT)}: legacy playlist response field is forbidden: {marker}")

    peripheral_path = WEB / "modules/pages/peripheralManagement.js"
    peripheral = strip_comments(peripheral_path.read_text(encoding="utf-8-sig"))
    for marker in ("else if (m.action)", "m.action = f.action"):
        if marker in peripheral:
            errors.append(f"{peripheral_path.relative_to(ROOT)}: legacy network mapping schema is forbidden: {marker}")


def check_package(errors: list[str]) -> None:
    package = (ROOT / "package.json").read_text(encoding="utf-8-sig")
    if '"check:api-contract": "python scripts/check_api_contract.py"' not in package:
        errors.append("package.json: missing check:api-contract script")


def main() -> int:
    errors: list[str] = []
    check_backend(errors)
    check_single_protocol_edges(errors)
    check_frontend(errors)
    check_package(errors)
    if errors:
        print("API contract check failed:")
        for error in errors:
            print(f"  - {error}")
        return 1
    print("API contract check passed: /api/v1, one protocol per endpoint, module actions, and strict envelopes are consistent.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
