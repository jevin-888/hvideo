/**
 * @file ResourceClassifier.java（文件名）
 * @brief 资源分类管理器
 * 
 * 将资源分为不同优先级，支持预打包和按需加载策略
 */

package com.hsvj.engine.resource;

import android.content.Context;
import android.content.res.AssetManager;
import android.util.Log;

import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * 资源分类管理器
 * 根据启动优先级对资源进行分类管理
 */
public class ResourceClassifier {
    private static final String TAG = "ResourceClassifier";
    
    /**
     * 资源优先级枚举
     */
    public enum Priority {
        CRITICAL,    // 启动必需，内置到APK
        HIGH,        // 首次启动需要，优先复制
        MEDIUM,      // 常用功能，延后复制
        LOW,         // 很少使用，按需下载
        DEFERRED     // 后台处理，不影响启动
    }
    
    /**
     * 资源信息
     */
    public static class ResourceInfo {
        public String assetPath;
        public String targetPath;
        public Priority priority;
        public long estimatedSize;
        public boolean isDirectory;
        public Set<String> dependencies;
        
        public ResourceInfo(String assetPath, String targetPath, Priority priority) {
            this.assetPath = assetPath;
            this.targetPath = targetPath;
            this.priority = priority;
            this.estimatedSize = 0;
            this.isDirectory = false;
            this.dependencies = new HashSet<>();
        }
    }
    
    private final Context context;
    private final Map<Priority, List<ResourceInfo>> resourcesByPriority;
    private final Map<String, ResourceInfo> resourcesByPath;
    
    // 预定义的资源分类规则
    private static final Map<String, Priority> RESOURCE_RULES = new HashMap<String, Priority>() {{
        // 关键资源 - 内置到APK
        put("config/com.json", Priority.CRITICAL);
        put("shaders/", Priority.CRITICAL);
        // 字体由 libass 从 ttf 目录按实际文件加载，不在此限制
        
        // 高优先级 - 首次启动需要
        put("data/", Priority.HIGH);
        put("ttf/", Priority.HIGH);
        put("res/", Priority.HIGH);
        
        // 中等优先级 - 常用功能
        put("logo/", Priority.MEDIUM);
        put("lyrics/", Priority.MEDIUM);
        
        // 低优先级 - 很少使用
        put("Image/", Priority.LOW);
        put("Layout/", Priority.LOW);
        put("QRCode/", Priority.LOW);
        
        // 延后处理 - 后台复制
        put("video/", Priority.DEFERRED);
        put("models/", Priority.DEFERRED);
        put("Music/", Priority.DEFERRED);
        put("Scene/", Priority.DEFERRED);
    }};
    
    public ResourceClassifier(Context context) {
        this.context = context;
        this.resourcesByPriority = new HashMap<>();
        this.resourcesByPath = new HashMap<>();
        
        // 初始化优先级列表
        for (Priority priority : Priority.values()) {
            resourcesByPriority.put(priority, new ArrayList<>());
        }
    }
    
    /**
     * 扫描并分类所有资源
     */
    public void scanAndClassifyResources() {
        Log.i(TAG, "开始由预定义目录增量扫描资源");
        
        try {
            AssetManager assetManager = context.getAssets();
            
            // 从 StartupConfig 中获取扫描根目录和目标目录映射，避免扫描 assets 根目录导致的 10s+ 耗时
            for (String[] pair : com.hsvj.engine.StartupConfig.CRITICAL_ASSET_DIRS) {
                scanAssetDirectory(assetManager, pair[0], pair[1]);
            }
            for (String[] pair : com.hsvj.engine.StartupConfig.DEFERRED_ASSET_DIRS) {
                scanAssetDirectory(assetManager, pair[0], pair[1]);
            }
            
            // 计算资源大小和依赖关系
            calculateResourceMetrics();
            
            // 输出分类统计
            logClassificationStats();
            
        } catch (Exception e) {
            Log.e(TAG, "资源扫描和分类失败", e);
        }
    }
    
    /**
     * 递归扫描资源目录
     */
    private void scanAssetDirectory(AssetManager assetManager, String assetPath, String targetPath) {
        try {
            String[] files = assetManager.list(assetPath);
            if (files == null || files.length == 0) {
                return;
            }
            
            for (String fileName : files) {
                String fullAssetPath = assetPath.isEmpty() ? fileName : assetPath + "/" + fileName;
                String fullTargetPath = targetPath.isEmpty() ? fileName : targetPath + "/" + fileName;
                
                // 判断是否为目录
                boolean isDirectory = isAssetDirectory(assetManager, fullAssetPath);
                
                if (isDirectory) {
                    // 分类目录
                    Priority priority = classifyResource(fullAssetPath + "/");
                    ResourceInfo dirInfo = new ResourceInfo(fullAssetPath, fullTargetPath, priority);
                    dirInfo.isDirectory = true;
                    
                    addResource(dirInfo);
                    
                    // 递归扫描子目录
                    scanAssetDirectory(assetManager, fullAssetPath, fullTargetPath);
                } else {
                    // 分类文件
                    Priority priority = classifyResource(fullAssetPath);
                    ResourceInfo fileInfo = new ResourceInfo(fullAssetPath, fullTargetPath, priority);
                    fileInfo.isDirectory = false;
                    
                    addResource(fileInfo);
                }
            }
            
        } catch (IOException e) {
            Log.w(TAG, "扫描资源目录失败: " + assetPath, e);
        }
    }
    
