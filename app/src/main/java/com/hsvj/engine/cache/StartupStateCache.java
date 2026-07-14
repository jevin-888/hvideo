/**
 * @file StartupStateCache.java（文件名）
 * @brief 启动状态缓存
 * 
 * 缓存启动过程中的状态信息，用于快速恢复启动状态
 */

package com.hsvj.engine.cache;

import android.util.Log;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;

/**
 * 启动状态缓存
 */
public class StartupStateCache {
    private static final String TAG = "StartupStateCache";
    private static final String CACHE_TYPE = "startup_state";
    
    private final CacheManager cacheManager;
    
    /**
     * 启动状态信息
     */
    public static class StartupState {
        // 资源复制状态
        public boolean criticalResourcesCopied = false;
        public boolean deferredResourcesCopied = false;
        public long resourceCopyTime = 0;
        
        // 引擎初始化状态
        public boolean engineInitialized = false;
        public long engineInitTime = 0;
        
        // 模块加载状态
        public boolean vulkanRendererReady = false;
        public boolean audioProcessorReady = false;
        public boolean effectManagerReady = false;
        
        // 性能统计
        public long totalStartupTime = 0;
        public long lastStartupTime = 0;
        public int startupCount = 0;
        
        // 设备信息
        public long availableMemoryMB = 0;
        public boolean lowMemoryMode = false;
        
        public JSONObject toJson() throws JSONException {
            JSONObject json = new JSONObject();
            
            // 资源状态
            json.put("criticalResourcesCopied", criticalResourcesCopied);
            json.put("deferredResourcesCopied", deferredResourcesCopied);
            json.put("resourceCopyTime", resourceCopyTime);
            
            // 引擎状态
            json.put("engineInitialized", engineInitialized);
            json.put("engineInitTime", engineInitTime);
            
            // 模块状态
            json.put("vulkanRendererReady", vulkanRendererReady);
            json.put("audioProcessorReady", audioProcessorReady);
            json.put("effectManagerReady", effectManagerReady);
            
            // 性能统计
            json.put("totalStartupTime", totalStartupTime);
            json.put("lastStartupTime", lastStartupTime);
            json.put("startupCount", startupCount);
            
            // 设备信息
            json.put("availableMemoryMB", availableMemoryMB);
            json.put("lowMemoryMode", lowMemoryMode);
            
            return json;
        }
        
        public static StartupState fromJson(JSONObject json) throws JSONException {
            StartupState state = new StartupState();
            
            // 资源状态
            state.criticalResourcesCopied = json.optBoolean("criticalResourcesCopied", false);
            state.deferredResourcesCopied = json.optBoolean("deferredResourcesCopied", false);
            state.resourceCopyTime = json.optLong("resourceCopyTime", 0);
            
            // 引擎状态
            state.engineInitialized = json.optBoolean("engineInitialized", false);
            state.engineInitTime = json.optLong("engineInitTime", 0);
            
            // 模块状态
            state.vulkanRendererReady = json.optBoolean("vulkanRendererReady", false);
            state.audioProcessorReady = json.optBoolean("audioProcessorReady", false);
            state.effectManagerReady = json.optBoolean("effectManagerReady", false);
            
            // 性能统计
            state.totalStartupTime = json.optLong("totalStartupTime", 0);
            state.lastStartupTime = json.optLong("lastStartupTime", 0);
            state.startupCount = json.optInt("startupCount", 0);
            
            // 设备信息
            state.availableMemoryMB = json.optLong("availableMemoryMB", 0);
            state.lowMemoryMode = json.optBoolean("lowMemoryMode", false);
            
            return state;
        }
    }
    
    public StartupStateCache(CacheManager cacheManager) {
        this.cacheManager = cacheManager;
    }
    
    /**
     * 加载启动状态缓存
     */
    public StartupState loadStartupState() {
        try {
            if (!cacheManager.hasCacheFile(CACHE_TYPE)) {
                return new StartupState();
            }
            
            String cacheFile = cacheManager.getCacheFilePath(CACHE_TYPE);
            File file = new File(cacheFile);
            
            FileInputStream fis = new FileInputStream(file);
            byte[] data = new byte[(int) file.length()];
            fis.read(data);
            fis.close();
            
            String jsonString = new String(data, StandardCharsets.UTF_8);
            JSONObject json = new JSONObject(jsonString);
            
            return StartupState.fromJson(json);
            
        } catch (Exception e) {
            Log.e(TAG, "加载启动状态缓存失败", e);
            return new StartupState();
        }
    }
    
