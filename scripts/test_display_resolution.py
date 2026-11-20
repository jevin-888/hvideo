"""Exercise the production Java bridge against a controllable framework service."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
cache = Path(os.environ.get("GRADLE_USER_HOME", Path.home() / ".gradle"))
json_jar = next(cache.glob("caches/modules-2/files-2.1/org.json/json/*/*/*.jar"))
with tempfile.TemporaryDirectory(prefix="hsvj-resolution-") as temp:
    work = Path(temp)
    mock = work / "android/os/RkDisplayOutputManager.java"
    mock.parent.mkdir(parents=True)
    mock.write_text('''package android.os;
public class RkDisplayOutputManager {
    public static int type=11, writes=0, saves=0;
    public static String mode="1920x1080p60.00-4";
    public static boolean connected=true, apply=true, save=true;
    public int[] getIfaceList(int display) { return connected ? new int[]{type} : null; }
    public int getCurrentInterface(int display) { return type; }
    public String[] getModeList(int display, int type) {
        return new String[]{"Auto", "1920x1080p60.00-4"};
    }
    public String getCurrentMode(int display,int type) { return mode; }
    public void setMode(int display,int type,String value) { writes++; if(apply)mode=value; }
    public int saveConfig() { saves++; return save ? 0 : -1; }
}''', encoding="utf-8")
    test = work / "ResolutionTest.java"
    test.write_text('''package com.hsvj.engine;
import org.json.JSONObject;
import android.os.RkDisplayOutputManager;
public class ResolutionTest {
    static void check(boolean pass) { if(!pass)throw new AssertionError(); }
    static JSONObject call(String mode) { return new JSONObject(SystemDisplayResolution.request(mode)); }
    public static void main(String[] args) {
        JSONObject data=call("").getJSONObject("data");
        check(data.getString("mode").equals("1920x1080p60.00"));
        check(data.getJSONArray("modes").length()==3);
        check(call("bad").getInt("status")==400 && RkDisplayOutputManager.writes==0);
        check(call("1920x1080p60.00").has("data") && RkDisplayOutputManager.writes==0);
        check(call("3840x2160p30.00").getJSONObject("data").getString("mode").equals("3840x2160p30.00"));
        check(RkDisplayOutputManager.writes==1 && RkDisplayOutputManager.saves==1);
        RkDisplayOutputManager.save=false;
        check(call("3840x2160p60.00").getInt("status")==503);
        check(RkDisplayOutputManager.mode.equals("3840x2160p30.00"));
        RkDisplayOutputManager.save=true; RkDisplayOutputManager.apply=false;
        check(call("3840x2160p60.00").getInt("status")==503);
        check(RkDisplayOutputManager.mode.equals("3840x2160p30.00"));
        RkDisplayOutputManager.type=16;
        data=call("").getJSONObject("data");
        check(data.getBoolean("read_only") && data.getJSONArray("modes").length()==1);
        check(call("1920x1080p60.00").getInt("status")==400);
        RkDisplayOutputManager.connected=false;
        check(call("").getInt("status")==503);
        System.out.println("PASS: read/normalize, invalid mode, unchanged, apply/save, save rollback, apply rollback, MIPI, disconnected");
    }
}''', encoding="utf-8")
    source = ROOT / "app/src/main/java/com/hsvj/engine/SystemDisplayResolution.java"
    subprocess.run(["javac", "-encoding", "UTF-8", "-cp", str(json_jar), "-d", str(work),
                    str(mock), str(source), str(test)], check=True)
    subprocess.run(["java", "-cp", os.pathsep.join([str(work), str(json_jar)]),
                    "com.hsvj.engine.ResolutionTest"], check=True)
