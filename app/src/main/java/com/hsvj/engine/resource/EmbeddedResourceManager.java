/**
 * @file EmbeddedResourceManager.java（文件名）
 * @brief 嵌入式资源管理器
 * 
 * 管理内置到APK中的关键资源，提供快速访问
 */

package com.hsvj.engine.resource;

import com.hsvj.engine.HuoshanStorageRepair;

import android.content.Context;
import android.content.res.AssetManager;
import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

/**
 * 嵌入式资源管理器
 * 负责管理和快速提取内置在APK中的关键资源
 */
public class EmbeddedResourceManager {
    private static final String TAG = "EmbeddedResourceManager";
    
    private final Context context;
    private final AssetManager assetManager;
    
    // 内存缓存，避免重复读取
    private final Map<String, byte[]> memoryCache;
    
    // 关键资源映射表
    private static final Map<String, String> CRITICAL_RESOURCES = new HashMap<String, String>() {{
        // com.json 已废弃并删除，不再加载
        // 示例/字段：put("com.json", "config/com.json");
    }};
    
    // 必须存在的关键着色器列表（用于完整性检查）
    private static final String[] CRITICAL_SHADERS = {
        "shaders/texture.vert.spv",
        "shaders/texture.frag.spv",
        "shaders/nv24_texture.frag.spv",
        "shaders/region.vert.spv",
        "shaders/region.frag.spv"
    };
    
    public EmbeddedResourceManager(Context context) {
        this(context, true);
    }

    public EmbeddedResourceManager(Context context, boolean preloadFonts) {
        // preloadFonts 保留在构造签名中兼容旧调用；字体复制由增量同步统一处理。
        this.context = context;
        this.assetManager = context.getAssets();
        this.memoryCache = new ConcurrentHashMap<>();
        // 关键资源预加载：仅加载极少量的关键配置，Shaders 之后走动态流式提取
        preloadCriticalResources();
    }
    
    private void preloadCriticalResources() {
        for (Map.Entry<String, String> entry : CRITICAL_RESOURCES.entrySet()) {
            byte[] data = loadAssetToMemory(entry.getValue());
            if (data != null) {
                memoryCache.put(entry.getKey(), data);
            }
        }
    }

    /**
     * 从Asset加载文件到内存
     */
    private byte[] loadAssetToMemory(String assetPath) {
        try (InputStream inputStream = assetManager.open(assetPath);
             ByteArrayOutputStream outputStream = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[8192];
            int bytesRead;
            while ((bytesRead = inputStream.read(buffer)) != -1) {
                outputStream.write(buffer, 0, bytesRead);
            }
            return outputStream.toByteArray();
            
        } catch (IOException e) {
            Log.w(TAG, "加载Asset文件失败: " + assetPath, e);
            return null;
        }
    }
    
    /**
     * 快速提取关键资源到目标路径。自动扫描并提取整个 shaders 目录，确保新加入的 shader 也能同步。
     */
    public boolean extractCriticalResources(String targetRootPath) {
        Log.i(TAG, "开始提取关键资源到: " + targetRootPath);
        
        long startTime = System.currentTimeMillis();
        int extractedCount = 0;
        
        try {
            // 确保目标目录存在
            createDirectories(targetRootPath);
            
            // 1. 提取预加载的关键配置
            for (Map.Entry<String, String> entry : CRITICAL_RESOURCES.entrySet()) {
                String key = entry.getKey();
                String targetPath = targetRootPath + entry.getValue();
                if (extractResourceFromCache(key, targetPath)) {
                    extractedCount++;
                }
            }
            
            // 2. 动态扫描并提取所有 Shader 文件 (含子目录)
            extractedCount += extractAssetDirectory("shaders", targetRootPath + "shaders");
            
            long duration = System.currentTimeMillis() - startTime;
            Log.i(TAG, String.format("关键资源提取完成: %d 个文件, 耗时 %d ms", extractedCount, duration));
            return extractedCount > 0;
            
        } catch (Exception e) {
            Log.e(TAG, "提取关键资源失败", e);
            return false;
        }
    }

