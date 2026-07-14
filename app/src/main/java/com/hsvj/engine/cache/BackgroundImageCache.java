/**
 * @file BackgroundImageCache.java（文件名）
 * @brief 背景图片索引缓存
 * 
 * 缓存背景图片的索引信息，避免每次启动都扫描文件系统
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
import java.util.ArrayList;
import java.util.List;

/**
 * 背景图片索引缓存
 */
public class BackgroundImageCache {
    private static final String TAG = "BackgroundImageCache";
    private static final String CACHE_TYPE = "background_images";
    
    private final CacheManager cacheManager;
    private final String backgroundDir;
    
    /**
     * 背景图片信息
     */
    public static class ImageInfo {
        public String filename;
        public long fileSize;
        public long lastModified;
        public int width = 0;
        public int height = 0;
        
        public JSONObject toJson() throws JSONException {
            JSONObject json = new JSONObject();
            json.put("filename", filename);
            json.put("fileSize", fileSize);
            json.put("lastModified", lastModified);
            json.put("width", width);
            json.put("height", height);
            return json;
        }
        
        public static ImageInfo fromJson(JSONObject json) throws JSONException {
            ImageInfo info = new ImageInfo();
            info.filename = json.getString("filename");
            info.fileSize = json.getLong("fileSize");
            info.lastModified = json.getLong("lastModified");
            info.width = json.optInt("width", 0);
            info.height = json.optInt("height", 0);
            return info;
        }
    }
    
    public BackgroundImageCache(CacheManager cacheManager, String backgroundDir) {
        this.cacheManager = cacheManager;
        this.backgroundDir = backgroundDir;
    }
    
    /**
     * 加载缓存的图片索引
     */
    public List<ImageInfo> loadCachedIndex() {
        try {
            if (!cacheManager.isCacheValid() || !cacheManager.hasCacheFile(CACHE_TYPE)) {
                return null;
            }
            
            String cacheFile = cacheManager.getCacheFilePath(CACHE_TYPE);
            File file = new File(cacheFile);
            
            FileInputStream fis = new FileInputStream(file);
            byte[] data = new byte[(int) file.length()];
            fis.read(data);
            fis.close();
            
            String jsonString = new String(data, StandardCharsets.UTF_8);
            JSONObject json = new JSONObject(jsonString);
            
            // 检查目录是否有变化
            long cachedDirModified = json.optLong("dirLastModified", 0);
            File bgDir = new File(backgroundDir);
            if (bgDir.exists() && bgDir.lastModified() != cachedDirModified) {
                return null;
            }
            
            JSONArray imageArray = json.getJSONArray("images");
            List<ImageInfo> images = new ArrayList<>();
            
            for (int i = 0; i < imageArray.length(); i++) {
                JSONObject imageJson = imageArray.getJSONObject(i);
                ImageInfo info = ImageInfo.fromJson(imageJson);
                
                // 验证文件是否仍然存在且未修改
                File imageFile = new File(backgroundDir, info.filename);
                if (imageFile.exists() && 
                    imageFile.length() == info.fileSize && 
                    imageFile.lastModified() == info.lastModified) {
                    images.add(info);
                } else {
                    return null;
                }
            }
            
            Log.i(TAG, String.format("成功加载背景图片索引缓存，共 %d 个文件", images.size()));
            return images;
            
        } catch (Exception e) {
            Log.e(TAG, "加载背景图片索引缓存失败", e);
            return null;
        }
    }
    
    /**
     * 保存图片索引到缓存
     */
    public void saveCachedIndex(List<ImageInfo> images) {
        try {
            JSONObject json = new JSONObject();
            
            // 记录目录修改时间
            File bgDir = new File(backgroundDir);
            if (bgDir.exists()) {
                json.put("dirLastModified", bgDir.lastModified());
            }
            
            JSONArray imageArray = new JSONArray();
            for (ImageInfo info : images) {
                imageArray.put(info.toJson());
            }
            json.put("images", imageArray);
            
            String jsonString = json.toString(2);
            String cacheFilePath = cacheManager.getCacheFilePath(CACHE_TYPE);
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
            
            Log.i(TAG, String.format("背景图片索引缓存已保存，共 %d 个文件", images.size()));
            
        } catch (Exception e) {
            Log.e(TAG, "保存背景图片索引缓存失败", e);
        }
    }
    
    /**
     * 扫描背景图片目录并创建索引
     */
    public List<ImageInfo> scanAndCreateIndex() {
        List<ImageInfo> images = new ArrayList<>();
        
        try {
            File bgDir = new File(backgroundDir);
            if (!bgDir.exists() || !bgDir.isDirectory()) {
                Log.w(TAG, "背景图片目录不存在: " + backgroundDir);
                return images;
            }
            
            File[] files = bgDir.listFiles();
            if (files == null) {
                Log.w(TAG, "无法读取背景图片目录: " + backgroundDir);
                return images;
            }
            
            for (File file : files) {
                if (file.isFile() && isImageFile(file.getName())) {
                    ImageInfo info = new ImageInfo();
                    info.filename = file.getName();
                    info.fileSize = file.length();
                    info.lastModified = file.lastModified();
                    
                    // 这里可以添加图片尺寸读取逻辑
                    // info.宽度 = getImageWidth(file);
                    // info.高度 = getImageHeight(file);
                    
                    images.add(info);
                }
            }
            
            Log.i(TAG, String.format("扫描背景图片目录完成，找到 %d 个文件", images.size()));
            
        } catch (Exception e) {
            Log.e(TAG, "扫描背景图片目录失败", e);
        }
        
        return images;
    }
    
    /**
     * 获取或创建背景图片索引
     */
    public List<ImageInfo> getImageIndex() {
        // 首先尝试从缓存加载
        List<ImageInfo> cachedImages = loadCachedIndex();
        if (cachedImages != null) {
            return cachedImages;
        }
        
        // 缓存无效，重新扫描
        List<ImageInfo> scannedImages = scanAndCreateIndex();
        
        // 保存到缓存
        saveCachedIndex(scannedImages);
        
        return scannedImages;
    }
    
    /**
     * 强制刷新索引
     */
    public List<ImageInfo> refreshIndex() {
        // 清除旧缓存
        cacheManager.clearCache(CACHE_TYPE);
        
        // 重新扫描并缓存
        List<ImageInfo> images = scanAndCreateIndex();
        saveCachedIndex(images);
        
        return images;
    }
    
    /**
     * 检查是否为图片文件
     */
    private boolean isImageFile(String filename) {
        if (filename == null) return false;
        
        String lowerName = filename.toLowerCase();
        return lowerName.endsWith(".jpg") || 
               lowerName.endsWith(".jpeg") || 
               lowerName.endsWith(".png") || 
               lowerName.endsWith(".bmp") || 
               lowerName.endsWith(".webp");
    }
    
    /**
     * 获取缓存统计信息
     */
    public String getCacheStats() {
        try {
            if (!cacheManager.hasCacheFile(CACHE_TYPE)) {
                return "背景图片索引缓存: 无缓存";
            }
            
            long cacheSize = cacheManager.getCacheFileSize(CACHE_TYPE);
            List<ImageInfo> images = loadCachedIndex();
            int imageCount = images != null ? images.size() : 0;
            
            return String.format("背景图片索引缓存: %d 个文件, 缓存大小 %d 字节", 
                imageCount, cacheSize);
                
        } catch (Exception e) {
            return "背景图片索引缓存: 获取统计信息失败";
        }
    }
}
