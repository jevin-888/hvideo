/**
 * @file IncrementalUpdater.java（文件名）
 * @brief 增量资源更新器
 * 
 * 支持增量更新资源，只复制变更的文件，减少启动时间
 */

package com.hsvj.engine.resource;

import com.hsvj.engine.HuoshanStorageRepair;
import com.hsvj.engine.StartupConfig;

import android.content.Context;
import android.content.res.AssetManager;
import android.util.Log;

import org.json.JSONException;
import org.json.JSONObject;

import android.os.StatFs;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;

/**
 * 增量资源更新器
 * 通过文件哈希比较，只更新变更的资源文件
 */
public class IncrementalUpdater {
    private static final String TAG = "IncrementalUpdater";
    private static final String[] REQUIRED_EXTRACTED_ASSETS = {
            "web/index.html"
    };

    // 磁盘容量保护：目标分区至少保留 100MB 才允许资源同步
    private static final long DISK_RESERVE_BYTES = 100L * 1024 * 1024;

    private final Context context;
    private final AssetManager assetManager;
    private final String targetRootPath;
    private final String manifestFile;

    // 防止并发执行
    private static final Object updateLock = new Object();
    private static volatile boolean isUpdating = false;
    
    // 已处理文件集合（防止重复处理）
    private final Set<String> processedFiles = new HashSet<>();
    
    // 资源清单信息
    private static class ResourceManifest {
        Map<String, String> fileHashes;
        long manifestVersion;
        String appVersion;
        String targetRootPath;
        
        ResourceManifest() {
            fileHashes = new HashMap<>();
            manifestVersion = System.currentTimeMillis();
        }
        
        JSONObject toJson() throws JSONException {
            JSONObject json = new JSONObject();
            json.put("manifestVersion", manifestVersion);
            json.put("appVersion", appVersion);
            json.put("targetRootPath", targetRootPath);
            
            JSONObject hashesJson = new JSONObject();
            for (Map.Entry<String, String> entry : fileHashes.entrySet()) {
                hashesJson.put(entry.getKey(), entry.getValue());
            }
            json.put("fileHashes", hashesJson);
            
            return json;
        }
        
        static ResourceManifest fromJson(JSONObject json) throws JSONException {
            ResourceManifest manifest = new ResourceManifest();
            manifest.manifestVersion = json.optLong("manifestVersion", 0);
            manifest.appVersion = json.optString("appVersion", "");
            manifest.targetRootPath = json.optString("targetRootPath", "");
            
            JSONObject hashesJson = json.optJSONObject("fileHashes");
            if (hashesJson != null) {
                java.util.Iterator<String> keys = hashesJson.keys();
                while (keys.hasNext()) {
                    String key = keys.next();
                    manifest.fileHashes.put(key, hashesJson.getString(key));
                }
            }
            
            return manifest;
        }
    }
    
    public IncrementalUpdater(Context context, String targetRootPath) {
        this.context = context;
        this.assetManager = context.getAssets();
        this.targetRootPath = targetRootPath;
        // 清单保存在应用私有目录，避免外部存储权限/rename 导致下次启动读不到而重复“新增”全部文件
        this.manifestFile = new File(context.getFilesDir(), "resource_manifest.json").getAbsolutePath();
    }
    