    /**
     * 判断资源路径是否为目录
     */
    private boolean isAssetDirectory(AssetManager assetManager, String path) {
        try {
            // 尝试打开文件。如果能打开，说明是一个文件，不是目录。
            // 如果抛出 IOException（通常是 FileNotFoundException），且 list() 返回不为空，则是目录。
            try (InputStream is = assetManager.open(path)) {
                return false;
            }
        } catch (IOException e) {
            // 打不开通常意味着它是一个目录或者文件不存在
            try {
                String[] files = assetManager.list(path);
                return files != null && files.length > 0;
            } catch (IOException e2) {
                return false;
            }
        }
    }
    
    /**
     * 根据路径分类资源
     */
    private Priority classifyResource(String resourcePath) {
        // 首先检查精确匹配
        if (RESOURCE_RULES.containsKey(resourcePath)) {
            return RESOURCE_RULES.get(resourcePath);
        }
        
        // 检查目录匹配
        for (Map.Entry<String, Priority> entry : RESOURCE_RULES.entrySet()) {
            String pattern = entry.getKey();
            if (pattern.endsWith("/") && resourcePath.startsWith(pattern)) {
                return entry.getValue();
            }
        }
        
        // 根据文件扩展名分类
        Priority priority = classifyByExtension(resourcePath);
        if (priority != null) {
            return priority;
        }
        
        // 根据文件大小分类
        priority = classifyBySize(resourcePath);
        if (priority != null) {
            return priority;
        }
        
        // 默认为中等优先级
        return Priority.MEDIUM;
    }
    
    /**
     * 根据文件扩展名分类
     */
    private Priority classifyByExtension(String resourcePath) {
        String lowerPath = resourcePath.toLowerCase();
        
        // 关键配置文件
        if (lowerPath.endsWith(".json") || lowerPath.endsWith(".dat") || lowerPath.endsWith(".cfg")) {
            return Priority.CRITICAL;
        }
        
        // Shader文件
        if (lowerPath.endsWith(".vert") || lowerPath.endsWith(".frag") || 
            lowerPath.endsWith(".glsl") || lowerPath.endsWith(".spv")) {
            return Priority.CRITICAL;
        }
        
        // 字体文件
        if (lowerPath.endsWith(".ttf") || lowerPath.endsWith(".otf")) {
            return Priority.HIGH;
        }
        
        // 小图片文件
        if (lowerPath.endsWith(".png") || lowerPath.endsWith(".jpg") || lowerPath.endsWith(".jpeg")) {
            return Priority.HIGH;
        }
        
        // 大媒体文件
        if (lowerPath.endsWith(".mp4") || lowerPath.endsWith(".avi") || 
            lowerPath.endsWith(".mp3") || lowerPath.endsWith(".wav")) {
            return Priority.DEFERRED;
        }
        
        // 3D模型文件
        if (lowerPath.endsWith(".obj") || lowerPath.endsWith(".fbx") || lowerPath.endsWith(".gltf")) {
            return Priority.LOW;
        }
        
        return null;
    }
    
    /**
     * 根据估算文件大小分类
     */
    private Priority classifyBySize(String resourcePath) {
        // 这里可以添加基于文件大小的分类逻辑
        // 由于无法直接获取Asset文件大小，这里返回null
        return null;
    }
    
    /**
     * 添加资源到分类中
     */
    private void addResource(ResourceInfo resource) {
        resourcesByPriority.get(resource.priority).add(resource);
        resourcesByPath.put(resource.assetPath, resource);
    }
    
    /**
     * 计算资源指标
     */
    private void calculateResourceMetrics() {
        // 这里可以添加资源大小估算和依赖关系分析
    }
    
    /**
     * 输出分类统计信息
     */
    private void logClassificationStats() {
        Log.i(TAG, "=== 资源分类统计 ===");
        
        for (Priority priority : Priority.values()) {
            List<ResourceInfo> resources = resourcesByPriority.get(priority);
            Log.i(TAG, String.format("%s: %d 个资源", priority.name(), resources.size()));
        }
    }
    
    /**
     * 获取指定优先级的资源列表
     */
    public List<ResourceInfo> getResourcesByPriority(Priority priority) {
        return new ArrayList<>(resourcesByPriority.get(priority));
    }
    
    /**
     * 获取所有关键资源
     */
    public List<ResourceInfo> getCriticalResources() {
        return getResourcesByPriority(Priority.CRITICAL);
    }
    
    /**
     * 获取需要预复制的资源（关键+高优先级）
     */
    public List<ResourceInfo> getPreCopyResources() {
        List<ResourceInfo> resources = new ArrayList<>();
        resources.addAll(getResourcesByPriority(Priority.CRITICAL));
        resources.addAll(getResourcesByPriority(Priority.HIGH));
        return resources;
    }
    
    /**
     * 获取可延后处理的资源
     */
    public List<ResourceInfo> getDeferredResources() {
        List<ResourceInfo> resources = new ArrayList<>();
        resources.addAll(getResourcesByPriority(Priority.MEDIUM));
        resources.addAll(getResourcesByPriority(Priority.LOW));
        resources.addAll(getResourcesByPriority(Priority.DEFERRED));
        return resources;
    }
    
    /**
     * 检查资源是否应该内置到APK
     */
    public boolean shouldEmbedInAPK(String resourcePath) {
        ResourceInfo resource = resourcesByPath.get(resourcePath);
        return resource != null && resource.priority == Priority.CRITICAL;
    }

    public ResourceInfo getResourceInfo(String assetPath) {
        return resourcesByPath.get(assetPath);
    }
    
    /**
     * 获取资源分类统计
     */
    public Map<Priority, Integer> getClassificationStats() {
        Map<Priority, Integer> stats = new HashMap<>();
        for (Priority priority : Priority.values()) {
            stats.put(priority, resourcesByPriority.get(priority).size());
        }
        return stats;
    }
}
