/**
 * @file CacheDebugActivity.java（文件名）
 * @brief 缓存调试界面
 * 
 * 用于调试和管理启动缓存，查看缓存状态和统计信息
 */

package com.hsvj.engine;

import android.app.Activity;
import android.os.Bundle;
import android.widget.Button;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import com.hsvj.engine.cache.CacheManager;
import com.hsvj.engine.cache.ConfigCache;
import com.hsvj.engine.cache.StartupStateCache;

/**
 * 缓存调试界面
 * 提供缓存管理和调试功能
 */
public class CacheDebugActivity extends Activity {
    private static final String TAG = "CacheDebug";
    
    private TextView infoText;
    private CacheManager cacheManager;
    private ConfigCache configCache;
    private StartupStateCache startupStateCache;
    
    // 预打包资源管理器
    private com.hsvj.engine.resource.EmbeddedResourceManager embeddedResourceManager;
    private com.hsvj.engine.resource.ResourceClassifier resourceClassifier;
    private com.hsvj.engine.resource.IncrementalUpdater incrementalUpdater;
    
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        
        // 创建简单的调试界面
        createDebugLayout();
        
        // 初始化缓存管理器
        initializeCacheManagers();
        
        // 刷新显示
        refreshCacheInfo();
    }
    
    private void createDebugLayout() {
        ScrollView scrollView = new ScrollView(this);
        
        // 创建垂直布局
        android.widget.LinearLayout layout = new android.widget.LinearLayout(this);
        layout.setOrientation(android.widget.LinearLayout.VERTICAL);
        layout.setPadding(32, 32, 32, 32);
        
        // 标题
        TextView titleText = new TextView(this);
        titleText.setText("HSVJ Engine 缓存调试");
        titleText.setTextSize(20);
        titleText.setTextColor(0xFFFFFFFF);
        titleText.setPadding(0, 0, 0, 32);
        layout.addView(titleText);
        
        // 信息显示区域
        infoText = new TextView(this);
        infoText.setTextSize(12);
        infoText.setTextColor(0xFFCCCCCC);
        infoText.setTypeface(android.graphics.Typeface.MONOSPACE);
        infoText.setPadding(16, 16, 16, 16);
        infoText.setBackgroundColor(0xFF333333);
        layout.addView(infoText);
        
        // 按钮区域
        createButtons(layout);
        
        scrollView.addView(layout);
        scrollView.setBackgroundColor(0xFF000000);
        setContentView(scrollView);
    }
    
    private void createButtons(android.widget.LinearLayout layout) {
        // 刷新按钮
        Button refreshButton = new Button(this);
        refreshButton.setText("刷新缓存信息");
        refreshButton.setOnClickListener(v -> refreshCacheInfo());
        layout.addView(refreshButton);
        
        // 清除所有缓存按钮
        Button clearAllButton = new Button(this);
        clearAllButton.setText("清除所有缓存");
        clearAllButton.setOnClickListener(v -> {
            cacheManager.clearAllCaches();
            Toast.makeText(this, "所有缓存已清除", Toast.LENGTH_SHORT).show();
            refreshCacheInfo();
        });
        layout.addView(clearAllButton);
        
        // 清除配置缓存按钮
        Button clearConfigButton = new Button(this);
        clearConfigButton.setText("清除配置缓存");
        clearConfigButton.setOnClickListener(v -> {
            configCache.clearConfigCache("config.json");
            Toast.makeText(this, "配置缓存已清除", Toast.LENGTH_SHORT).show();
            refreshCacheInfo();
        });
        layout.addView(clearConfigButton);
        
        // 扫描资源分类按钮
        Button scanResourcesButton = new Button(this);
        scanResourcesButton.setText("扫描资源分类");
        scanResourcesButton.setOnClickListener(v -> {
            new Thread(() -> {
                resourceClassifier.scanAndClassifyResources();
                runOnUiThread(() -> {
                    Toast.makeText(this, "资源分类扫描完成", Toast.LENGTH_SHORT).show();
                    refreshCacheInfo();
                });
            }).start();
        });
        layout.addView(scanResourcesButton);
        
        // 验证预打包资源按钮
        Button validateEmbeddedButton = new Button(this);
        validateEmbeddedButton.setText("验证预打包资源");
        validateEmbeddedButton.setOnClickListener(v -> {
            new Thread(() -> {
                boolean valid = embeddedResourceManager.validateCriticalResources();
                runOnUiThread(() -> {
                    String message = valid ? "预打包资源验证通过" : "预打包资源验证失败";
                    Toast.makeText(this, message, Toast.LENGTH_SHORT).show();
                    refreshCacheInfo();
                });
            }).start();
        });
        layout.addView(validateEmbeddedButton);
        
        // 重置启动统计按钮
        Button resetStatsButton = new Button(this);
        resetStatsButton.setText("重置启动统计");
        resetStatsButton.setOnClickListener(v -> {
            startupStateCache.resetStartupStats();
            Toast.makeText(this, "启动统计已重置", Toast.LENGTH_SHORT).show();
            refreshCacheInfo();
        });
        layout.addView(resetStatsButton);
        
        // 返回按钮
        Button backButton = new Button(this);
        backButton.setText("返回");
        backButton.setOnClickListener(v -> finish());
        layout.addView(backButton);
    }
    
    private void initializeCacheManagers() {
        try {
            String rootPath = PathConfig.getRootPath(this);
            cacheManager = new CacheManager(this, rootPath);
            configCache = new ConfigCache(cacheManager, rootPath + "config/");
            startupStateCache = new StartupStateCache(cacheManager);
            
            // 初始化预打包资源管理器
            embeddedResourceManager = new com.hsvj.engine.resource.EmbeddedResourceManager(this);
            resourceClassifier = new com.hsvj.engine.resource.ResourceClassifier(this);
            incrementalUpdater = new com.hsvj.engine.resource.IncrementalUpdater(this, rootPath);
            
        } catch (Exception e) {
            infoText.setText("初始化缓存管理器失败: " + e.getMessage());
        }
    }
    
    private void refreshCacheInfo() {
        new Thread(() -> {
            try {
                StringBuilder info = new StringBuilder();
                
                // 基本信息
                info.append("=== HSVJ Engine 缓存状态 ===\n\n");
                info.append("时间: ").append(new java.util.Date().toString()).append("\n");
                info.append("缓存目录: ").append(cacheManager.getCacheFilePath("")).append("\n\n");
                
                // 缓存有效性
                info.append("=== 缓存有效性 ===\n");
                info.append("总体有效性: ").append(cacheManager.isCacheValid() ? "有效" : "无效").append("\n\n");
                
                // 配置缓存
                info.append("=== 配置缓存 ===\n");
                info.append(configCache.getCacheStats("config.json")).append("\n");
                info.append("\n");
                
                // 启动统计
                info.append("=== 启动统计 ===\n");
                info.append(startupStateCache.getStartupStats()).append("\n");
                info.append("平均启动时间: ").append(startupStateCache.getAverageStartupTime()).append("ms\n");
                info.append("建议快速启动: ").append(startupStateCache.shouldUseFastStartup() ? "是" : "否").append("\n\n");
                
                // 预打包资源信息
                info.append("=== 预打包资源 ===\n");
                info.append(embeddedResourceManager.getCacheStats()).append("\n");
                boolean resourcesValid = embeddedResourceManager.validateCriticalResources();
                info.append("资源完整性: ").append(resourcesValid ? "通过" : "失败").append("\n");
                
                String[] criticalKeys = embeddedResourceManager.getCriticalResourceKeys();
                info.append("关键资源数量: ").append(criticalKeys.length).append("\n");
                for (String key : criticalKeys) {
                    boolean hasResource = embeddedResourceManager.hasResource(key);
                    info.append(String.format("  - %s: %s\n", key, hasResource ? "已加载" : "缺失"));
                }
                info.append("\n");
                
                // 资源分类统计
                info.append("=== 资源分类统计 ===\n");
                try {
                    resourceClassifier.scanAndClassifyResources();
                    java.util.Map<com.hsvj.engine.resource.ResourceClassifier.Priority, Integer> stats = 
                        resourceClassifier.getClassificationStats();
                    
                    for (java.util.Map.Entry<com.hsvj.engine.resource.ResourceClassifier.Priority, Integer> entry : stats.entrySet()) {
                        info.append(String.format("%s: %d 个资源\n", entry.getKey().name(), entry.getValue()));
                    }
                } catch (Exception e) {
                    info.append("资源分类统计获取失败: ").append(e.getMessage()).append("\n");
                }
                info.append("\n");
                
                // 增量更新信息
                info.append("=== 增量更新 ===\n");
                boolean needsUpdate = incrementalUpdater.needsIncrementalUpdate();
                info.append("需要增量更新: ").append(needsUpdate ? "是" : "否").append("\n\n");
                
                // 缓存文件详情
                info.append("=== 缓存文件详情 ===\n");
                addCacheFileInfo(info, "background_images");
                addCacheFileInfo(info, "config_config.json");
                addCacheFileInfo(info, "startup_state");
                addCacheFileInfo(info, "version");
                
                // 内存信息
                info.append("\n=== 内存信息 ===\n");
                MemoryMonitor memoryMonitor = new MemoryMonitor(this);
                memoryMonitor.checkMemoryStatus();
                info.append("可用内存: ").append(memoryMonitor.getAvailableMemoryMB()).append("MB\n");
                info.append("总内存: ").append(memoryMonitor.getTotalMemoryMB()).append("MB\n");
                info.append("低内存模式: ").append(memoryMonitor.isCriticalMemory() ? "是" : "否").append("\n");
                
                runOnUiThread(() -> infoText.setText(info.toString()));
                
            } catch (Exception e) {
                runOnUiThread(() -> infoText.setText("获取缓存信息失败: " + e.getMessage()));
            }
        }).start();
    }
    
    private void addCacheFileInfo(StringBuilder info, String cacheType) {
        try {
            if (cacheManager.hasCacheFile(cacheType)) {
                long size = cacheManager.getCacheFileSize(cacheType);
                info.append(String.format("%-20s: %d 字节\n", cacheType, size));
            } else {
                info.append(String.format("%-20s: 不存在\n", cacheType));
            }
        } catch (Exception e) {
            info.append(String.format("%-20s: 错误 - %s\n", cacheType, e.getMessage()));
        }
    }
}