    /**
     * 执行增量更新
     */
    public UpdateResult performIncrementalUpdate(ResourceClassifier classifier) {
        // 防止并发执行
        synchronized (updateLock) {
            if (isUpdating) {
                Log.w(TAG, "增量更新正在进行中，跳过本次调用");
                UpdateResult result = new UpdateResult();
                result.success = false;
                result.errorMessage = "Update already in progress";
                return result;
            }
            
            isUpdating = true;
        }
        
        try {
            Log.i(TAG, "开始执行增量资源更新");

            long startTime = System.currentTimeMillis();
            UpdateResult result = new UpdateResult();

            // 磁盘容量保护：检查目标存储可用空间
            long availableSpace = getAvailableSpace(targetRootPath);
            if (availableSpace >= 0 && availableSpace < DISK_RESERVE_BYTES) {
                long availMB = availableSpace / (1024 * 1024);
                Log.e(TAG, "磁盘空间不足，跳过资源同步: 可用 " + availMB + "MB, 最低要求 "
                    + (DISK_RESERVE_BYTES / (1024 * 1024)) + "MB");
                result.success = false;
                result.errorMessage = "磁盘空间不足: 可用 " + availMB + "MB";
                return result;
            }

            try {
                // 清空已处理文件集合
                processedFiles.clear();

                // 加载现有清单
                Log.i(TAG, "[增量更新] 步骤1/4: 加载现有清单");
                ResourceManifest existingManifest = loadExistingManifest();
                
                // 创建新清单（这是最耗时的步骤）
                Log.i(TAG, "[增量更新] 步骤2/4: 扫描并计算资源哈希（可能需要几秒钟）");
                ResourceManifest newManifest = createNewManifest(classifier);
                
                // 比较并更新资源
                Log.i(TAG, "[增量更新] 步骤3/4: 比较清单并更新变更文件");
                result = compareAndUpdate(existingManifest, newManifest, classifier);
                
                // 保存新清单
                if (result.success) {
                    Log.i(TAG, "[增量更新] 步骤4/4: 保存新清单");
                    saveManifest(newManifest);
                }
                
                long duration = System.currentTimeMillis() - startTime;
                result.totalTime = duration;
                
                Log.i(TAG, String.format("增量更新完成: 新增 %d, 更新 %d, 删除 %d, 耗时 %d ms",
                    result.addedFiles, result.updatedFiles, result.deletedFiles, duration));
                
            } catch (Exception e) {
                Log.e(TAG, "增量更新失败", e);
                result.success = false;
                result.errorMessage = e.getMessage();
            }
            
            return result;
            
        } finally {
            synchronized (updateLock) {
                isUpdating = false;
            }
        }
    }
    
    /**
     * 加载现有资源清单
     */
    private ResourceManifest loadExistingManifest() {
        try {
            File manifestFileObj = new File(manifestFile);
            if (!manifestFileObj.exists()) {
                return new ResourceManifest();
            }
            
            byte[] data = new byte[(int) manifestFileObj.length()];
            try (FileInputStream fis = new FileInputStream(manifestFileObj)) {
                fis.read(data);
            }
            
            String jsonString = new String(data, StandardCharsets.UTF_8);
            JSONObject json = new JSONObject(jsonString);
            
            ResourceManifest manifest = ResourceManifest.fromJson(json);
            Log.i(TAG, String.format("加载现有清单: %d 个文件, 应用版本 %s", 
                manifest.fileHashes.size(), manifest.appVersion));
            
            return manifest;
            
        } catch (Exception e) {
            Log.w(TAG, "加载现有清单失败", e);
            return new ResourceManifest();
        }
    }
    
    /**
     * 创建新的资源清单
     */
    private ResourceManifest createNewManifest(ResourceClassifier classifier) {
        ResourceManifest manifest = new ResourceManifest();
        
        try {
            manifest.appVersion = context.getPackageManager()
                .getPackageInfo(context.getPackageName(), 0).versionName;
            manifest.targetRootPath = normalizeRootPath(targetRootPath);
            
            for (ResourceClassifier.Priority priority : ResourceClassifier.Priority.values()) {
                // [HSVJ_Fix] 不再跳过 CRITICAL 资源。Shader 和关键配置也需要增量更新哈希，
                // 否则源码改动后无法自动同步到设备。
                
                for (ResourceClassifier.ResourceInfo resource : classifier.getResourcesByPriority(priority)) {
                    if (!resource.isDirectory) {
                        // 检查线程中断标志，支持超时退出
                        if (Thread.interrupted()) {
                            Log.w(TAG, "[增量更新] 扫描被中断（可能已超时），停止处理");
                            return manifest;
                        }
                        
                        String hash = calculateAssetHash(resource.assetPath);
                        if (hash != null) {
                            manifest.fileHashes.put(resource.assetPath, hash);
                        }
                    }
                }
            }
            
            Log.i(TAG, String.format("新清单创建完成: %d 个文件", manifest.fileHashes.size()));
            
        } catch (Exception e) {
            Log.e(TAG, "创建新清单失败", e);
        }
        
        return manifest;
    }
    