    /**
     * 保存启动状态缓存
     */
    public void saveStartupState(StartupState state) {
        try {
            JSONObject json = state.toJson();
            String jsonString = json.toString(2);
            
            String cacheFile = cacheManager.getCacheFilePath(CACHE_TYPE);
            File file = new File(cacheFile);
            
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
            Log.e(TAG, "保存启动状态缓存失败", e);
        }
    }
    
    /**
     * 更新启动统计信息
     */
    public void updateStartupStats(long startupTime, long availableMemory, boolean lowMemoryMode) {
        StartupState state = loadStartupState();
        
        state.lastStartupTime = startupTime;
        state.totalStartupTime += startupTime;
        state.startupCount++;
        state.availableMemoryMB = availableMemory;
        state.lowMemoryMode = lowMemoryMode;
        
        saveStartupState(state);
        
        Log.i(TAG, String.format("启动统计已更新: 第%d次启动, 耗时%dms, 内存%dMB", 
            state.startupCount, startupTime, availableMemory));
    }
    
    /**
     * 标记资源复制完成
     */
    public void markResourcesCopied(boolean critical, long copyTime) {
        StartupState state = loadStartupState();
        
        if (critical) {
            state.criticalResourcesCopied = true;
        } else {
            state.deferredResourcesCopied = true;
            // 延后复制完成时，若读到的是默认状态（如缓存文件不存在），说明关键资源在本轮已由 onCriticalComplete 标记，避免保存 critical=false 导致下次误判
            if (!state.criticalResourcesCopied) {
                state.criticalResourcesCopied = true;
            }
        }
        state.resourceCopyTime = copyTime;
        
        saveStartupState(state);
        
        Log.i(TAG, String.format("资源复制状态已更新: 关键资源=%s, 延后资源=%s", 
            state.criticalResourcesCopied, state.deferredResourcesCopied));
    }
    
    /**
     * 标记模块就绪状态
     */
    public void markModuleReady(String moduleName, boolean ready) {
        StartupState state = loadStartupState();
        
        switch (moduleName.toLowerCase()) {
            case "vulkan":
            case "renderer":
                state.vulkanRendererReady = ready;
                break;
            case "audio":
                state.audioProcessorReady = ready;
                break;
            case "effect":
            case "effects":
                state.effectManagerReady = ready;
                break;
        }
        
        saveStartupState(state);
        
        Log.i(TAG, String.format("模块状态已更新: %s = %s", moduleName, ready));
    }
    
    /**
     * 检查是否为快速启动（基于历史数据）
     */
    public boolean shouldUseFastStartup() {
        StartupState state = loadStartupState();
        
        // 如果启动次数少于3次，使用正常启动流程
        if (state.startupCount < 3) {
            return false;
        }
        
        // 如果上次启动时间过长，使用正常启动流程
        if (state.lastStartupTime > 15000) { // 15秒
            return false;
        }
        
        // 如果缓存无效，使用正常启动流程
        if (!cacheManager.isCacheValid()) {
            return false;
        }
        
        return true;
    }
    
    /**
     * 获取平均启动时间
     */
    public long getAverageStartupTime() {
        StartupState state = loadStartupState();
        
        if (state.startupCount == 0) {
            return 0;
        }
        
        return state.totalStartupTime / state.startupCount;
    }
    
    /**
     * 获取启动统计信息
     */
    public String getStartupStats() {
        StartupState state = loadStartupState();
        
        if (state.startupCount == 0) {
            return "启动统计: 无历史数据";
        }
        
        long avgTime = state.totalStartupTime / state.startupCount;
        
        return String.format("启动统计: 总计%d次, 平均%dms, 上次%dms, 内存%dMB%s", 
            state.startupCount, avgTime, state.lastStartupTime, 
            state.availableMemoryMB, state.lowMemoryMode ? "(低内存)" : "");
    }
    
    /**
     * 重置启动统计
     */
    public void resetStartupStats() {
        cacheManager.clearCache(CACHE_TYPE);
        Log.i(TAG, "启动统计已重置");
    }
}
