#!/usr/bin/env python3
"""Generate a traceable feature/API inventory from the Hplayer source tree."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
from typing import Iterable


MODULE_RE = re.compile(r"^\s*([A-Z][A-Z0-9_]*)\s*:\s*['\"]([^'\"]+)['\"]", re.MULTILINE)
DIRECT_ACTION_RE = re.compile(
    r"apiAction\(\s*['\"]([^'\"]+)['\"]\s*,\s*['\"]([^'\"]+)['\"]"
)
CONSTANT_ACTION_RE = re.compile(
    r"apiAction\(\s*ApiModule\.([A-Z][A-Z0-9_]*)\s*,\s*['\"]([^'\"]+)['\"]"
)
ROUTE_RE = re.compile(
    r"\b(?:get|post|put|patch|del|delete)\s*\(\s*['\"]([^'\"]*?/api/v1/[^'\"]*)['\"]"
)
REGISTERED_ROUTE_RE = re.compile(r'registerModuleAction\("/api/v1/([^"/]+)"')
BACKEND_ACTION_RE = re.compile(r'\baction\s*==\s*"([A-Za-z0-9_-]+)"')

BACKEND_MODULES = {
    "CommandRouter_LayerManagement.cpp": "layers",
    "CommandRouter_LayerRender.cpp": "rendering",
    "CommandRouter_Lyric.cpp": "lyrics",
    "CommandRouter_Peripheral.cpp": "peripherals",
    "CommandRouter_Playlist.cpp": "playlists",
    "CommandRouter_RegionConfig.cpp": "regions",
    "CommandRouter_Scene.cpp": "scenes",
    "CommandRouter_StageControl.cpp": "stage-control",
    "CommandRouter_Sync.cpp": "sync",
    "CommandRouter_System.cpp": "system",
    "CommandRouter_VideoPlayback.cpp": "playback",
    "StageControlManager.cpp": "stage-control",
    "PeripheralManager.cpp": "peripherals",
}


def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        return path.read_text(encoding="utf-8", errors="replace")


def source_files(root: Path, suffixes: Iterable[str]) -> list[Path]:
    excluded = {".git", ".gradle", "build", "node_modules", "artifacts", "backups"}
    return [
        p for p in root.rglob("*")
        if p.is_file() and p.suffix in suffixes and not excluded.intersection(p.parts)
    ]


def rel(path: Path, root: Path) -> str:
    return path.relative_to(root).as_posix()


def add_source(item: dict, source: str) -> None:
    if source not in item["sources"]:
        item["sources"].append(source)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--json", type=Path)
    parser.add_argument("--markdown", type=Path)
    args = parser.parse_args()
    root = args.root.resolve()
    output_json = args.json or root / "tests" / "catalog" / "function_catalog.json"
    output_md = args.markdown or root / "tests" / "catalog" / "function_catalog.md"

    js_files = source_files(root / "src" / "network" / "web", {".js"})
    cpp_files = source_files(root / "src", {".cpp", ".h"})

    modules: dict[str, dict] = {}
    shared_actions = root / "src" / "network" / "web" / "shared" / "moduleActions.js"
    if shared_actions.exists():
        for constant, module in dict(MODULE_RE.findall(read_text(shared_actions))).items():
            modules[module] = {
                "module": module,
                "constant": constant,
                "actions": {},
                "status": "cataloged",
                "test_status": "planned",
            }

    for path in js_files:
        text = read_text(path)
        source = rel(path, root)
        matches = [(module, action) for module, action in DIRECT_ACTION_RE.findall(text)]
        for constant, action in CONSTANT_ACTION_RE.findall(text):
            module = next(
                (item["module"] for item in modules.values() if item["constant"] == constant),
                constant,
            )
            matches.append((module, action))
        for module, action in matches:
            entry = modules.setdefault(
                module,
                {
                    "module": module,
                    "constant": None,
                    "actions": {},
                    "status": "discovered_outside_registry",
                    "test_status": "planned",
                },
            )
            action_entry = entry["actions"].setdefault(
                action, {"action": action, "sources": [], "test_status": "planned"}
            )
            add_source(action_entry, source)

    registered_modules = set()
    special_routes: dict[str, dict] = {}
    for path in cpp_files:
        text = read_text(path)
        source = rel(path, root)
        registered_modules.update(REGISTERED_ROUTE_RE.findall(text))
        module = BACKEND_MODULES.get(path.name)
        if module:
            entry = modules.setdefault(
                module,
                {
                    "module": module,
                    "constant": None,
                    "actions": {},
                    "status": "backend_registered",
                    "test_status": "planned",
                },
            )
            for action in BACKEND_ACTION_RE.findall(text):
                action_entry = entry["actions"].setdefault(
                    action, {"action": action, "sources": [], "test_status": "planned"}
                )
                add_source(action_entry, source)
        for route in ROUTE_RE.findall(text):
            special_routes.setdefault(
                route, {"route": route, "sources": [], "test_status": "planned"}
            )["sources"].append(source)

    for module in sorted(registered_modules):
        modules.setdefault(
            module,
            {
                "module": module,
                "constant": None,
                "actions": {},
                "status": "backend_registered",
                "test_status": "planned",
            },
        )

    pages = []
    for base in (
        root / "src" / "network" / "web" / "modules" / "pages",
        root / "src" / "network" / "web" / "mobile" / "modules" / "pages",
    ):
        if base.exists():
            pages.extend(
                {
                    "name": path.stem,
                    "surface": "mobile" if "mobile" in path.parts else "desktop",
                    "source": rel(path, root),
                    "test_status": "planned",
                }
                for path in sorted(base.glob("*.js"))
            )

    features = []
    for base in (
        root / "src" / "network" / "web" / "modules" / "features",
        root / "src" / "network" / "web" / "mobile" / "modules" / "features",
    ):
        if base.exists():
            features.extend(
                {
                    "name": path.stem,
                    "surface": "mobile" if "mobile" in path.parts else "desktop",
                    "source": rel(path, root),
                    "test_status": "planned",
                }
                for path in sorted(base.glob("*.js"))
            )

    native_domains = []
    for base in (root / "src", root / "include"):
        if base.exists():
            native_domains.extend(
                {
                    "name": path.name,
                    "source": rel(path, root),
                    "test_status": "planned",
                }
                for path in sorted(base.iterdir())
                if path.is_dir()
            )
    native_domains = sorted(
        {item["name"]: item for item in native_domains}.values(),
        key=lambda item: item["name"],
    )

    for entry in modules.values():
        entry["actions"] = [entry["actions"][name] for name in sorted(entry["actions"])]
        for action in entry["actions"]:
            action["sources"] = sorted(set(action["sources"]))
    special_routes = sorted(special_routes.values(), key=lambda item: item["route"])

    catalog = {
        "schema_version": 1,
        "generated_from": "source discovery; planned items require executable cases",
        "api_modules": [modules[name] for name in sorted(modules)],
        "ui_pages": sorted(pages, key=lambda item: (item["surface"], item["name"], item["source"])),
        "feature_files": sorted(features, key=lambda item: (item["surface"], item["name"], item["source"])),
        "native_domains": native_domains,
        "special_routes": special_routes,
        "coverage_gaps": [
            "Actions built dynamically at runtime need explicit cases or a backend action manifest.",
            "HTTP routes outside /api/v1 and WebSocket/event payloads need protocol-specific cases.",
            "Hardware-only behavior needs a real hw81/hw82 device profile and evidence collector.",
            "Server and Flutter Pad flows need external project adapters and environment fixtures.",
        ],
    }
    output_json.parent.mkdir(parents=True, exist_ok=True)
    output_json.write_text(json.dumps(catalog, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    lines = [
        "# Hplayer 功能覆盖清单",
        "",
        "> 本清单由 tests/catalog/generate_catalog.py 从源码生成。planned 只表示已发现，只有执行过测试用例后才能改为 passed。",
        "",
        "## API 模块",
        "",
        "| 模块 | 发现动作 | 测试状态 | 来源 |",
        "| --- | --- | --- | --- |",
    ]
    for module in catalog["api_modules"]:
        actions = ", ".join(action["action"] for action in module["actions"]) or "动态动作/待补充"
        sources = ", ".join(sorted({source for action in module["actions"] for source in action["sources"]})) or "后端注册"
        lines.append(f"| {module['module']} | {actions} | {module['test_status']} | {sources} |")
    lines.extend(["", "## 页面和功能文件", "", "| 类型 | 名称 | 页面 | 测试状态 |", "| --- | --- | --- | --- |"])
    for item in catalog["ui_pages"] + catalog["feature_files"]:
        lines.append(f"| UI | {item['name']} | {item['surface']} | {item['test_status']} |")
    lines.extend(["", "## 原生功能域", "", "| 功能域 | 测试状态 |", "| --- | --- |"])
    for item in catalog["native_domains"]:
        lines.append(f"| {item['name']} | {item['test_status']} |")
    lines.extend(["", "## 特殊路由", "", "| 路由 | 测试状态 |", "| --- | --- |"])
    for item in catalog["special_routes"]:
        lines.append(f"| {item['route']} | {item['test_status']} |")
    lines.extend(["", "## 覆盖边界", ""])
    lines.extend(f"- {gap}" for gap in catalog["coverage_gaps"])
    output_md.parent.mkdir(parents=True, exist_ok=True)
    output_md.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"Generated {output_json}")
    print(f"Generated {output_md}")
    print(
        f"API modules: {len(catalog['api_modules'])}; "
        f"UI pages: {len(catalog['ui_pages'])}; "
        f"native domains: {len(catalog['native_domains'])}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