    /**
     * 比较清单并更新资源
     */
    private UpdateResult compareAndUpdate(ResourceManifest existing, ResourceManifest newManifest, 
                                        ResourceClassifier classifier) {
        UpdateResult result = new UpdateResult();
        int unchangedCount = 0;
        
        for (Map.Entry<String, String> entry : newManifest.fileHashes.entrySet()) {
            String assetPath = entry.getKey();
            String newHash = entry.getValue();
            String existingHash = existing.fileHashes.get(assetPath);
            
            if (processedFiles.contains(assetPath)) {
                continue;
            }
            
            if (existingHash == null) {
                if (copyAssetFile(assetPath, classifier)) {
                    result.addedFiles++;
                    processedFiles.add(assetPath);
                } else {
                    result.failedFiles++;
                }
            } else if (!newHash.equals(existingHash)) {
                if (copyAssetFile(assetPath, classifier)) {
                    result.updatedFiles++;
                    processedFiles.add(assetPath);
                } else {
                    result.failedFiles++;
                }
            } else {
                String targetPath = getTargetPath(assetPath, classifier);
                File targetFile = new File(targetPath);
                
                if (!targetFile.exists()) {
                    if (copyAssetFile(assetPath, classifier)) {
                        result.addedFiles++;
                        processedFiles.add(assetPath);
                    } else {
                        result.failedFiles++;
                    }
                } else {
                    processedFiles.add(assetPath);
                    unchangedCount++;
                }
            }
        }
        
        Log.i(TAG, String.format("文件比较完成: 未变化 %d, 新增 %d, 更新 %d", 
            unchangedCount, result.addedFiles, result.updatedFiles));
        
        // 找出需要删除的文件
        for (String assetPath : existing.fileHashes.keySet()) {
            if (!newManifest.fileHashes.containsKey(assetPath)) {
                // 防止重复处理
                if (processedFiles.contains(assetPath)) {
                    continue;
                }
                
                // [HSVJ_Security] 严禁自动删除授权、配置、数据库、字体、素材、场景等敏感目录下的文件
                // 确保手动推送的 license.dat, 播放列表.db 以及用户加的字体和视频不会被 APK 同步误删。
                if (assetPath.startsWith("license/") || assetPath.startsWith("config/") || 
                    assetPath.startsWith("data/") || assetPath.startsWith("ttf/") || 
                    assetPath.startsWith("video/") || assetPath.startsWith("Scene/") || 
                    assetPath.startsWith("Lyrics/") || assetPath.startsWith("Music/") ||
                    assetPath.endsWith(".sh")) { // 保护脚本文件
                    processedFiles.add(assetPath);
                    continue;
                }
                
                if (deleteTargetFile(assetPath, classifier)) {
                    result.deletedFiles++;
                    processedFiles.add(assetPath);
                }
            }
        }
        
        result.success = result.failedFiles == 0;
        if (!result.success) {
            result.errorMessage = "资源复制失败: " + result.failedFiles + " 个文件";
        }
        return result;
    }
    
    /**
     * 复制Asset文件到目标位置
     */
    private boolean copyAssetFile(String assetPath, ResourceClassifier classifier) {
        try {
            // 获取目标路径
            String targetPath = getTargetPath(assetPath, classifier);
            if (targetPath == null) {
                return false;
            }
            
            // 确保目标目录存在
            File targetFile = new File(targetPath);
            if (targetFile.exists() && (assetPath.startsWith("config/") ||
                    assetPath.startsWith("license/") ||
                    assetPath.startsWith("data/"))) {
                Log.i(TAG, "保留用户文件，跳过覆盖: " + assetPath);
                return true;
            }
            File parentDir = targetFile.getParentFile();
            if (parentDir != null && !parentDir.exists()) {
                parentDir.mkdirs();
            }
            
            try {
                writeAssetToFile(assetPath, targetFile);
            } catch (IOException firstError) {
                if (!tryRepairWritableTarget(targetFile) || !retryWriteAsset(assetPath, targetFile)) {
                    throw firstError;
                }
            }
            
            return true;
            
        } catch (IOException e) {
            Log.e(TAG, "复制文件失败: " + assetPath, e);
            return false;
        }
    }

    private void writeAssetToFile(String assetPath, File targetFile) throws IOException {
        try (InputStream inputStream = assetManager.open(assetPath);
             FileOutputStream outputStream = new FileOutputStream(targetFile)) {
            byte[] buffer = new byte[8192];
            int bytesRead;
            while ((bytesRead = inputStream.read(buffer)) != -1) {
                outputStream.write(buffer, 0, bytesRead);
            }
        }
    }

    private boolean retryWriteAsset(String assetPath, File targetFile) {
        try {
            writeAssetToFile(assetPath, targetFile);
            Log.i(TAG, "权限修复后复制成功: " + assetPath);
            return true;
        } catch (IOException retryError) {
            Log.e(TAG, "权限修复后复制仍失败: " + assetPath, retryError);
            return false;
        }
    }