    /**
     * 递归提取 assets 目录下的所有文件
     * @return 提取成功的数量
     */
    private int extractAssetDirectory(String assetPath, String targetPath) {
        int count = 0;
        try {
            String[] files = assetManager.list(assetPath);
            if (files == null || files.length == 0) {
                // 这是一个文件而非文件夹
                if (extractResourceFromAsset(assetPath, targetPath)) {
                    return 1;
                }
                return 0;
            }

            // 这是一个文件夹
            File targetDir = new File(targetPath);
            if (!targetDir.exists()) targetDir.mkdirs();
            ensureWritableDirectory(targetDir);

            for (String file : files) {
                String subAssetPath = assetPath + "/" + file;
                String subTargetPath = targetPath + "/" + file;
                count += extractAssetDirectory(subAssetPath, subTargetPath);
            }
        } catch (IOException e) {
            Log.w(TAG, "提取目录失败: " + assetPath, e);
        }
        return count;
    }
    
    /**
     * 从 asset 流式复制到文件（用于未预加载的大文件如字体，降低内存峰值）
     */
    private boolean extractResourceFromAsset(String assetPath, String targetPath) {
        try {
            File targetFile = new File(targetPath);

            // [[Sync_Fix]] 保护逻辑：如果是在线/配置类文件且已存在，跳过覆盖
            if (shouldSkipExtraction(targetFile, assetPath, targetPath)) {
                Log.i(TAG, "跳过提取已存在的配置文件: " + targetPath);
                return true;
            }

            ensureParentDirectory(targetFile);
            try {
                writeAssetToFile(assetPath, targetFile);
            } catch (IOException firstError) {
                if (!repairWritableTarget(targetFile)) {
                    throw firstError;
                }
                writeAssetToFile(assetPath, targetFile);
                Log.i(TAG, "权限修复后按需提取成功: " + assetPath);
            }
            return true;
        } catch (IOException e) {
            Log.w(TAG, "按需提取失败: " + assetPath, e);
            return false;
        }
    }

    /**
     * 从缓存提取资源到文件
     * [[Sync_Fix]] 禁止在启动时盲目覆盖已存在的关键配置文件，否则用户调试的所有参数（几何、遮罩等）都会在重启后丢失。
     */
    private boolean extractResourceFromCache(String key, String targetPath) {
        try {
            File targetFile = new File(targetPath);
            
            // 保护策略：如果文件已存在且属于关键配置类资源，则跳过提取以保留用户数据
            if (shouldSkipExtraction(targetFile, key, targetPath)) {
                Log.i(TAG, "检测到已存在的配置文件，跳过覆盖以保留用户调试数据: " + targetPath);
                return true;
            }

            byte[] data = memoryCache.get(key);
            if (data == null) {
                Log.w(TAG, "缓存中未找到资源: " + key);
                return false;
            }
            
            // 确保目标目录存在
            ensureParentDirectory(targetFile);

            try {
                writeBytesToFile(data, targetFile);
            } catch (IOException firstError) {
                if (!repairWritableTarget(targetFile)) {
                    throw firstError;
                }
                writeBytesToFile(data, targetFile);
                Log.i(TAG, "权限修复后提取缓存资源成功: " + key);
            }
            return true;
            
        } catch (IOException e) {
            Log.e(TAG, "提取资源失败: " + key + " -> " + targetPath, e);
            return false;
        }
    }
    
    /**
     * 创建必要的目录结构
     */
    private void createDirectories(String rootPath) {
        String[] directories = {
            "config/",
            "license/", 
            "shaders/",
            "ttf/",
            "data/"
        };
        
        for (String dir : directories) {
            File directory = new File(rootPath + dir);
            if (!directory.exists()) {
                directory.mkdirs();
            }
            ensureWritableDirectory(directory);
        }
    }

    private void ensureParentDirectory(File targetFile) {
        File parentDir = targetFile.getParentFile();
        if (parentDir != null && !parentDir.exists()) {
            parentDir.mkdirs();
        }
        ensureWritableDirectory(parentDir);
    }

    private void ensureWritableDirectory(File directory) {
        if (directory != null && directory.getAbsolutePath().startsWith("/huoshan/")
                && !HuoshanStorageRepair.isWritableDirectory(directory)) {
            HuoshanStorageRepair.repairDirectory(context, directory);
        }
    }

