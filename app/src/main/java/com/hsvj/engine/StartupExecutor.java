/**
 * @file StartupExecutor.java（文件名）
 * @brief 启动阶段统一线程池
 *
 * 集中管理启动相关后台任务，支持取消与生命周期绑定，避免分散 new Thread()。
 */

package com.hsvj.engine;

import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.ThreadFactory;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * 启动专用线程池单例。
 * 用于 Startup管理器 与启动阶段后台任务，便于在 Activity 销毁时取消任务。
 */
public final class StartupExecutor {
    private static final int CORE_POOL_SIZE = 2;
    private static volatile StartupExecutor sInstance;
    private static final Object LOCK = new Object();

    private final ExecutorService executor;
    private final AtomicInteger threadNumber = new AtomicInteger(0);

    private StartupExecutor() {
        ThreadFactory factory = r -> {
            Thread t = new Thread(r, "StartupWorker-" + threadNumber.incrementAndGet());
            t.setPriority(Thread.NORM_PRIORITY);
            return t;
        };
        this.executor = Executors.newFixedThreadPool(CORE_POOL_SIZE, factory);
    }

    /**
     * 获取单例（懒加载）
     */
    public static StartupExecutor get() {
        if (sInstance == null) {
            synchronized (LOCK) {
                if (sInstance == null) {
                    sInstance = new StartupExecutor();
                }
            }
        }
        return sInstance;
    }

    /** 供启动阶段延后任务使用 */
    public ExecutorService getExecutor() {
        return executor;
    }

    /**
     * 提交任务，返回 Future 便于取消
     */
    public Future<?> submit(Runnable task) {
        return executor.submit(task);
    }

    /**
     * 执行任务（不关心返回值与取消时使用）
     */
    public void execute(Runnable task) {
        executor.execute(task);
    }

    /**
     * 立即关闭线程池并尝试中断正在执行的任务。
     * 仅在应用进程退出或确定不再需要启动任务时调用，一般由 Activity 取消单个 Future 即可。
     */
    public void shutdownNow() {
        executor.shutdownNow();
    }
}
