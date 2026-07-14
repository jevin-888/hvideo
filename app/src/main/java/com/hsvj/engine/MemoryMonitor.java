/**
 * @file MemoryMonitor.java（文件名）
 * @brief 内存监控和低内存模式管理
 * 
 * 负责：
 * - 监控系统可用内存
 * - 决定是否启用低内存模式
 * - 提供内存状态查询接口
 */

package com.hsvj.engine;

import android.app.ActivityManager;
import android.content.Context;
import android.util.Log;

public class MemoryMonitor {
    private static final String TAG = "MemoryMonitor";
    
    // 内存阈值（MB）
    private static final long LOW_MEMORY_THRESHOLD_MB = 150;
    private static final long CRITICAL_MEMORY_THRESHOLD_MB = 100;
    
    private final Context context;
    private final ActivityManager activityManager;
    private boolean lowMemoryMode = false;
    
    public MemoryMonitor(Context context) {
        this.context = context;
        this.activityManager = (ActivityManager) context.getSystemService(Context.ACTIVITY_SERVICE);
    }
    
    /**
     * 检查当前内存状态并决定是否启用低内存模式
     * @return true 如果应该启用低内存模式
     */
    public boolean checkMemoryStatus() {
        ActivityManager.MemoryInfo memInfo = new ActivityManager.MemoryInfo();
        activityManager.getMemoryInfo(memInfo);
        
        long availMB = memInfo.availMem / (1024 * 1024);
        long totalMB = memInfo.totalMem / (1024 * 1024);
        
        Log.i(TAG, String.format("内存状态: 可用=%dMB, 总计=%dMB, 低内存=%b", 
            availMB, totalMB, memInfo.lowMemory));
        
        // 系统标记为低内存或可用内存低于阈值
        if (memInfo.lowMemory || availMB < LOW_MEMORY_THRESHOLD_MB) {
            lowMemoryMode = true;
            Log.w(TAG, "启用低内存模式 (可用内存: " + availMB + "MB)");
            return true;
        }
        
        lowMemoryMode = false;
        return false;
    }
    
    /**
     * 获取当前可用内存（MB）
     */
    public long getAvailableMemoryMB() {
        ActivityManager.MemoryInfo memInfo = new ActivityManager.MemoryInfo();
        activityManager.getMemoryInfo(memInfo);
        return memInfo.availMem / (1024 * 1024);
    }
    
    /**
     * 获取总内存（MB）
     */
    public long getTotalMemoryMB() {
        ActivityManager.MemoryInfo memInfo = new ActivityManager.MemoryInfo();
        activityManager.getMemoryInfo(memInfo);
        return memInfo.totalMem / (1024 * 1024);
    }
    
    /**
     * 是否处于低内存模式
     */
    public boolean isLowMemoryMode() {
        return lowMemoryMode;
    }
    
    /**
     * 是否处于临界内存状态
     */
    public boolean isCriticalMemory() {
        return getAvailableMemoryMB() < CRITICAL_MEMORY_THRESHOLD_MB;
    }
    
    /**
     * 请求系统GC（谨慎使用）
     */
    public void requestGC() {
        Log.i(TAG, "请求系统GC");
        System.gc();
    }
}
