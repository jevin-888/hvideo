# Add project specific ProGuard rules here.
-keep class com.hsvj.engine.** { *; }
-keepclassmembers class com.hsvj.engine.** { *; }

# AirPlay Mirroring SDK - 必须防止混淆，否则 JNI 回调会因找不到方法而崩溃 (mid == null)
-keep class com.huoshan.mirror.** { *; }
-keepclassmembers class com.huoshan.mirror.** { *; }
-dontwarn com.huoshan.mirror.**
