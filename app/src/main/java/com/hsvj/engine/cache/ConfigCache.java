/**
 * @file ConfigCache.java（文件名）
 * @brief 配置文件缓存
 * 
 * 缓存解析后的配置文件，避免重复解析JSON等操作
 */

package com.hsvj.engine.cache;

import android.util.Log;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.HashMap;
import java.util.Map;

/**
 * 配置文件缓存
 */
public class ConfigCache {
    private static final String TAG = "ConfigCache";
    private static final String CACHE_TYPE = "config";
    
    private final CacheManager cacheManager;
    private final String configDir;
    
    /**
     * 缓存的配置信息
     */
    public static class CachedConfig {
        public String configHash;
        public long cacheTime;
        public JSONObject configData;
        public Map<String, Object> parsedData;
        
        public CachedConfig() {
            parsedData = new HashMap<>();
        }
    }
    
    public ConfigCache(CacheManager cacheManager, String configDir) {
        this.cacheManager = cacheManager;
        this.configDir = configDir;
    }
    
    /**
     * 加载缓存的配置
     */
    public CachedConfig loadCachedConfig(String configFileName) {
        try {
            if (!cacheManager.isCacheValid()) {
                return null;
            }
            
            String cacheFile = cacheManager.getCacheFilePath(CACHE_TYPE + "_" + configFileName);
            File file = new File(cacheFile);
            if (!file.exists()) {
                return null;
            }
            
            // 检查原始配置文件是否有变化
            String configFilePath = configDir + configFileName;
            String currentHash = calculateFileHash(configFilePath);
            if (currentHash == null) {
                return null;
            }
            
            // 读取缓存
            FileInputStream fis = new FileInputStream(file);
            byte[] data = new byte[(int) file.length()];
            fis.read(data);
            fis.close();
            
            String jsonString = new String(data, StandardCharsets.UTF_8);
            JSONObject cacheJson = new JSONObject(jsonString);
            
            String cachedHash = cacheJson.optString("configHash", "");
            if (!currentHash.equals(cachedHash)) {
                return null;
            }
            
            CachedConfig cached = new CachedConfig();
            cached.configHash = cachedHash;
            cached.cacheTime = cacheJson.optLong("cacheTime", 0);
            cached.configData = cacheJson.optJSONObject("configData");
            
            // 恢复解析后的数据
            JSONObject parsedJson = cacheJson.optJSONObject("parsedData");
            if (parsedJson != null) {
                cached.parsedData = jsonToMap(parsedJson);
            }
            
            Log.i(TAG, "成功加载配置缓存: " + configFileName);
            return cached;
            
        } catch (Exception e) {
            Log.e(TAG, "加载配置缓存失败: " + configFileName, e);
            return null;
        }
    }
    
    /**
     * 保存配置到缓存
     */
    public void saveCachedConfig(String configFileName, CachedConfig config) {
        try {
            JSONObject cacheJson = new JSONObject();
            cacheJson.put("configHash", config.configHash);
            cacheJson.put("cacheTime", System.currentTimeMillis());
            cacheJson.put("configData", config.configData);
            
            // 保存解析后的数据
            if (config.parsedData != null && !config.parsedData.isEmpty()) {
                JSONObject parsedJson = mapToJson(config.parsedData);
                cacheJson.put("parsedData", parsedJson);
            }
            
            String jsonString = cacheJson.toString(2);
            String cacheFilePath = cacheManager.getCacheFilePath(CACHE_TYPE + "_" + configFileName);
            File cacheFile = new File(cacheFilePath);
            
            // 确保父目录存在
            File parentDir = cacheFile.getParentFile();
            if (parentDir != null && !parentDir.exists()) {
                if (!parentDir.mkdirs()) {
                    Log.e(TAG, "无法创建缓存目录: " + parentDir.getAbsolutePath());
                    return;
                }
            }
            
            FileOutputStream fos = new FileOutputStream(cacheFile);
            fos.write(jsonString.getBytes(StandardCharsets.UTF_8));
            fos.close();
            
            Log.i(TAG, "配置缓存已保存: " + configFileName);
            
        } catch (Exception e) {
            Log.e(TAG, "保存配置缓存失败: " + configFileName, e);
        }
    }
    
    /**
     * 加载并缓存配置文件
     */
    public CachedConfig loadAndCacheConfig(String configFileName) {
        // 首先尝试从缓存加载
        CachedConfig cached = loadCachedConfig(configFileName);
        if (cached != null) {
            return cached;
        }
        
        // 缓存无效，重新加载配置文件
        try {
            String configFilePath = configDir + configFileName;
            File configFile = new File(configFilePath);
            if (!configFile.exists()) {
                Log.w(TAG, "配置文件不存在: " + configFilePath);
                return null;
            }
            
            // 读取配置文件
            FileInputStream fis = new FileInputStream(configFile);
            byte[] data = new byte[(int) configFile.length()];
            fis.read(data);
            fis.close();
            
            String configContent = new String(data, StandardCharsets.UTF_8).trim();
            JSONObject configJson = parseConfigContent(configFileName, configContent);
            if (configJson == null) {
                return null;
            }

            // 创建缓存对象
            CachedConfig newCached = new CachedConfig();
            newCached.configHash = calculateFileHash(configFilePath);
            newCached.cacheTime = System.currentTimeMillis();
            newCached.configData = configJson;
            
            // 预解析一些常用配置
            parseCommonConfigs(configJson, newCached.parsedData);
            
            // 保存到缓存
            saveCachedConfig(configFileName, newCached);
            
            Log.i(TAG, "配置文件已加载并缓存: " + configFileName);
            return newCached;
            
        } catch (Exception e) {
            Log.e(TAG, "加载配置文件失败: " + configFileName, e);
            return null;
        }
    }
    
