# HSVJ Codec Vendor Package

This folder contains the libraries needed to move HSVJ Engine FFmpeg/RKMPP runtime dependencies into the Android vendor partition.

## Required files

Copy these into the ROM tree, typically under `vendor/hsvj-codec-vendor/vendor/lib64/`, then add `AndroidProductCopyFiles.mk` or equivalent `PRODUCT_COPY_FILES` lines:

```text
vendor/lib64/libmpp.so
vendor/lib64/libavcodec.so
vendor/lib64/libavformat.so
vendor/lib64/libavutil.so
vendor/lib64/libavfilter.so
vendor/lib64/libswscale.so
vendor/lib64/libswresample.so
vendor/lib64/libffmpeg_58.so
```

Merge `vendor/etc/public.libraries.txt` into the ROM's existing `/vendor/etc/public.libraries.txt` source.

## Optional files

`optional/lib64` contains:

```text
libavdevice.so
libpostproc.so
libdrm.so
librockit.so
```

Only move these to `/vendor/lib64` if you also want these components to be system/vendor provided. If `librockit.so` remains in the APK, `libdrm.so` can remain in the APK too.

## SELinux

Merge `sepolicy/file_contexts.hsvj_codec` into the device vendor file contexts. Required libraries should use `same_process_hal_file` so app linker namespaces can load vendor public native libraries.

## App build

After the ROM provides these libraries, build the APK without bundled FFmpeg/RKMPP:

```powershell
.\gradlew.bat assembleHw81BetaDebug -PuseSystemCodecStack=true
```

## Full integration notes

See `docs/系统FFmpeg-RKMPP集成说明.md` in the project root.
