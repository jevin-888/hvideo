#!/usr/bin/env python3
"""Enforce native module layering and declared CMake dependency contracts."""

from __future__ import annotations

import re
import sys
import xml.etree.ElementTree as ET
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
INCLUDE = ROOT / "include"
MODULE_TARGETS = {
    "audio": "hsvj_audio",
    "capture": "hsvj_capture",
    "core": "hsvj_core",
    "database": "hsvj_database",
    "decoder": "hsvj_decoder",
    "effect": "hsvj_effect",
    "foundation": "hsvj_foundation",
    "fusion": "hsvj_fusion",
    "layer": "hsvj_layer",
    "lyric": "hsvj_lyric",
    "network": "hsvj_network",
    "playcontrol": "hsvj_playcontrol",
    "renderer": "hsvj_renderer",
    "text": "hsvj_text",
    "utils": "hsvj_utils",
    "vod": "hsvj_local_vod",
}
CPP_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}
LEGACY_PATHS = (
    INCLUDE / "core/PathConfig.h",
    INCLUDE / "core/Resolution.h",
    INCLUDE / "utils/SliceConfigJson.h",
    SRC / "core/PathConfig.cpp",
    SRC / "utils/SliceConfigJson.cpp",
)
RETIRED_RKNN_PATHS = (
    ROOT / "scripts/convert_onnx_to_rknn.py",
    ROOT / "scripts/convert_onnx_to_rknn_rk3568.py",
    ROOT / "scripts/convert_vocal_remover_rknn.py",
    ROOT / "scripts/convert_wavebeat.py",
    ROOT / "scripts/convert_wavebeat_light.py",
    ROOT / "scripts/convert_wavebeat_simple.py",
    ROOT / "scripts/deploy_rknn_from_toolkit.ps1",
    ROOT / "scripts/export_vocal_remover.py",
    ROOT / "scripts/export_wavebeat_rk3568.py",
    ROOT / "scripts/fetch_rknn_runtime.ps1",
    ROOT / "scripts/setup_rknn_sdk.ps1",
    ROOT / "scripts/vocal_remover_rk3568.onnx",
    ROOT / "scripts/vocal_remover_rk3588.onnx",
    ROOT / "scripts/wavebeat.onnx",
    ROOT / "scripts/wavebeat_calib.npy",
    ROOT / "scripts/wavebeat_dataset.txt",
    ROOT / "scripts/wavebeat_rk3568.onnx",
    ROOT / "docs/\u9f13\u70b9\u68c0\u6d4b\u4e0e\u5f3a\u62cd\u8bf4\u660e.md",
)
RETIRED_TEST_PATHS = (
    ROOT / "Fultter_pad/test",
    ROOT / "vendor/ffmpeg-rockchip/libavformat/fifo_test.c",
    ROOT / "vendor/ffmpeg-rockchip/tools/enc_recon_frame_test.c",
    ROOT / "vendor/ffmpeg-rockchip/tools/scale_slice_test.c",
    ROOT / "vendor/jsoncpp/src/jsontestrunner",
    ROOT / "vendor/jsoncpp/src/test_lib_json",
    ROOT / "vendor/jsoncpp/test",
    ROOT / "vendor/libyuv/unit_test",
    ROOT / "vendor/libyuv/tools_libyuv/autoroller/unittests",
    ROOT / "vendor/rkmpp/tools/resolution_test.sh",
    ROOT / "vendor/rkmpp/utils/osd3_test.c",
    ROOT / "vendor/rkmpp/utils/osd3_test.h",
    ROOT / "vendor/LibJpeg-Turbo-Android-master/app/src/androidTest",
    ROOT / "vendor/LibJpeg-Turbo-Android-master/turbojpeg/src/androidTest",
    ROOT / "vendor/LibJpeg-Turbo-Android-master/turbojpeg/src/main/cpp/java/TJUnitTest.java",
)


def source_files(module: str):
    roots = [SRC / module, INCLUDE / module]
    for base in roots:
        if not base.exists():
            continue
        for path in base.rglob("*"):
            if path.is_file() and path.suffix.lower() in CPP_SUFFIXES:
                yield path


def project_dependency(include_name: str) -> str | None:
    first = include_name.replace("\\", "/").split("/", 1)[0]
    if first in MODULE_TARGETS:
        return first
    if first == "json":
        return "jsoncpp"
    return None


