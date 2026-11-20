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

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Locale;

/**
 * 嵌入式资源管理器
 * 负责管理和快速提取内置在APK中的关键资源
 */
public class EmbeddedResourceManager {
    private static final String TAG = "EmbeddedResourceManager";
    
    private final Context context;
    private final AssetManager assetManager;
    
    // 必须存在的关键着色器列表（用于完整性检查）
    private static final String[] CRITICAL_SHADERS = {
        "shaders/texture.vert.spv",
        "shaders/texture.frag.spv",
        "shaders/nv24_texture.frag.spv",
        "shaders/region.vert.spv",
        "shaders/region.frag.spv"
    };
    
    private EmbeddedResourceManager(Context context) {
        this.context = context;
        this.assetManager = context.getAssets();
    }

    public static void prepareForEngine(Context context, String rootPath) {
        EmbeddedResourceManager embedded = new EmbeddedResourceManager(context);
        IncrementalUpdater updater = new IncrementalUpdater(context, rootPath);
        boolean needsUpdate = updater.needsIncrementalUpdate();
        boolean criticalReady = embedded.areCriticalResourcesExtracted(rootPath);
        if (!needsUpdate && criticalReady) {
            Log.i(TAG, "资源清单未变化且关键资源完整，跳过同步");
            return;
        }

        embedded.extractCriticalResources(rootPath);
        if (needsUpdate) {
            ResourceClassifier classifier = new ResourceClassifier(context);
            classifier.scanAndClassifyResources();
            IncrementalUpdater.UpdateResult result = updater.performIncrementalUpdate(classifier);
            Log.i(TAG, String.format(Locale.ROOT,
                    "增量同步完成 success=%b added=%d updated=%d deleted=%d failed=%d cost=%dms",
                    result.success, result.addedFiles, result.updatedFiles,
                    result.deletedFiles, result.failedFiles, result.totalTime));
            if (!result.success) {
                Log.e(TAG, "资源同步失败: " + result.errorMessage);
                throw new IllegalStateException("resource_update_failed");
            }
        }
        if (!embedded.areCriticalResourcesExtracted(rootPath)) {
            throw new IllegalStateException("critical_resources_missing");
        }
    }

    /**
     * 快速提取关键资源到目标路径。自动扫描并提取整个 shaders 目录，确保新加入的 shader 也能同步。
     */
    private void extractCriticalResources(String targetRootPath) {
        Log.i(TAG, "开始提取关键资源到: " + targetRootPath);
        
        long startTime = System.currentTimeMillis();
        try {
            createDirectories(targetRootPath);
            int extractedCount = extractAssetDirectory("shaders", targetRootPath + "shaders");
            
            long duration = System.currentTimeMillis() - startTime;
            Log.i(TAG, String.format(Locale.ROOT, "关键资源提取完成: %d 个文件, 耗时 %d ms", extractedCount, duration));
            
        } catch (Exception e) {
            Log.e(TAG, "提取关键资源失败", e);
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

    /**
     * 检查关键资源是否已存在
     */
    private boolean areCriticalResourcesExtracted(String targetRootPath) {
        for (String relativePath : CRITICAL_SHADERS) {
            File file = new File(targetRootPath + relativePath);
            if (!file.exists() || file.length() <= 0) {
                Log.w(TAG, "关键 shader 缺失: " + file.getAbsolutePath());
                return false;
            }
        }

        return true;
    }
    
    private boolean shouldSkipExtraction(File targetFile, String resourceKey, String targetPath) {
        return targetFile.exists()
                && (resourceKey.endsWith(".json") || targetPath.contains("/config/"))
                && !isEmptyJsonArrayFile(targetFile);
    }

    private boolean isEmptyJsonArrayFile(File file) {
        if (!file.isFile()) return false;
        if (file.length() == 0) return true;
        try {
            String content = new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8);
            return content.trim().equals("[]");
        } catch (IOException e) {
            return false;
        }
    }
}
