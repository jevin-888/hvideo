# HSVJ arm64 codec stack. Keep this list one-to-one with the canonical
# public-library manifest and sepolicy/file_contexts.
BOARD_SEPOLICY_DIRS += vendor/hsvj-codec-vendor/sepolicy

PRODUCT_COPY_FILES += \
    vendor/hsvj-codec-vendor/vendor/lib64/libmpp.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libmpp.so \
    vendor/hsvj-codec-vendor/vendor/lib64/libavcodec.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libavcodec.so \
    vendor/hsvj-codec-vendor/vendor/lib64/libavformat.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libavformat.so \
    vendor/hsvj-codec-vendor/vendor/lib64/libavutil.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libavutil.so \
    vendor/hsvj-codec-vendor/vendor/lib64/libswscale.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libswscale.so \
    vendor/hsvj-codec-vendor/vendor/lib64/libswresample.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libswresample.so

# Merge vendor/etc/public.libraries.txt into the device vendor public-library
# source. Do not overwrite unrelated platform entries.