def actual_dependencies(module: str) -> tuple[set[str], dict[str, list[str]]]:
    dependencies: set[str] = set()
    locations: dict[str, list[str]] = defaultdict(list)
    include_re = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.M)
    for path in source_files(module):
        text = path.read_text(encoding="utf-8-sig", errors="replace")
        for match in include_re.finditer(text):
            dependency = project_dependency(match.group(1))
            if dependency is None or dependency == module:
                continue
            dependencies.add(dependency)
            line = text.count("\n", 0, match.start()) + 1
            locations[dependency].append(f"{path.relative_to(ROOT)}:{line}")
    return dependencies, locations


def declared_contract(module: str) -> set[str] | None:
    cmake_path = SRC / module / "CMakeLists.txt"
    if not cmake_path.exists():
        return None
    text = cmake_path.read_text(encoding="utf-8-sig")
    marker = "Architecture dependency contract"
    marker_at = text.find(marker)
    if marker_at < 0:
        return None
    if module == "foundation":
        return set()
    tail = text[marker_at:]
    target = MODULE_TARGETS[module]
    match = re.search(
        rf"target_link_libraries\s*\(\s*{re.escape(target)}\b(.*?)\)", tail, re.S
    )
    if not match:
        return None
    tokens = re.findall(r"\bhsvj_[A-Za-z0-9_]+\b", match.group(1))
    return set(tokens)



def check_shader_pipeline(errors: list[str]) -> None:
    source_shader_dir = ROOT / "shaders"
    source_asset_dir = ROOT / "app/src/main/assets/shaders"
    tracked_artifacts = [
        *source_shader_dir.rglob("*.spv"),
        *(source_asset_dir.rglob("*.spv") if source_asset_dir.exists() else []),
    ]
    generated_headers = [
        source_shader_dir / "shader_spirv.h",
        source_shader_dir / "effect_spirv.h",
        source_shader_dir / "capture_texture_frag.h",
        INCLUDE / "renderer/effect_color_frag_spv.h",
        INCLUDE / "renderer/effect_color_vert_spv.h",
    ]
    if tracked_artifacts:
        errors.append(
            "SPIR-V must be generated under app/build, not stored in source/assets: "
            + ", ".join(str(path.relative_to(ROOT)) for path in tracked_artifacts)
        )
    for path in generated_headers:
        if path.exists():
            errors.append(f"embedded/generated shader header is forbidden: {path.relative_to(ROOT)}")

    gradle = (ROOT / "app/build.gradle").read_text(encoding="utf-8-sig")
    if "compiledShaderDir = layout.buildDirectory.dir('generated/shaders')" not in gradle:
        errors.append("Gradle shader output must be app/build/generated/shaders")
    if "rootProject.file('shaders')" not in gradle or "compiler.absolutePath" not in gradle:
        errors.append("Gradle compileAppShaders must compile the production shaders directory")
    if "layout.buildDirectory.dir('generated/assets/shaders')" not in gradle:
        errors.append("Gradle generated shader asset source is missing")
    packages_from_compiled = "from(compiledShaderDir)" in gradle
    nested_shader_destination = (
        "layout.buildDirectory.dir('generated/assets/shaders')" in gradle
        and "into generatedShaderAssetDir" in gradle
    )
    rooted_shader_destination = (
        "layout.buildDirectory.dir('generated/assets')" in gradle
        and "into 'shaders'" in gradle
    )
    if not packages_from_compiled or not (nested_shader_destination or rooted_shader_destination):
        errors.append("Gradle copyShaders must package generated SPIR-V under assets/shaders")

    compiled_sources = {
        path.relative_to(ROOT).as_posix()
        for path in source_shader_dir.rglob("*") if path.suffix in {".frag", ".vert"}
    }
    for relative in sorted(compiled_sources):
        if not (ROOT / relative).exists():
            errors.append(f"Gradle shader source does not exist: {relative}")

    runtime_names: set[str] = set()
    runtime_re = re.compile(r'["\']([^"\']+\.(?:frag|vert|comp)\.spv)["\']')
    for path in (SRC / "renderer").rglob("*.cpp"):
        runtime_names.update(runtime_re.findall(path.read_text(encoding="utf-8-sig", errors="replace")))
    compiled_names = {
        relative.removeprefix("shaders/") + ".spv" for relative in compiled_sources
    }
    missing_runtime = sorted(runtime_names - compiled_names)
    if missing_runtime:
        errors.append(f"runtime shader assets missing from Gradle compilation inputs: {missing_runtime}")