    /**
     * 预解析常用配置项
     */
    private void parseCommonConfigs(JSONObject configJson, Map<String, Object> parsedData) {
        try {
            // 解析显示相关配置
            if (configJson.has("display")) {
                JSONObject display = configJson.getJSONObject("display");
                parsedData.put("display_width", display.optInt("width", 1920));
                parsedData.put("display_height", display.optInt("height", 1080));
                parsedData.put("display_fps", display.optInt("fps", 60));
            }
            
            // 解析图层配置（layers 可为 JSONObject 或 JSONArray）
            if (configJson.has("layers")) {
                JSONArray layersArray = configJson.optJSONArray("layers");
                JSONObject layersObj = configJson.optJSONObject("layers");
                int layerCount = layersArray != null ? layersArray.length() : (layersObj != null ? layersObj.length() : 0);
                parsedData.put("layer_count", layerCount);
            }
            
            // 解析渲染配置
            if (configJson.has("renderer")) {
                JSONObject renderer = configJson.getJSONObject("renderer");
                parsedData.put("renderer_type", renderer.optString("type", "vulkan"));
                parsedData.put("renderer_vsync", renderer.optBoolean("vsync", true));
            }
            
            // 解析音频配置
            if (configJson.has("audio")) {
                JSONObject audio = configJson.getJSONObject("audio");
                parsedData.put("audio_sample_rate", audio.optInt("sampleRate", 48000));
                parsedData.put("audio_channels", audio.optInt("channels", 2));
            }
            
        } catch (JSONException e) {
            Log.w(TAG, "预解析配置项失败", e);
        }
    }

    /**
     * 解析配置内容：支持根节点为 JSON 对象
     */
    private JSONObject parseConfigContent(String configFileName, String configContent) {
        try {
            if (configContent.startsWith("[")) {
                Log.w(TAG, "配置文件根节点不能为数组: " + configFileName);
                return null;
            }
            return new JSONObject(configContent);
        } catch (JSONException e) {
            Log.e(TAG, "解析配置内容失败: " + configFileName, e);
            return null;
        }
    }
    
    /**
     * 计算文件哈希值
     */
    private String calculateFileHash(String filePath) {
        try {
            File file = new File(filePath);
            if (!file.exists()) {
                return null;
            }
            
            FileInputStream fis = new FileInputStream(file);
            MessageDigest md = MessageDigest.getInstance("MD5");
            
            byte[] buffer = new byte[8192];
            int bytesRead;
            while ((bytesRead = fis.read(buffer)) != -1) {
                md.update(buffer, 0, bytesRead);
            }
            fis.close();
            
            byte[] hash = md.digest();
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
            Log.e(TAG, "计算文件哈希失败: " + filePath, e);
            return null;
        }
    }
    
    /**
     * Map转JSONObject
     */
    private JSONObject mapToJson(Map<String, Object> map) throws JSONException {
        JSONObject json = new JSONObject();
        for (Map.Entry<String, Object> entry : map.entrySet()) {
            json.put(entry.getKey(), entry.getValue());
        }
        return json;
    }
    
    /**
     * JSONObject转Map
     */
    private Map<String, Object> jsonToMap(JSONObject json) throws JSONException {
        Map<String, Object> map = new HashMap<>();
        java.util.Iterator<String> keys = json.keys();
        while (keys.hasNext()) {
            String key = keys.next();
            map.put(key, json.get(key));
        }
        return map;
    }
    
    /**
     * 清除特定配置的缓存
     */
    public void clearConfigCache(String configFileName) {
        cacheManager.clearCache(CACHE_TYPE + "_" + configFileName);
    }
    
    /**
     * 获取缓存统计信息
     */
    public String getCacheStats(String configFileName) {
        try {
            String cacheType = CACHE_TYPE + "_" + configFileName;
            if (!cacheManager.hasCacheFile(cacheType)) {
                return "配置缓存 " + configFileName + ": 无缓存";
            }
            
            long cacheSize = cacheManager.getCacheFileSize(cacheType);
            return String.format("配置缓存 %s: 缓存大小 %d 字节", configFileName, cacheSize);
            
        } catch (Exception e) {
            return "配置缓存 " + configFileName + ": 获取统计信息失败";
        }
    }
}
