/**
 * @file CacheManager.java（文件名）
 * @brief 启动缓存管理器
 * 
 * 管理启动过程中的各种缓存，包括配置缓存、资源索引缓存等
 */

package com.hsvj.engine.cache;

import android.content.Context;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.os.Build;
import android.util.Log;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;

/**
 * 启动缓存管理器
 * 负责管理启动过程中的各种缓存数据
 */
public class CacheManager {
    private static final String TAG = "CacheManager";
    
    private final Context context;
    private final String cacheDir;
    private final String versionFile;
    
    // 缓存版本信息
    private static class CacheVersion {
        String appVersion;
        long appVersionCode;
        long lastModified;
        String configHash;
        
        JSONObject toJson() throws JSONException {
            JSONObject json = new JSONObject();
            json.put("appVersion", appVersion);
            json.put("appVersionCode", appVersionCode);
            json.put("lastModified", lastModified);
            json.put("configHash", configHash);
            return json;
        }
        
        static CacheVersion fromJson(JSONObject json) throws JSONException {
            CacheVersion version = new CacheVersion();
            version.appVersion = json.optString("appVersion", "");
            version.appVersionCode = json.optLong("appVersionCode", 0);
            version.lastModified = json.optLong("lastModified", 0);
            version.configHash = json.optString("configHash", "");
            return version;
        }
    }
    
    public CacheManager(Context context, String rootPath) {
        this.context = context;
        voidRootPathForCompatibility(rootPath);
        File privateCacheRoot = new File(context.getCacheDir(), "hsvj_startup_cache");
        this.cacheDir = privateCacheRoot.getAbsolutePath() + "/";
        this.versionFile = cacheDir + "version.json";
        
        File dir = new File(cacheDir);
        if (!dir.exists()) {
            dir.mkdirs();
        }
    }

    private static void voidRootPathForCompatibility(String rootPath) {
        // 保留构造参数兼容旧调用，但 Java 缓存不能写入 C++ 管理的资源根目录。
    }
    
    /**
     * 检查缓存是否有效
     */
    public boolean isCacheValid() {
        try {
            File versionFileObj = new File(versionFile);
            if (!versionFileObj.exists()) {
                return false;
            }
            
            // 读取缓存版本信息
            CacheVersion cachedVersion = loadCacheVersion();
            if (cachedVersion == null) {
                return false;
            }
            
            // 获取当前应用版本
            CacheVersion currentVersion = getCurrentVersion();
            if (currentVersion == null) {
                return false;
            }
            
            // 比较版本信息
            boolean valid = cachedVersion.appVersionCode == currentVersion.appVersionCode &&
                           cachedVersion.configHash.equals(currentVersion.configHash);
            return valid;
            
        } catch (Exception e) {
            Log.w(TAG, "缓存有效性检查失败", e);
            return false;
        }
    }
    
    /**
     * 更新缓存版本信息
     */
    public void updateCacheVersion() {
        try {
            CacheVersion currentVersion = getCurrentVersion();
            if (currentVersion != null) {
                saveCacheVersion(currentVersion);
                Log.i(TAG, "缓存版本信息已更新");
            }
        } catch (Exception e) {
            Log.e(TAG, "更新缓存版本信息失败", e);
        }
    }
    
    /**
     * 清除所有缓存
     */
    public void clearAllCaches() {
        try {
            File dir = new File(cacheDir);
            if (dir.exists()) {
                deleteDirectory(dir);
                dir.mkdirs();
                Log.i(TAG, "所有缓存已清除");
            }
        } catch (Exception e) {
            Log.e(TAG, "清除缓存失败", e);
        }
    }
    
    /**
     * 清除特定类型的缓存
     */
    public void clearCache(String cacheType) {
        try {
            String cacheFile = getCacheFilePath(cacheType);
            File file = new File(cacheFile);
            if (file.exists()) {
                file.delete();
                Log.i(TAG, "已清除缓存: " + cacheType);
            }
        } catch (Exception e) {
            Log.e(TAG, "清除缓存失败: " + cacheType, e);
        }
    }
    
