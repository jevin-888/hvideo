# H66 internal ROM protocol 1

## Hardware metadata (V60, 2026-10-25)

New canonical ROM builds use manifest metadata `universal`. The player APK can
still be built as hw81 or hw82, but the native controller no longer rejects a
signed H6_POR bundle because of that metadata. The signed `hw81` and `hw82`
values remain accepted as compatibility fields for old controllers.

Existing V54 and earlier signed deliveries keep their historical `hw81` target.
Changing a manifest invalidates its signature and release ID. A new full ROM
version must rebuild the Android and Recovery controllers together. The desktop
tool can create a separately signed `hw81` or `hw82` compatibility manifest for
legacy controllers; it never changes `update.img`.

The desktop compatibility option verifies the original manifest with the embedded
ROM public key and checks the image size/SHA256 before changing only `hardware`.
It verifies the new signature before handing off; failures retain the selection.
Re-signing uses the local WSL Ubuntu-24.04 user jevin OpenSSL and the build-host
platform.pk8 (override with HVIDEO_ROM_SIGNING_KEY); private keys are not embedded
in the EXE. Moving the EXE alone does not provision a signing environment.
Both compatibility labels use the ROM trust key, not the APK hw81/hw82 key choice.

The authoritative module sources are Android.bp, rom_update.cpp, trusted_key.h and version_policy.h.
The canonical ROM build synchronizes these four files into
`vendor/chuangwei-minimal-perf/hsvj-rom-update` before building.

## Version policy (V50 onward)

Protocol 1 `--status` additionally reports boolean `supports_reflash` and
`supports_downgrade`. Same-version reflashing is enabled by default. A lower
version requires `--allow-downgrade` on each of `--manifest ID`, `--check ID`
and `--arm ID`; the option is rejected for other commands and unknown options
are rejected. Manifest/signature/image/partition validation remains mandatory.
The signed bundle schema and Recovery command are unchanged.

An explicit new attempt may reuse the same release after a terminal result;
`armed`, `installing` and `installed` remain locked. Recovery still clears BCB
before writing, preventing automatic retry after power loss. The desktop task
captures the checkbox as `allowDowngrade`. Legacy devices need one normal upgrade
to V50 first. Downgrading to an older ROM also restores its older controller rules.
Back up device data before flashing; ROM upgrades include the userdata partition.

Run `python3 -B test_version_policy.py` under WSL to execute the native policy
against higher/equal/lower versions, opt-in downgrade and pending/terminal states.

The existing Android worktree already has the integration applied. For a matching
pre-integration worktree, apply `recovery.patch`, `build-integration.patch`, then
`build-policy.patch` (the latter two use `git apply --unidiff-zero`). Do not reapply
patches on every build. Preserve unrelated vendor modifications.

## Huoshan first boot (V40 onward)

Android must mount an existing valid ext4 filesystem directly, and initialize a
wiped huoshan partition on first boot. The product already creates `/huoshan`
through `BOARD_ROOT_EXTRA_FOLDERS`; the fixed GPT already provides the block
partition. Do not repartition the device or confuse a missing directory with a
missing filesystem.

The normal Android fstab retains `latemount,wait,check,formattable,nofail`.
Android fs_mgr tries mounting first. Only after a mount failure (excluding
EBUSY/EACCES) and a positive native `partition_wiped()` check does it run mke2fs
and e2fsdroid, then retry mounting. This Android version checks the first 4096
bytes for all zero or all 0xff; it does not scan the entire partition and is not
a damaged-filesystem recovery service. A valid mounted filesystem is never
reformatted by this path.

The existing `post-fs-data` action sets only the mounted root directory to
`system:system` / 0777 before restoring labels and starting apps. Do not add
recursive ownership or mode changes to this action; `.rom-update` stays private.
Recovery internal upgrades still refuse to format huoshan, and no `huoshan.img`
is included in ROM bundles. The obsolete `storage-preservation.patch`, which
removed first-boot initialization, has been deleted. Never reapply it.

`verify_recovery.py` requires the built fstab, mount point, format/check tools,
early root permissions and Recovery no-format guard. `test_storage.py` tests
regressions that previously allowed a ROM unable to initialize blank storage.

For the v33 boot permission and ART fixes, apply `runtime-fixes.patch` once with
`git apply`. It excludes `.rom-update` from media ownership, mode and label repair,
enables `dalvik.vm.dex2oat64.enabled` before Android starts, and adds the runtime
artifact check to the canonical ROM build. It does not weaken controller checks.

`verify_recovery.py` checks the actual ramdisk, stable kernel, dynamic dependencies
and ext4 mounts. `package_rom.py` consumes the canonical image and pack directory,
pins the unchanged partition map, creates the signed manifest, and runs the host
build of the same native validator. Neither tool publishes or flashes a ROM.

The sole release entry `D:/HVIDEO/HSVJ/build_hsvj_rom.ps1` now calls
`package_rom.py` automatically after the canonical full ROM build succeeds.
A full release is complete only when the device-upgrade bundle passes signature,
whole-image, partition and expanded-image validation. Engine-only builds do not
produce ROM bundles. No separate build chain is required.

Every full release delivers:

- `D:/HVIDEO/HSVJ/rom-bundles/VNN/manifest.json`, `manifest.sig`, and
  `update.img`. These three matching files are required for device upgrades;
  `update.img` is also the complete ROM image for manual flashing.
- Build records contain `ROM_BUNDLE_vNN.log` and `ROM_BUNDLE_vNN.json`, recording
  the version, release ID, bundle path and ROM checksum; `LATEST_BUILD.txt`
  records the verified bundle. The root output directory receives no standalone
  image, `.sha256`, or `.bundle.json`. The signed manifest contains the hashes.

`release_id` is the SHA-256 of `manifest.json`, not the image hash. Never mix
manifests or signatures from different ROMs. Local delivery directories use the
uppercase version (`V36`, `V37`, ...). Before preparing a server release, copy
the three files into `rom/<release_id>/`; the receipt records that required name
as `server_directory_name`. Publication remains
an explicit server action. Signing material stays on the existing build host.

Existing version directories are never overwritten. Native verification runs
in a temporary staging directory before the completed version directory becomes
visible, and failed staging is cleaned automatically.
The signing tool reads the verified image directly from the existing build's
`out-hsvj-user/h66_por_full_update/update.img`. An exact existing bundle can be
reverified to repair delivery records; any different manifest at the same version
is rejected. The full build entry still rejects an existing version before building.

`verify_runtime.py` executes the installed boot permission block in an isolated
temporary filesystem tree, checks protected state, media, hidden downloads and
symlink targets across repeated runs, and verifies the built ART property and
AArch64 compiler. Both the build and bundle packager require it to pass.
Run `test_runtime.py` with `HSVJ_PERF_SCRIPT` pointing to the current vendor boot
script for regressions against the old recursive repair and invalid ART settings.

An already affected v33 device needs a one-time validated ownership repair before
it can download v34. The inspected H202609171001 repair and results are recorded
in `D:/HVIDEO/Hplayer/records/20260920-rom-runtime-fixes/`. That script is restricted to
the exact inspected v33 transaction; it is not a general startup migration.

Server/client contract and test records:
`D:/HVIDEO/Hvideo_server/docs/20260920_ROM内部升级实现.md`.