    private boolean repairWritableTarget(File targetFile) {
        if (!HuoshanStorageRepair.repairFileTarget(context, targetFile)) {
            return false;
        }
        File parentDir = targetFile.getParentFile();
        return parentDir != null
                && HuoshanStorageRepair.isWritableDirectory(parentDir)
                && (!targetFile.exists() || targetFile.canWrite());
    }

    private void writeAssetToFile(String assetPath, File targetFile) throws IOException {
        try (InputStream in = assetManager.open(assetPath);
             FileOutputStream fos = new FileOutputStream(targetFile)) {
            byte[] buf = new byte[8192];
            int n;
            while ((n = in.read(buf)) > 0) {
                fos.write(buf, 0, n);
            }
        }
    }

    private void writeBytesToFile(byte[] data, File targetFile) throws IOException {
        try (FileOutputStream fos = new FileOutputStream(targetFile)) {
            fos.write(data);
        }
    }
    
    /**
     * 检查关键资源是否已存在
     */
    public boolean areCriticalResourcesExtracted(String targetRootPath) {
        for (String relativePath : CRITICAL_RESOURCES.values()) {
            File file = new File(targetRootPath + relativePath);
            if (!file.exists() || file.length() <= 0) {
                return false;
            }
        }

        for (String relativePath : CRITICAL_SHADERS) {
            File file = new File(targetRootPath + relativePath);
            if (!file.exists() || file.length() <= 0) {
                Log.w(TAG, "关键 shader 缺失: " + file.getAbsolutePath());
                return false;
            }
        }

        return true;
    }
    
    /**
     * 获取资源内容（从内存缓存）
     */
    public byte[] getResourceContent(String key) {
        return memoryCache.get(key);
    }
    
    /**
     * 获取资源内容（作为字符串）
     */
    public String getResourceContentAsString(String key) {
        byte[] data = memoryCache.get(key);
        return data != null ? new String(data) : null;
    }
    
    /**
     * 检查资源是否在缓存中
     */
    public boolean hasResource(String key) {
        return memoryCache.containsKey(key);
    }
    
    /**
     * 获取缓存统计信息
     */
    public String getCacheStats() {
        int totalResources = memoryCache.size();
        long totalSize = 0;
        
        for (byte[] data : memoryCache.values()) {
            totalSize += data.length;
        }
        
        return String.format("嵌入式资源缓存: %d 个文件, 总大小 %d 字节", totalResources, totalSize);
    }

    /**
     * 判断是否应跳过文件提取（保护已存在的配置文件不被覆盖）
     */
    private boolean shouldSkipExtraction(File targetFile, String resourceKey, String targetPath) {
        return targetFile.exists()
                && (resourceKey.endsWith(".json") || targetPath.contains("/config/"))
                && !isEmptyJsonArrayFile(targetFile);
    }

    private boolean isEmptyJsonArrayFile(File file) {
        if (!file.exists() || !file.isFile()) {
            return false;
        }
        if (file.length() == 0) {
            return true;
        }
        try (InputStream in = new java.io.FileInputStream(file);
             ByteArrayOutputStream out = new ByteArrayOutputStream()) {
            byte[] buf = new byte[1024];
            int n;
            while ((n = in.read(buf)) > 0) {
                out.write(buf, 0, n);
            }
            String content = out.toString("UTF-8").trim();
            return content.equals("[]");
        } catch (IOException e) {
            return false;
        }
    }
    
    /**
     * 清理内存缓存
     */
    public void clearCache() {
        memoryCache.clear();
        Log.i(TAG, "嵌入式资源缓存已清理");
    }
    
    /**
     * 验证关键资源完整性
     */
    public boolean validateCriticalResources() {
        Log.i(TAG, "验证关键资源完整性");
        
        for (String key : CRITICAL_RESOURCES.keySet()) {
            if (!memoryCache.containsKey(key)) {
                Log.e(TAG, "缺少关键资源: " + key);
                return false;
            }
            
            byte[] data = memoryCache.get(key);
            if (data == null || data.length == 0) {
                Log.e(TAG, "关键资源为空: " + key);
                return false;
            }
        }
        
        Log.i(TAG, "关键资源完整性验证通过");
        return true;
    }
    
    /**
     * 获取关键资源列表
     */
    public String[] getCriticalResourceKeys() {
        return CRITICAL_RESOURCES.keySet().toArray(new String[0]);
    }
}
