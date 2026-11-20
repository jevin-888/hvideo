#!/usr/bin/env python3
"""Validate the Java/JNI boundary and critical native-to-Java callbacks."""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
JAVA_PATH = ROOT / "app/src/main/java/com/hsvj/engine/HSVJEngine.java"
CPP_PATH = ROOT / "app/src/main/cpp/jni_interface.cpp"
JNI_PREFIX = "Java_com_hsvj_engine_HSVJEngine_"

CRITICAL_CALLBACKS = {
    "controlDisplayResolution": (
        "(Ljava/lang/String;)Ljava/lang/String;",
        r"public\s+static\s+String\s+controlDisplayResolution\s*\(\s*String\s+\w+\s*\)",
    ),
    "processConfigBackup": (
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
        r"public\s+static\s+String\s+processConfigBackup\s*\(\s*String\s+\w+\s*,\s*String\s+\w+\s*,\s*String\s+\w+\s*\)",
    ),
    "requestHttpAcrossNetworks": (
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;I)Ljava/lang/String;",
        r"public\s+static\s+String\s+requestHttpAcrossNetworks\s*\(\s*String\s+\w+\s*,\s*String\s+\w+\s*,\s*String\s+\w+\s*,\s*String\s+\w+\s*,\s*int\s+\w+\s*\)",
    ),
    "downloadFileAcrossNetworks": (
        "(Ljava/lang/String;Ljava/lang/String;I)Ljava/lang/String;",
        r"public\s+static\s+String\s+downloadFileAcrossNetworks\s*\(\s*String\s+\w+\s*,\s*String\s+\w+\s*,\s*int\s+\w+\s*\)",
    ),
    "offerGroupLeaderTime": (
        "(JJ)V",
        r"public\s+static\s+void\s+offerGroupLeaderTime\s*\(\s*long\s+\w+\s*,\s*long\s+\w+\s*\)",
    ),
    "executeRestart": ("()Z", r"public\s+static\s+boolean\s+executeRestart\s*\(\s*\)"),
    "scheduleWatchdogAlarm": ("()Z", r"public\s+static\s+boolean\s+scheduleWatchdogAlarm\s*\(\s*\)"),
    "applyNetworkIpConfig": (
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Z",
        r"public\s+static\s+boolean\s+applyNetworkIpConfig\s*\(\s*String\s+\w+\s*,\s*String\s+\w+\s*,\s*String\s+\w+\s*,\s*String\s+\w+\s*\)",
    ),
    "applyPowerSchedule": (
        "(ZZLjava/lang/String;Ljava/lang/String;ZLjava/lang/String;Ljava/lang/String;)Z",
        r"public\s+static\s+boolean\s+applyPowerSchedule\s*\(\s*boolean\s+\w+\s*,\s*boolean\s+\w+\s*,\s*String\s+\w+\s*,\s*String\s+\w+\s*,\s*boolean\s+\w+\s*,\s*String\s+\w+\s*,\s*String\s+\w+\s*\)",
    ),
}


def main() -> int:
    java = JAVA_PATH.read_text(encoding="utf-8-sig")
    cpp = CPP_PATH.read_text(encoding="utf-8-sig")
    errors: list[str] = []

    java_natives = re.findall(
        r"\bpublic\s+(?:static\s+)?native\s+[\w.<>\[\]]+\s+(\w+)\s*\(", java
    )
    cpp_exports = re.findall(
        rf"JNIEXPORT\s+[A-Za-z0-9_]+\s+JNICALL\s+{re.escape(JNI_PREFIX)}([A-Za-z0-9_]+)\s*\(",
        cpp,
        re.S,
    )

    java_duplicates = sorted({name for name in java_natives if java_natives.count(name) > 1})
    cpp_duplicates = sorted({name for name in cpp_exports if cpp_exports.count(name) > 1})
    if java_duplicates:
        errors.append(f"overloaded/duplicate Java native methods require explicit JNI mangling: {java_duplicates}")
    if cpp_duplicates:
        errors.append(f"duplicate JNI exports: {cpp_duplicates}")

    java_set, cpp_set = set(java_natives), set(cpp_exports)
    missing_exports = sorted(java_set - cpp_set)
    orphan_exports = sorted(cpp_set - java_set)
    if missing_exports:
        errors.append(f"Java native declarations without C++ exports: {missing_exports}")
    if orphan_exports:
        errors.append(f"C++ JNI exports without Java declarations: {orphan_exports}")

    for name, (descriptor, java_pattern) in CRITICAL_CALLBACKS.items():
        if not re.search(java_pattern, java, re.S):
            errors.append(f"HSVJEngine.java callback signature mismatch: {name}")
        lookup = re.compile(
            r"GetStaticMethodID\s*\(\s*g_hsvjEngineClass\s*,\s*\""
            + re.escape(name)
            + r"\"\s*,\s*\""
            + re.escape(descriptor)
            + r"\"\s*\)",
            re.S,
        )
        if not lookup.search(cpp):
            errors.append(f"jni_interface.cpp callback descriptor mismatch: {name} {descriptor}")

    if errors:
        print("JNI contract check failed:")
        for error in errors:
            print(f"  - {error}")
        return 1

    print(
        f"JNI contract check passed: {len(java_natives)} Java native declarations map one-to-one "
        f"to {len(cpp_exports)} C++ exports; critical callback descriptors match."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
