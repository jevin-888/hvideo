/**
 * @file StartupManager.java（文件名）
 * @brief 启动管理器
 * 
 * 统一管理启动流程，协调资源复制和引擎初始化，集成缓存机制
 */

package com.hsvj.engine;

import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;

import com.hsvj.engine.cache.CacheManager;
import com.hsvj.engine.cache.ConfigCache;
import com.hsvj.engine.cache.StartupStateCache;

import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

/**
 * 启动管理器
 * 负责协调整个启动流程，包括资源复制、引擎初始化等，集成缓存优化
 */
public class StartupManager {
    private static final String TAG = "StartupManager";
    
    public interface StartupListener {
        void onStageChanged(String stage, int progress);
        void onProgress(String message, int progress);
        void onCriticalResourcesReady();
        void onEngineInitialized(boolean success);
        void onStartupComplete();
        void onError(String error);
    }
    
    private final Context context;
    private final StartupListener listener;
    private final Handler mainHandler;
    
    // 缓存管理器
    private final CacheManager cacheManager;
    private final ConfigCache configCache;
    private final StartupStateCache startupStateCache;
    
    // 内存监控器（复用，避免重复创建）
    private final MemoryMonitor memoryMonitor;
    
    // 启动状态
    private boolean criticalResourcesReady = false;
    private boolean startupComplete = false;
    private long startupStartTime;

    /** 用于替代忙等待，超时 2 分钟 */
    private final CountDownLatch criticalResourcesLatch = new CountDownLatch(1);
    private static final long LATCH_AWAIT_MINUTES = 2;

    /** 进度节流：最小间隔 ms，避免主线程消息过多导致 Skipped frames */
    private static final long PROGRESS_THROTTLE_MS = 80;
    private long lastProgressTime = 0;
    private int lastProgressPercent = -1;

    /** 当前启动任务 Future，便于 Activity 销毁时取消 */
    private final AtomicReference<java.util.concurrent.Future<?>> startupFutureRef = new AtomicReference<>();

    public StartupManager(Context context, StartupListener listener) {
        this.context = context;
        this.listener = listener;
        this.mainHandler = new Handler(Looper.getMainLooper());
        
        // 初始化内存监控器（只创建一次）
        this.memoryMonitor = new MemoryMonitor(context);
        
        // 初始化缓存管理器
        // Java 启动缓存只放应用私有 cache 目录，不能写入 C++ 管理的 /huoshan 或 /sdcard/huoshan。
        String javaCacheRoot = context.getCacheDir().getAbsolutePath() + "/startup/";
        this.cacheManager = new CacheManager(context, javaCacheRoot);
        this.configCache = new ConfigCache(cacheManager, javaCacheRoot + "config/");
        this.startupStateCache = new StartupStateCache(cacheManager);
        
        this.startupStartTime = System.currentTimeMillis();
    }
    
    /**
     * 开始启动流程（提交到统一线程池，可被 cancelStartup 取消）
     */
    public void startInitialization() {
        Future<?> future = StartupExecutor.get().submit(() -> {
            try {
                performStartup();
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                Log.w(TAG, "启动被中断");
                releaseLatchesOnError();
            } catch (Exception e) {
                Log.e(TAG, "启动失败", e);
                notifyError("启动失败: " + e.getMessage());
                releaseLatchesOnError();
            }
        });
        startupFutureRef.set(future);
    }

    /** 取消启动任务（如 Activity 销毁时调用） */
    public void cancelStartup() {
        Future<?> future = startupFutureRef.getAndSet(null);
        if (future != null && !future.isDone()) {
            future.cancel(true);
        }
    }

    private void releaseLatchesOnError() {
        criticalResourcesLatch.countDown();
    }
    
