# Include this snippet from your product/device makefile, or copy these lines into it.
# Required HSVJ FFmpeg/RKMPP runtime libraries:

PRODUCT_COPY_FILES += \
    vendor/hsvj-codec-vendor/vendor/lib64/libmpp.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libmpp.so \
    vendor/hsvj-codec-vendor/vendor/lib64/libavcodec.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libavcodec.so \
    vendor/hsvj-codec-vendor/vendor/lib64/libavformat.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libavformat.so \
    vendor/hsvj-codec-vendor/vendor/lib64/libavutil.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libavutil.so \
    vendor/hsvj-codec-vendor/vendor/lib64/libavfilter.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libavfilter.so \
    vendor/hsvj-codec-vendor/vendor/lib64/libswscale.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libswscale.so \
    vendor/hsvj-codec-vendor/vendor/lib64/libswresample.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libswresample.so \
    vendor/hsvj-codec-vendor/vendor/lib64/libffmpeg_58.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libffmpeg_58.so

# Merge vendor/etc/public.libraries.txt into the device's existing
# $(TARGET_COPY_OUT_VENDOR)/etc/public.libraries.txt source. Do not overwrite
# the vendor public libraries file if the ROM already has one.