    private boolean tryRepairWritableTarget(File targetFile) {
        if (!targetRootPath.startsWith("/huoshan/")) {
            return false;
        }
        if (!HuoshanStorageRepair.repairFileTarget(context, targetFile)) {
            return false;
        }
        File parentDir = targetFile.getParentFile();
        return parentDir != null
                && HuoshanStorageRepair.isWritableDirectory(parentDir)
                && (!targetFile.exists() || targetFile.canWrite());
    }
    
    /**
     * 删除目标文件
     */
    private boolean deleteTargetFile(String assetPath, ResourceClassifier classifier) {
        try {
            String targetPath = getTargetPath(assetPath, classifier);
            if (targetPath == null) {
                return false;
            }

            File targetFile = new File(targetPath);
            if (targetFile.exists()) {
                return targetFile.delete();
            }

            return true;

        } catch (Exception e) {
            Log.e(TAG, "删除文件失败: " + assetPath, e);
            return false;
        }
    }
    
    /**
     * 获取Asset文件的目标路径
     */
    private String getTargetPath(String assetPath, ResourceClassifier classifier) {
        ResourceClassifier.ResourceInfo resource = classifier.getResourceInfo(assetPath);
        String relativePath = (resource != null && resource.targetPath != null && !resource.targetPath.isEmpty())
                ? resource.targetPath
                : assetPath;
        if (targetRootPath.startsWith("/huoshan/") && relativePath.startsWith("/sdcard/huoshan/")) {
            throw new IllegalStateException("资源目标路径错误，当前根目录为 /huoshan/，禁止写入 /sdcard/huoshan/: " + relativePath);
        }
        return targetRootPath + relativePath;
    }
    
    /**
     * 计算Asset文件的哈希值
     * 优化：对于大文件（>1MB），只计算前512KB的哈希以加速处理
     */
    private String calculateAssetHash(String assetPath) {
        InputStream inputStream = null;
        try {
            inputStream = assetManager.open(assetPath);
            MessageDigest md = MessageDigest.getInstance("MD5");
            
            byte[] buffer = new byte[8192];
            int bytesRead;
            long totalBytesRead = 0;
            long maxBytesToRead = 512 * 1024; // 最多读取512KB用于哈希计算
            
            while ((bytesRead = inputStream.read(buffer)) != -1) {
                md.update(buffer, 0, bytesRead);
                totalBytesRead += bytesRead;
                
                // 对于大文件，只计算前512KB的哈希以加速处理
                if (totalBytesRead >= maxBytesToRead) {
                    break;
                }
            }
            
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
            
        } catch (IOException | NoSuchAlgorithmException e) {
            Log.e(TAG, "计算文件哈希失败: " + assetPath, e);
            return null;
        } finally {
            if (inputStream != null) {
                try {
                    inputStream.close();
                } catch (IOException e) {
                    // 忽略关闭异常
                }
            }
        }
    }
    
    /**
     * 保存资源清单
     */
    private void saveManifest(ResourceManifest manifest) {
        try {
            File manifestFileObj = new File(manifestFile);
            File parentDir = manifestFileObj.getParentFile();
            if (parentDir != null && !parentDir.exists()) {
                if (!parentDir.mkdirs()) {
                    Log.e(TAG, "创建清单目录失败");
                    return;
                }
            }
            
            JSONObject json = manifest.toJson();
            String jsonString = json.toString(2);
            
            File tempFile = new File(manifestFile + ".tmp");
            try (FileOutputStream fos = new FileOutputStream(tempFile)) {
                fos.write(jsonString.getBytes(StandardCharsets.UTF_8));
                fos.flush();
                fos.getFD().sync(); // 强制同步到磁盘
            }
            
            if (manifestFileObj.exists()) {
                manifestFileObj.delete();
            }
            
            if (!tempFile.renameTo(manifestFileObj)) {
                // 外部存储上 renameTo 可能失败，改用复制到目标路径
                try (FileInputStream in = new FileInputStream(tempFile);
                     FileOutputStream out = new FileOutputStream(manifestFileObj)) {
                    byte[] buf = new byte[8192];
                    int n;
                    while ((n = in.read(buf)) != -1) {
                        out.write(buf, 0, n);
                    }
                    out.getFD().sync();
                }
                if (!tempFile.delete()) {
                    tempFile.deleteOnExit();
                }
                Log.w(TAG, "清单重命名失败，已通过复制写入: " + manifestFile);
            }
            
            Log.i(TAG, String.format("资源清单已保存: %s (%d 个文件)", 
                manifestFile, manifest.fileHashes.size()));
            
        } catch (Exception e) {
            Log.e(TAG, "保存资源清单失败", e);
        }
    }
    