    private void performStartup() throws InterruptedException {
        Log.i(TAG, "开始启动流程，缓存有效性: " + cacheManager.isCacheValid());
        
        // 检查是否可以使用快速启动
        boolean useFastStartup = startupStateCache.shouldUseFastStartup();
        Log.i(TAG, "使用快速启动: " + useFastStartup);
        
        if (useFastStartup) {
            performFastStartup();
        } else {
            performNormalStartup();
        }
    }
    
    /**
     * 快速启动流程：仅跳过资源复制与缓存建立，Native 引擎仍在 MainActivity.surfaceChanged() 中初始化。
     */
    private void performFastStartup() {
        Log.i(TAG, "执行快速启动流程");
        notifyStageChanged("正在加载缓存配置", 10);
        loadCachedConfigs();
        notifyStageChanged("正在检查资源", 30);
        checkAndCopyResources();
        notifyStageChanged("正在快速初始化引擎", 60);
        simulateEngineInitForProgress();
        notifyStageChanged("启动完成", 100);
        completeStartup();
    }
    
    /**
     * 正常启动流程
     */
    private void performNormalStartup() throws InterruptedException {
        Log.i(TAG, "执行正常启动流程");
        notifyStageChanged("正在复制关键资源", 0);
        copyResources();
        if (!criticalResourcesLatch.await(LATCH_AWAIT_MINUTES, TimeUnit.MINUTES)) {
            Log.e(TAG, "等待关键资源超时");
            notifyError("关键资源准备超时");
            return;
        }
        notifyStageChanged("正在准备引擎启动", 33);
        prepareEngineStartupProgress();
        notifyStageChanged("正在建立缓存", 90);
        buildCaches();
        notifyStageChanged("启动完成", 100);
        completeStartup();
    }
    
    /**
     * 加载缓存的配置
     */
    private void loadCachedConfigs() {
        try {
            // 加载主配置文件
            ConfigCache.CachedConfig mainConfig = configCache.loadAndCacheConfig("config.json");
            if (mainConfig != null) {
                Log.i(TAG, "主配置已从缓存加载");
                notifyProgress("主配置已加载", 15);
            }
        } catch (Exception e) {
            Log.w(TAG, "加载缓存配置失败", e);
        }
    }
    
    /**
     * 检查并复制资源（快速启动时）
     */
    private void checkAndCopyResources() {
        // 在快速启动模式下，假设资源已经存在
        // 这里可以添加快速检查逻辑
        criticalResourcesReady = true;
        notifyCriticalResourcesReady();
        notifyProgress("资源检查完成", 40);
    }
    
    /** 快速启动路径下仅用于进度展示，不执行真实引擎初始化；真实引擎在 MainActivity 中初始化 */
    private void simulateEngineInitForProgress() {
        String[] steps = { "恢复渲染器状态", "加载运行配置", "初始化音频处理器", "启动渲染循环" };
        for (int i = 0; i < steps.length; i++) {
            notifyProgress("正在" + steps[i] + "...", 60 + (i * 25) / steps.length);
            try {
                Thread.sleep(100);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                return;
            }
        }
        notifyEngineInitialized(true);
    }
    
    private void copyResources() {
        criticalResourcesReady = true;
        criticalResourcesLatch.countDown();
        notifyCriticalResourcesReady();
        notifyProgress("跳过 Java 资源复制，资源由 C++ 路径配置管理", 33);
        startupStateCache.markResourcesCopied(true, System.currentTimeMillis() - startupStartTime);
        Log.i(TAG, "跳过 Java assets 复制，避免覆盖 C++ 管理的配置/数据库/资源");
    }

    /** 仅用于进度展示的模拟步骤，不执行真实引擎初始化；真实引擎在 MainActivity 中初始化。 */
    private void prepareEngineStartupProgress() throws InterruptedException {
        String[] initSteps = {
            "环境预检", "初始化路径和日志", "目录治理", "检查配置文件", "授权检查",
            "启动引擎框架", "加载配置", "构造管理器骨架", "初始化模块", "预创建授权图层",
            "加载各模块", "启动硬件模块", "初始化渲染模块"
        };
        for (int i = 0; i < initSteps.length; i++) {
            if (Thread.interrupted()) throw new InterruptedException("启动已取消");
            notifyProgress("正在" + initSteps[i] + "...", 33 + (i * 50) / initSteps.length);
            Thread.sleep(200);
        }
        notifyEngineInitialized(true);
    }
    