def check_android_architecture(errors: list[str]) -> None:
    manifest_path = ROOT / "app/src/main/AndroidManifest.xml"
    android_name = "{http://schemas.android.com/apk/res/android}name"
    android_exported = "{http://schemas.android.com/apk/res/android}exported"
    try:
        root = ET.parse(manifest_path).getroot()
    except (OSError, ET.ParseError) as error:
        errors.append(f"cannot parse AndroidManifest.xml: {error}")
        return

    drm_services = [
        service
        for service in root.findall("./application/service")
        if service.get(android_name, "").endswith(".DrmKmsRenderService")
    ]
    if len(drm_services) != 1:
        errors.append("AndroidManifest.xml must declare exactly one DrmKmsRenderService")
    elif drm_services[0].get(android_exported) != "false":
        errors.append("DrmKmsRenderService must remain android:exported=\"false\"")

    java_root = ROOT / "app/src/main/java/com/hsvj/engine"
    main_activity = (java_root / "MainActivity.java").read_text(encoding="utf-8-sig")
    hsvj_engine = (java_root / "HSVJEngine.java").read_text(encoding="utf-8-sig")
    drm_service = (java_root / "DrmKmsRenderService.java").read_text(encoding="utf-8-sig")
    gateway = java_root / "PlatformControlGateway.java"
    coordinator = java_root / "AppUpdateCoordinator.java"

    if not gateway.exists():
        errors.append("PlatformControlGateway.java is required for process-safe platform operations")
    if not coordinator.exists():
        errors.append("AppUpdateCoordinator.java is required to keep update UI out of MainActivity")
    if "AppUpdateManager" in main_activity:
        errors.append("MainActivity must not own AppUpdateManager directly")
    if "AppUpdateCoordinator" not in main_activity:
        errors.append("MainActivity must delegate update flow to AppUpdateCoordinator")

    for owner, source in (("HSVJEngine", hsvj_engine), ("DrmKmsRenderService", drm_service)):
        if "PlatformControlGateway.initialize(" not in source:
            errors.append(f"{owner} must initialize PlatformControlGateway in its process")

    critical_methods = (
        "executeRestart",
        "scheduleWatchdogAlarm",
        "applyNetworkIpConfig",
        "applyPowerSchedule",
    )
    for method in critical_methods:
        if f"PlatformControlGateway.{method}(" not in hsvj_engine:
            errors.append(f"HSVJEngine.{method} must delegate to PlatformControlGateway")


def check_retired_subsystems(errors: list[str]) -> None:
    for path in RETIRED_RKNN_PATHS:
        if path.exists():
            errors.append(f"retired RKNN artifact must not return: {path.relative_to(ROOT)}")

    runtime_rknn = [
        *(ROOT / "app/src/main/assets").rglob("*.rknn"),
        *(ROOT / "app/src/main/jniLibs").rglob("librknnrt.so"),
    ]
    if runtime_rknn:
        errors.append(
            "retired RKNN runtime artifacts found: "
            + ", ".join(str(path.relative_to(ROOT)) for path in runtime_rknn)
        )

    for path in (ROOT / "docs").glob("*.md"):
        text = path.read_text(encoding="utf-8-sig", errors="replace")
        if "WaveBeatDetector" in text:
            errors.append(f"documentation references retired WaveBeatDetector: {path.relative_to(ROOT)}")


