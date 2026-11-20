#!/usr/bin/env python3
"""Execute the Java ROM request contract against native-controller status data."""
import argparse
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--json-jar', required=True, type=Path)
    args = parser.parse_args()
    android = Path(os.environ['ANDROID_HOME']) / 'platforms/android-34/android.jar'
    with tempfile.TemporaryDirectory(prefix='hsvj-rom-target-') as directory:
        root = Path(directory)
        harness = root / 'RomTargetContract.java'
        harness.write_text('''package com.hsvj.engine.update;
import org.json.JSONObject;
public class RomTargetContract {
    public static void main(String[] args) throws Exception {
        for (String hardware : new String[]{"hw81", "hw82"}) {
            JSONObject status = new JSONObject().put("protocol", 1).put("product", "H6_POR")
                .put("hardware", hardware).put("version_code", 48).put("device_id", "private-device");
            JSONObject actual = RomUpdateManager.createCheckRequest(status);
            JSONObject expected = new JSONObject().put("protocol", 1).put("product", "H6_POR")
                .put("hardware", hardware).put("current_version_code", 48);
            if (!actual.similar(expected)) throw new AssertionError(actual.toString());
        }
        for (String missing : new String[]{"protocol", "product", "hardware", "version_code"}) {
            JSONObject status = new JSONObject().put("protocol", 1).put("product", "H6_POR")
                .put("hardware", "hw81").put("version_code", 48);
            status.remove(missing);
            try {
                RomUpdateManager.createCheckRequest(status);
                throw new AssertionError("Missing " + missing + " was accepted");
            } catch (org.json.JSONException expected) { }
        }
        System.out.println("ROM request contract: both native targets preserved; four missing fields rejected.");
    }
}
''', encoding='utf-8')
        classpath = os.pathsep.join(map(str, [args.json_jar, android]))
        sources = [ROOT / 'app/src/main/java/com/hsvj/engine/update/RomUpdateManager.java',
                   ROOT / 'app/src/main/java/com/hsvj/engine/util/ProcessRunner.java', harness]
        subprocess.run(['javac', '-encoding', 'UTF-8', '-cp', classpath, '-d', str(root), *map(str, sources)], check=True)
        subprocess.run(['java', '-cp', str(root) + os.pathsep + classpath,
                        'com.hsvj.engine.update.RomTargetContract'], check=True)


if __name__ == '__main__':
    main()
