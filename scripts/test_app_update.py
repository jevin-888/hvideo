#!/usr/bin/env python3
"""Run real Java APP update protocol/transport tests without a device or test APK."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    jars = sorted((Path(os.environ['USERPROFILE']) / '.gradle/caches/modules-2/files-2.1/org.json/json').rglob('json-*.jar'))
    if not jars:
        raise SystemExit('An org.json runtime JAR is required in the Gradle cache.')
    with tempfile.TemporaryDirectory(prefix='hsvj-app-update-') as directory:
        root = Path(directory)
        java = ROOT / 'app/src/main/java/com/hsvj/engine'
        sources = [java / 'util/CloudAuthTransport.java', java / 'update/AppUpdateProtocol.java',
                   java / 'update/AppUpdateDownload.java', ROOT / 'tests/update/AppUpdateContractTest.java']
        subprocess.run(['javac', '-encoding', 'UTF-8', '-Xlint:unchecked', '-cp', str(jars[-1]),
                        '-d', str(root), *map(str, sources)], check=True)
        subprocess.run(['java', '-cp', str(root) + os.pathsep + str(jars[-1]),
                        'com.hsvj.engine.update.AppUpdateContractTest', str(root)], check=True, timeout=120)


if __name__ == '__main__':
    main()