def check_retired_tests(errors: list[str]) -> None:
    for path in RETIRED_TEST_PATHS:
        if path.exists():
            errors.append(f"retired test artifact must not return: {path.relative_to(ROOT)}")

    forbidden_build_markers = {
        ROOT / "package.json": ("jest",),
        ROOT / "Fultter_pad/pubspec.yaml": ("flutter_test",),
        ROOT / "vendor/ffmpeg-rockchip/Makefile": (
            "enc_recon_frame_test", "scale_slice_test",
        ),
        ROOT / "vendor/ffmpeg-rockchip/tools/Makefile": (
            "enc_recon_frame_test", "scale_slice_test",
        ),
        ROOT / "vendor/ffmpeg-rockchip/libavformat/Makefile": ("fifo_test",),
        ROOT / "vendor/ffmpeg-rockchip/libavformat/allformats.c": ("fifo_test",),
        ROOT / "vendor/ffmpeg-rockchip/libavformat/muxer_list.c": ("fifo_test",),
        ROOT / "vendor/ffmpeg-rockchip/config_components.h": ("FIFO_TEST",),
        ROOT / "vendor/ffmpeg-rockchip/ffbuild/config.mak": ("FIFO_TEST",),
        ROOT / "vendor/jsoncpp/CMakeLists.txt": (
            "JSONCPP_WITH_TESTS", "JSONCPP_WITH_POST_BUILD_UNITTEST",
        ),
        ROOT / "vendor/jsoncpp/src/CMakeLists.txt": (
            "jsontestrunner", "test_lib_json",
        ),
        ROOT / "vendor/jsoncpp/meson.build": ("jsoncpp_test", "jsontestrunner"),
        ROOT / "vendor/libyuv/Android.bp": ("libyuv_unittest", "unit_test/"),
        ROOT / "vendor/libyuv/Android.mk": ("libyuv_unittest", "unit_test/"),
        ROOT / "vendor/libyuv/BUILD.gn": ("libyuv_unittest", "unit_test/"),
        ROOT / "vendor/libyuv/CMakeLists.txt": ("UNIT_TEST", "ly_unittest_sources"),
        ROOT / "vendor/rkmpp/utils/CMakeLists.txt": ("osd3_test",),
        ROOT / "vendor/LibJpeg-Turbo-Android-master/app/build.gradle": (
            "testImplementation", "androidTestImplementation", "testInstrumentationRunner",
        ),
        ROOT / "vendor/LibJpeg-Turbo-Android-master/turbojpeg/build.gradle": (
            "testImplementation", "androidTestImplementation", "testInstrumentationRunner",
        ),
        ROOT / "vendor/LibJpeg-Turbo-Android-master/turbojpeg/src/main/cpp/java/CMakeLists.txt": (
            "TJUnitTest", "JAVAARGS",
        ),
    }
    for path, markers in forbidden_build_markers.items():
        if not path.exists():
            continue
        text = path.read_text(encoding="utf-8-sig", errors="replace")
        for marker in markers:
            if marker in text:
                errors.append(
                    f"retired test build marker must not return: "
                    f"{path.relative_to(ROOT)}: {marker}"
                )


def main() -> int:
    errors: list[str] = []
    check_shader_pipeline(errors)
    check_android_architecture(errors)
    check_retired_subsystems(errors)
    check_retired_tests(errors)

    for legacy in LEGACY_PATHS:
        if legacy.exists():
            errors.append(f"legacy moved file still exists: {legacy.relative_to(ROOT)}")

    all_actual: dict[str, set[str]] = {}
    for module in MODULE_TARGETS:
        actual, locations = actual_dependencies(module)
        all_actual[module] = actual

        if module == "foundation" and actual:
            errors.append(f"foundation must not depend on project modules: {sorted(actual)}")
        if module == "utils":
            forbidden = actual - {"foundation", "jsoncpp"}
            if forbidden:
                details = ", ".join(
                    f"{dependency} at {locations[dependency][0]}" for dependency in sorted(forbidden)
                )
                errors.append(f"utils may only depend on foundation/jsoncpp: {details}")

        declared_targets = declared_contract(module)
        if declared_targets is None:
            errors.append(f"src/{module}/CMakeLists.txt: missing Architecture dependency contract")
            continue
        actual_targets = {
            "hsvj_jsoncpp" if dependency == "jsoncpp" else MODULE_TARGETS[dependency]
            for dependency in actual
        }
        missing = sorted(actual_targets - declared_targets)
        extra = sorted(declared_targets - actual_targets)
        if missing:
            errors.append(
                f"src/{module}/CMakeLists.txt: contract missing actual dependencies {missing}"
            )
        if extra:
            errors.append(
                f"src/{module}/CMakeLists.txt: contract has stale dependencies {extra}"
            )

    json_sources = {
        "json_reader.cpp",
        "json_value.cpp",
        "json_writer.cpp",
    }
    declarations: list[str] = []
    active_cmakes = [ROOT / "app/src/main/cpp/CMakeLists.txt", *(SRC / module / "CMakeLists.txt" for module in MODULE_TARGETS)]
    for cmake in active_cmakes:
        if not cmake.exists():
            continue
        text = cmake.read_text(encoding="utf-8-sig", errors="replace")
        if any(name in text for name in json_sources):
            declarations.append(str(cmake.relative_to(ROOT)))
    if declarations != ["src\\foundation\\CMakeLists.txt"] and declarations != ["src/foundation/CMakeLists.txt"]:
        errors.append(f"jsoncpp implementation must be declared only by src/foundation/CMakeLists.txt: {declarations}")

    if errors:
        print("Architecture check failed:")
        for error in errors:
            print(f"  - {error}")
        return 1

    edges = sum(len(dependencies) for dependencies in all_actual.values())
    print(
        f"Architecture check passed: {len(MODULE_TARGETS)} modules, {edges} explicit include edges, "
        "native layering, Android ownership, retired subsystems/tests, shaders, and jsoncpp ownership are consistent."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