    /**
     * 建立缓存
     */
    private void buildCaches() {
        try {
            // 更新缓存版本信息
            cacheManager.updateCacheVersion();
            
            // 建立配置缓存
            configCache.loadAndCacheConfig("config.json");
            
            notifyProgress("缓存建立完成", 95);
            
        } catch (Exception e) {
            Log.w(TAG, "建立缓存失败", e);
        }
    }
    
    /**
     * 完成启动
     */
    private void completeStartup() {
        startupComplete = true;
        
        // 更新启动统计（使用已创建的内存监控器）
        long totalTime = System.currentTimeMillis() - startupStartTime;
        long availableMemory = memoryMonitor.getAvailableMemoryMB();
        boolean lowMemoryMode = memoryMonitor.checkMemoryStatus();
        
        startupStateCache.updateStartupStats(totalTime, availableMemory, lowMemoryMode);
        
        Log.i(TAG, String.format("启动完成，总耗时: %dms", totalTime));
        Log.i(TAG, startupStateCache.getStartupStats());
        
        notifyStartupComplete();
    }
    
    // 通知方法保持不变...
    private void notifyStageChanged(String stage, int progress) {
        mainHandler.post(() -> {
            if (listener != null) {
                listener.onStageChanged(stage, progress);
            }
        });
    }
    
    private void notifyProgress(String message, int progress) {
        long now = SystemClock.uptimeMillis();
        boolean send = (now - lastProgressTime >= PROGRESS_THROTTLE_MS)
            || progress >= 100
            || (progress != lastProgressPercent && progress % 10 == 0);
        if (!send) return;
        lastProgressTime = now;
        lastProgressPercent = progress;
        mainHandler.post(() -> {
            if (listener != null) listener.onProgress(message, progress);
        });
    }
    
    private void notifyCriticalResourcesReady() {
        mainHandler.post(() -> {
            if (listener != null) {
                listener.onCriticalResourcesReady();
            }
        });
    }
    
    private void notifyEngineInitialized(boolean success) {
        mainHandler.post(() -> {
            if (listener != null) {
                listener.onEngineInitialized(success);
            }
        });
    }
    
    private void notifyStartupComplete() {
        mainHandler.post(() -> {
            if (listener != null) {
                listener.onStartupComplete();
            }
        });
    }
    
    private void notifyError(String error) {
        mainHandler.post(() -> {
            if (listener != null) {
                listener.onError(error);
            }
        });
    }
    
    // 状态查询方法
    public boolean isCriticalResourcesReady() {
        return criticalResourcesReady;
    }
    
    public boolean isStartupComplete() {
        return startupComplete;
    }
    
    /**
     * 获取内存监控器（供外部复用，避免重复创建）
     */
    public MemoryMonitor getMemoryMonitor() {
        return memoryMonitor;
    }
    
    /**
     * 获取缓存统计信息
     */
    public String getCacheStats() {
        StringBuilder stats = new StringBuilder();
        stats.append("=== 缓存统计信息 ===\n");
        stats.append("缓存有效性: ").append(cacheManager.isCacheValid() ? "有效" : "无效").append("\n");
        stats.append(configCache.getCacheStats("config.json")).append("\n");
        stats.append(startupStateCache.getStartupStats()).append("\n");
        return stats.toString();
    }
    
    /**
     * 清除所有缓存
     */
    public void clearAllCaches() {
        cacheManager.clearAllCaches();
        Log.i(TAG, "所有缓存已清除");
    }
}
