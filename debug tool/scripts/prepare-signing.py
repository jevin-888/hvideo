"""Build the Windows signing payload from the project's two hardware keys."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
PLAYER = ROOT.parent
SDK = Path(os.environ.get('ANDROID_HOME') or os.environ.get('ANDROID_SDK_ROOT') or
           str(Path(os.environ['LOCALAPPDATA']) / 'Android/Sdk'))
JLINK = shutil.which('jlink')
if not JLINK:
    raise SystemExit('Build host requires jlink on PATH (JDK 17).')
JDK = Path(JLINK).resolve().parents[1]
TOOLS = SDK / 'build-tools/37.0.0'
OUTPUT = ROOT / 'src-tauri/resources/signing-runtime.zip'
STAMP = OUTPUT.with_suffix('.inputs.json')
signing_file = PLAYER / 'signing.properties'
signing_properties = {}
if signing_file.is_file():
    for line in signing_file.read_text(encoding='utf-8-sig').splitlines():
        name, separator, value = line.partition('=')
        if separator and not name.lstrip().startswith(('#', '!')):
            signing_properties[name.strip()] = value.strip()

credentials = {}
for hardware in ('hw81', 'hw82'):
    for key, filename in [('storePassword', 'store-password'), ('keyPassword', 'key-password')]:
        name = f'{hardware}.platform.{key}'
        env_name = f'HVIDEO_{hardware.upper()}_PLATFORM_{key.upper()}'
        value = signing_properties.get(name) or os.environ.get(env_name, '').strip()
        if not value:
            raise SystemExit(f'Missing {name} in signing.properties or {env_name}.')
        credentials[f'{hardware}/{filename}'] = value.encode('utf-8')

SOURCES = {
    'hw81/platform.keystore': PLAYER / 'vendor/security8.1/platform.keystore',
    'hw82/platform.keystore': PLAYER / 'vendor/security8.2/platform.keystore',
    'apksigner.jar': TOOLS / 'lib/apksigner.jar',
    'android-NOTICE.txt': TOOLS / 'NOTICE.txt',
    'android-source.properties': TOOLS / 'source.properties',
}
# These public signed manifests apply only to the exact published V60 image.
for name in ('manifest', 'manifest-hw81', 'manifest-hw82'):
    for suffix in ('json', 'sig'):
        filename = f'{name}.{suffix}'
        SOURCES[f'rom-bundles/V60/{filename}'] = ROOT / 'src-tauri/resources/rom-v60' / filename


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


inputs = {name: sha(path) for name, path in SOURCES.items()}
inputs['jdk-release'] = sha(JDK / 'release')
inputs['builder'] = sha(Path(__file__))
inputs['credentials'] = hashlib.sha256(b'\0'.join(credentials.values())).hexdigest()
if OUTPUT.is_file() and STAMP.is_file():
    stamp = json.loads(STAMP.read_text())
    if stamp.get('inputs') == inputs and stamp.get('sha256') == sha(OUTPUT):
        print('Embedded signing payload is current.')
        raise SystemExit(0)

cache = ROOT / 'src-tauri/target'
cache.mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix='signing-package-', dir=cache) as staging:
    directory = Path(staging)
    for name, path in SOURCES.items():
        destination = directory / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, destination)
    subprocess.run([JLINK, '--module-path', str(JDK / 'jmods'), '--add-modules',
                    'java.base,java.logging,java.xml,jdk.crypto.ec,jdk.crypto.cryptoki',
                    '--strip-debug', '--no-man-pages', '--no-header-files', '--compress=2',
                    '--output', str(directory / 'jre')], check=True, creationflags=0x08000000)
    archive_path = directory / 'payload.zip'
    with zipfile.ZipFile(archive_path, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for name, value in credentials.items():
            archive.writestr(name, value)
        for file in sorted(directory.rglob('*')):
            if file.is_file() and file != archive_path:
                archive.write(file, file.relative_to(directory).as_posix())
    OUTPUT.parent.mkdir(exist_ok=True)
    shutil.copyfile(archive_path, OUTPUT)
STAMP.write_text(json.dumps({'inputs': inputs, 'sha256': sha(OUTPUT)}, indent=2) + '\n')
print(f'Embedded signing payload ready: {OUTPUT.stat().st_size} bytes')