    /**
     * 检查是否需要增量更新。
     * [HSVJ_Optimization] 只有当 APK 最后更新时间变了，才发起 MD5 深度比对，否则快速通过。
     */
    public boolean needsIncrementalUpdate() {
        try {
            File manifestFileObj = new File(manifestFile);
            if (!manifestFileObj.exists()) {
                Log.i(TAG, "清单文件不存在，需要执行首次同步");
                return true;
            }
            
            ResourceManifest existing = loadExistingManifest();
            String existingRoot = normalizeRootPath(existing.targetRootPath);
            String currentRoot = normalizeRootPath(targetRootPath);
            if (!currentRoot.equals(existingRoot)) {
                Log.i(TAG, "资源根路径变化，需要重新同步: old=" + existingRoot + ", new=" + currentRoot);
                return true;
            }
            String currentAppVersion = getCurrentAppVersion();
            if (!currentAppVersion.equals(existing.appVersion)) {
                Log.i(TAG, "应用版本变化，需要重新同步: old=" + existing.appVersion + ", new=" + currentAppVersion);
                return true;
            }
            if (hasMissingRequiredExtractedAssets()) {
                return true;
            }
            // 获取 APK 的最后安装/更新时间
            long apkLastUpdate = context.getPackageManager()
                .getPackageInfo(context.getPackageName(), 0).lastUpdateTime;
            
            if (apkLastUpdate > existing.manifestVersion) {
                Log.i(TAG, "检测到 APK 已更新，发起资源哈希深度比对...");
                return true;
            }
            
            Log.i(TAG, "APK 版本未变且关键延后资源完整，使用现有资产缓存");
            return false;
            
        } catch (Exception e) {
            Log.w(TAG, "检查增量更新需求失败", e);
            return true;
        }
    }
    
    /**
     * 更新结果
     */
    public static class UpdateResult {
        public boolean success = false;
        public int addedFiles = 0;
        public int updatedFiles = 0;
        public int deletedFiles = 0;
        public int failedFiles = 0;
        public long totalTime = 0;
        public String errorMessage = "";
        
        public int getTotalChangedFiles() {
            return addedFiles + updatedFiles + deletedFiles;
        }
        
        public boolean hasChanges() {
            return getTotalChangedFiles() > 0;
        }
    }

    private String normalizeRootPath(String rootPath) {
        if (rootPath == null || rootPath.isEmpty()) {
            return "";
        }
        return rootPath.endsWith("/") ? rootPath : rootPath + "/";
    }

    private String getCurrentAppVersion() throws Exception {
        return context.getPackageManager()
                .getPackageInfo(context.getPackageName(), 0)
                .versionName;
    }

    private boolean hasMissingRequiredExtractedAssets() {
        String normalizedRoot = normalizeRootPath(targetRootPath);
        for (String assetPath : REQUIRED_EXTRACTED_ASSETS) {
            File targetFile = new File(normalizedRoot + assetPath);
            if (!targetFile.exists() || targetFile.length() <= 0) {
                Log.i(TAG, "检测到必需资源缺失，需要重新同步: " + targetFile.getAbsolutePath());
                return true;
            }
        }

        for (String[] pair : StartupConfig.DEFERRED_ASSET_DIRS) {
            if (!"web".equals(pair[0])) {
                continue;
            }
            File targetDir = new File(normalizedRoot + pair[1]);
            if (!targetDir.exists() || !targetDir.isDirectory()) {
                Log.i(TAG, "检测到延后资源目录缺失，需要重新同步: " + targetDir.getAbsolutePath());
                return true;
            }
        }
        return false;
    }

    /**
     * 获取指定路径所在分区的可用空间（字节）
     * @return 可用字节数，无法获取时返回 -1
     */
    private long getAvailableSpace(String path) {
        try {
            File target = new File(path);
            if (!target.exists()) {
                // 尝试向上找到存在的父目录
                File parent = target.getParentFile();
                while (parent != null && !parent.exists()) {
                    parent = parent.getParentFile();
                }
                if (parent == null) return -1;
                target = parent;
            }
            StatFs stat = new StatFs(target.getAbsolutePath());
            return stat.getAvailableBytes();
        } catch (Exception e) {
            Log.w(TAG, "获取磁盘可用空间失败: " + e.getMessage());
            return -1;
        }
    }
}
