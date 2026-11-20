"""Test the ROM production boot receiver with controllable Android services."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = "/mnt/d/HVIDEO/HSVJ/android11-hsvj-src/packages/apps/TvSettings/Settings/src/com/android/tv/settings/connectivity/WirelessBootReceiver.java"
source = subprocess.check_output(["wsl", "-d", "Ubuntu-24.04", "-u", "jevin", "--", "cat", SOURCE]).decode("utf-8")
cache = Path(os.environ.get("GRADLE_USER_HOME", Path.home() / ".gradle"))
jar = next(cache.glob("caches/modules-2/files-2.1/org.json/json/*/*/*.jar"))
stubs = {
    "android/content/BroadcastReceiver.java": '''package android.content;
public abstract class BroadcastReceiver {
  public static class PendingResult { public void finish() {} }
  public PendingResult goAsync() { return new PendingResult(); }
  public abstract void onReceive(Context context, Intent intent);
}''',
    "android/content/Intent.java": '''package android.content;
public class Intent {
  public static final String ACTION_BOOT_COMPLETED="boot";
  final String action; public Intent(String value) { action=value; }
  public String getAction() { return action; }
}''',
    "android/content/Context.java": '''package android.content;
import android.net.wifi.WifiManager;
import android.net.ConnectivityManager;
public class Context {
  public WifiManager wifi=new WifiManager();
  public ConnectivityManager connectivity=new ConnectivityManager();
  public <T> T getSystemService(Class<T> type) {
    return type.cast(type==WifiManager.class ? wifi : connectivity);
  }
}''',
    "android/net/wifi/SoftApConfiguration.java": '''package android.net.wifi;
public class SoftApConfiguration {
  public static final int BAND_2GHZ=1, SECURITY_TYPE_WPA2_PSK=1;
  public String ssid,password; public boolean shutdown=true;
  public static class Builder {
    SoftApConfiguration config=new SoftApConfiguration();
    public Builder() {} public Builder(SoftApConfiguration old) {}
    public Builder setBand(int value) { return this; }
    public Builder setSsid(String value) { config.ssid=value; return this; }
    public Builder setPassphrase(String value,int type) { config.password=value; return this; }
    public Builder setAutoShutdownEnabled(boolean value) { config.shutdown=value; return this; }
    public SoftApConfiguration build() { return config; }
  }
}''',
    "android/net/wifi/WifiManager.java": '''package android.net.wifi;
public class WifiManager {
  public static final int WIFI_AP_STATE_DISABLED=11;
  public int calls=0,state=11; public boolean enabled=false,accept=true;
  public SoftApConfiguration config;
  public SoftApConfiguration getSoftApConfiguration() { return config; }
  public boolean setSoftApConfiguration(SoftApConfiguration value) { config=value; return accept; }
  public boolean setWifiEnabled(boolean value) { calls++; enabled=value; return true; }
  public int getWifiApState() { return state; }
}''',
    "android/net/ConnectivityManager.java": '''package android.net;
import android.os.Handler;
public class ConnectivityManager {
  public static final int TETHERING_WIFI=0;
  public int stops=0,starts=0;
  public static class OnStartTetheringCallback {
    public void onTetheringStarted() {} public void onTetheringFailed() {}
  }
  public void stopTethering(int type) { stops++; }
  public void startTethering(int type,boolean ui,OnStartTetheringCallback cb,Handler handler) {
    starts++; cb.onTetheringStarted();
  }
}''',
    "android/os/Handler.java": "package android.os; public class Handler { public Handler(Looper loop) {} }",
    "android/os/Looper.java": "package android.os; public class Looper { public static Looper getMainLooper() { return new Looper(); } }",
    "android/os/SystemProperties.java": '''package android.os;
public class SystemProperties { public static String value="1"; public static String get(String key) { return value; } }''',
    "android/os/SystemClock.java": '''package android.os;
public class SystemClock { static long now=0; public static long elapsedRealtime() { now+=13000; return now; } }''',
    "android/util/Log.java": '''package android.util;
public class Log {
  public static int i(String tag,String value) { return 0; }
  public static int e(String tag,String value) { return 0; }
  public static int e(String tag,String value,Throwable t) { return 0; }
  public static int w(String tag,String value,Throwable t) { return 0; }
}''',
}
test = '''package com.android.tv.settings.connectivity;
import android.content.*;
import android.os.SystemProperties;
import java.nio.file.*;
public class WirelessBootTest {
  static void check(boolean pass,String name) { if(!pass)throw new AssertionError(name); }
  public static void main(String[] args) throws Exception {
    Path file=Paths.get(args[0]);
    check(WirelessBootReceiver.readStartupMode(file.toFile()).equals("hotspot"),"missing");
    String[] configs={"{}","{\\"debugHotspotEnabled\\":false}","{\\"startupWirelessMode\\":true}","broken",
        "{\\"startupWirelessMode\\":\\"wifi\\"}","{\\"startupWirelessMode\\":\\"hotspot\\"}"};
    for(int i=0;i<configs.length;i++) {
      Files.write(file,configs[i].getBytes(java.nio.charset.StandardCharsets.UTF_8));
      check(WirelessBootReceiver.readStartupMode(file.toFile()).equals(i==4 ? "wifi" : "hotspot"),"config "+i);
    }
    Context c=new Context(); WirelessBootReceiver.applyStartupMode(c,"hotspot");
    check(!c.wifi.enabled && c.wifi.calls==1 && c.connectivity.starts==1,"hotspot start");
    check(c.wifi.config.ssid.equals("HVIDEO") && c.wifi.config.password.equals("88888888") && !c.wifi.config.shutdown,"fixed AP");
    c=new Context(); WirelessBootReceiver.applyStartupMode(c,"wifi");
    check(c.wifi.enabled && c.wifi.calls==1 && c.connectivity.stops==1 && c.connectivity.starts==0 && c.wifi.config==null,"WiFi only");
    c=new Context(); c.wifi.state=13; WirelessBootReceiver.applyStartupMode(c,"wifi");
    check(c.wifi.calls==0 && c.connectivity.stops==1,"failed AP stop");
    c=new Context(); c.wifi.accept=false; WirelessBootReceiver.applyStartupMode(c,"hotspot");
    check(c.wifi.calls==0 && c.connectivity.starts==0,"failed configuration");
    c=new Context(); WirelessBootReceiver receiver=new WirelessBootReceiver();
    receiver.onReceive(c,new Intent("manual-radio-switch"));
    check(c.wifi.calls==0 && c.connectivity.starts==0,"ignore manual broadcasts");
    SystemProperties.value="0"; receiver.onReceive(c,new Intent(Intent.ACTION_BOOT_COMPLETED));
    check(c.wifi.calls==0 && c.connectivity.starts==0,"ignore non-dedicated ROM");
    System.out.println("PASS: production receiver config parsing, default/legacy/invalid, WiFi/hotspot, fixed AP, stop/configuration failures, manual broadcasts, non-dedicated ROM");
  }
}'''
with tempfile.TemporaryDirectory(prefix="hsvj-wireless-boot-") as temp:
    work = Path(temp)
    sources = []
    for relative, content in {**stubs, "com/android/tv/settings/connectivity/WirelessBootReceiver.java": source,
                              "com/android/tv/settings/connectivity/WirelessBootTest.java": test}.items():
        path = work / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
        sources.append(str(path))
    subprocess.run(["javac", "-encoding", "UTF-8", "-cp", str(jar), "-d", str(work), *sources], check=True)
    subprocess.run(["java", "-cp", os.pathsep.join([str(work), str(jar)]),
                    "com.android.tv.settings.connectivity.WirelessBootTest", str(work / "config.json")], check=True)