    /**
     * 获取缓存文件路径
     */
    public String getCacheFilePath(String cacheType) {
        return cacheDir + cacheType + ".cache";
    }
    
    /**
     * 检查特定缓存是否存在
     */
    public boolean hasCacheFile(String cacheType) {
        String cacheFile = getCacheFilePath(cacheType);
        return new File(cacheFile).exists();
    }
    
    /**
     * 获取缓存文件大小
     */
    public long getCacheFileSize(String cacheType) {
        String cacheFile = getCacheFilePath(cacheType);
        File file = new File(cacheFile);
        return file.exists() ? file.length() : 0;
    }
    
    private CacheVersion getCurrentVersion() {
        try {
            CacheVersion version = new CacheVersion();
            
            // 获取应用版本信息
            PackageManager pm = context.getPackageManager();
            PackageInfo packageInfo = pm.getPackageInfo(context.getPackageName(), 0);
            version.appVersion = packageInfo.versionName;
            version.appVersionCode = getPackageVersionCode(packageInfo);
            version.lastModified = System.currentTimeMillis();
            
            // 计算配置文件哈希
            version.configHash = calculateConfigHash();
            
            return version;
        } catch (Exception e) {
            Log.e(TAG, "获取当前版本信息失败", e);
            return null;
        }
    }
    
    private String calculateConfigHash() {
        try {
            // 这里应该计算关键配置文件的哈希值
            // 包括 config.json 等
            StringBuilder content = new StringBuilder();
            
            // 添加应用版本作为哈希的一部分
            PackageManager pm = context.getPackageManager();
            PackageInfo packageInfo = pm.getPackageInfo(context.getPackageName(), 0);
            content.append(packageInfo.versionName);
            content.append(getPackageVersionCode(packageInfo));
            
            // 计算MD5哈希
            MessageDigest md = MessageDigest.getInstance("MD5");
            byte[] hash = md.digest(content.toString().getBytes(StandardCharsets.UTF_8));
            
            StringBuilder hexString = new StringBuilder();
            for (byte b : hash) {
                String hex = Integer.toHexString(0xff & b);
                if (hex.length() == 1) {
                    hexString.append('0');
                }
                hexString.append(hex);
            }
            
            return hexString.toString();
            
        } catch (Exception e) {
            Log.e(TAG, "计算配置哈希失败", e);
            return String.valueOf(System.currentTimeMillis());
        }
    }

    @SuppressWarnings("deprecation")
    private long getPackageVersionCode(PackageInfo packageInfo) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            return packageInfo.getLongVersionCode();
        }
        return packageInfo.versionCode;
    }
    
    private CacheVersion loadCacheVersion() {
        try {
            File file = new File(versionFile);
            if (!file.exists()) {
                return null;
            }
            
            FileInputStream fis = new FileInputStream(file);
            byte[] data = new byte[(int) file.length()];
            fis.read(data);
            fis.close();
            
            String jsonString = new String(data, StandardCharsets.UTF_8);
            JSONObject json = new JSONObject(jsonString);
            
            return CacheVersion.fromJson(json);
            
        } catch (Exception e) {
            Log.e(TAG, "加载缓存版本信息失败", e);
            return null;
        }
    }
    
    private void saveCacheVersion(CacheVersion version) {
        try {
            JSONObject json = version.toJson();
            String jsonString = json.toString(2);
            
            File file = new File(versionFile);
            
            // 确保父目录存在
            File parentDir = file.getParentFile();
            if (parentDir != null && !parentDir.exists()) {
                if (!parentDir.mkdirs()) {
                    Log.e(TAG, "无法创建缓存目录: " + parentDir.getAbsolutePath());
                    return;
                }
            }
            
            FileOutputStream fos = new FileOutputStream(file);
            fos.write(jsonString.getBytes(StandardCharsets.UTF_8));
            fos.close();
            
        } catch (Exception e) {
            Log.e(TAG, "保存缓存版本信息失败", e);
        }
    }
    
    private void deleteDirectory(File dir) {
        if (dir.isDirectory()) {
            File[] files = dir.listFiles();
            if (files != null) {
                for (File file : files) {
                    deleteDirectory(file);
                }
            }
        }
        dir.delete();
    }
}
