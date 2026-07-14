/**
 * @file UpdateExample.java（文件名）
 * @brief App更新功能集成示例代码
 * 
 * 这是一个完整的示例，展示如何在Activity中集成AppUpdate管理器
 */

package com.hsvj.engine.update;

import android.app.AlertDialog;
import android.app.ProgressDialog;
import android.content.Context;
import android.net.Uri;
import android.os.Bundle;
import android.util.Log;
import android.widget.Toast;

import androidx.appcompat.app.AppCompatActivity;

/**
 * 更新功能集成示例
 * 
 * 使用方法：
 * 1. 复制此代码到你的Activity中
 * 2. 修改CHECK_URL为实际的服务器地址
 * 3. 在onCreate中调用 initUpdate管理器()
 * 4. 在合适的时机调用 checkForUpdate()
 */
@SuppressWarnings("deprecation")
public class UpdateExample extends AppCompatActivity {
    
    private static final String TAG = "UpdateExample";
    
    // TODO: 修改为实际的版本检查API地址
    private static final String CHECK_URL = "https://your-server.com/api/version/check";
    
    private AppUpdateManager updateManager;
    private ProgressDialog downloadProgressDialog;
    
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        
        // 初始化更新管理器
        initUpdateManager();
        
        // 检查更新（通常在应用启动时调用）
        checkForUpdate();
    }
    
    /**
     * 初始化更新管理器
     */
    private void initUpdateManager() {
        updateManager = new AppUpdateManager(this);
        
        // 设置监听器
        updateManager.setUpdateListener(new AppUpdateManager.UpdateListener() {
            @Override
            public void onCheckStart() {
                Log.i(TAG, "开始检查更新...");
            }

            @Override
            public void onNewVersionAvailable(String versionName, String versionCode, 
                                             String downloadUrl, String releaseNotes, boolean forceUpdate) {
                Log.i(TAG, "发现新版本: " + versionName + "(" + versionCode + "), forceUpdate=" + forceUpdate);
                showUpdateDialog(versionName, releaseNotes, downloadUrl);
            }

            @Override
            public void onNoNewVersion(String currentVersionName, int currentVersionCode,
                                       String serverVersionName, int serverVersionCode, boolean forceUpdate) {
                Log.i(TAG, "当前已是最新版本 current=" + currentVersionName + "(" + currentVersionCode + ")"
                        + ", server=" + serverVersionName + "(" + serverVersionCode + ")"
                        + ", forceUpdate=" + forceUpdate);
                // 可选：显示"已是最新版本"的提示
                // Toast.makeText(UpdateExample.this, "已是最新版本", Toast.LENGTH_SHORT).show();
            }

            @Override
            public void onCheckFailed(String error) {
                Log.e(TAG, "检查更新失败: " + error);
                // 可选：显示错误提示
                // Toast.makeText(UpdateExample.this, "检查更新失败", Toast.LENGTH_SHORT).show();
            }

            @Override
            public void onDownloadStart(long totalBytes) {
                Log.i(TAG, "开始下载更新...");
                showDownloadProgressDialog();
            }

            @Override
            public void onDownloadProgress(long downloadedBytes, long totalBytes, int progress) {
                updateDownloadProgress(progress);
            }

            @Override
            public void onDownloadComplete(Uri fileUri) {
                Log.i(TAG, "下载完成，准备安装");
                hideDownloadProgressDialog();
                
                // 自动安装
                updateManager.installApk(fileUri);
            }

            @Override
            public void onDownloadFailed(String error) {
                Log.e(TAG, "下载失败: " + error);
                hideDownloadProgressDialog();
                Toast.makeText(UpdateExample.this, "下载失败: " + error, Toast.LENGTH_LONG).show();
            }

            @Override
            public void onInstallStart() {
                Log.i(TAG, "开始安装更新...");
                Toast.makeText(UpdateExample.this, "正在安装更新...", Toast.LENGTH_SHORT).show();
            }
        });
    }
    
    /**
     * 检查更新
     */
    public void checkForUpdate() {
        if (updateManager == null) {
            return;
        }
        
        // 检查是否需要更新（基于时间间隔，避免频繁检查）
        if (updateManager.shouldCheckForUpdate()) {
            updateManager.checkForUpdate(CHECK_URL);
        } else {
            Log.i(TAG, "未到检查时间，跳过更新检查");
        }
    }
    
    /**
     * 手动检查更新（用户点击按钮时调用）
     */
    public void manualCheckForUpdate() {
        if (updateManager == null) {
            return;
        }
        
        // 手动检查时忽略时间间隔
        updateManager.checkForUpdate(CHECK_URL);
    }
    
    /**
     * 显示更新对话框
     */
    private void showUpdateDialog(String versionName, String releaseNotes, String downloadUrl) {
        AlertDialog.Builder builder = new AlertDialog.Builder(this);
        builder.setTitle("发现新版本 " + versionName);
        
        // 显示更新说明
        if (releaseNotes != null && !releaseNotes.isEmpty()) {
            builder.setMessage("更新内容：\n" + releaseNotes);
        } else {
            builder.setMessage("发现新版本，建议更新以获得更好的体验。");
        }
        
        // 立即下载按钮
        builder.setPositiveButton("立即下载", (dialog, which) -> {
            String fileName = "hsvj-engine-v" + versionName + ".apk";
            updateManager.downloadApk(downloadUrl, fileName);
        });
        
        // 跳过此版本按钮
        builder.setNeutralButton("跳过此版本", (dialog, which) -> {
            updateManager.skipVersion(versionName);
            dialog.dismiss();
        });
        
        // 稍后再说按钮
        builder.setNegativeButton("稍后再说", (dialog, which) -> {
            dialog.dismiss();
        });
        
        builder.setCancelable(true);
        builder.show();
    }
    
    /**
     * 显示下载进度对话框
     */
    private void showDownloadProgressDialog() {
        if (isFinishing() || isDestroyed()) {
            return;
        }
        
        if (downloadProgressDialog == null) {
            downloadProgressDialog = new ProgressDialog(this);
            downloadProgressDialog.setTitle("下载更新");
            downloadProgressDialog.setMessage("正在下载新版本...");
            downloadProgressDialog.setProgressStyle(ProgressDialog.STYLE_HORIZONTAL);
            downloadProgressDialog.setMax(100);
            downloadProgressDialog.setCancelable(false);
            downloadProgressDialog.setButton("取消", (dialog, which) -> {
                if (updateManager != null) {
                    updateManager.cancelDownload();
                }
            });
        }
        
        downloadProgressDialog.setProgress(0);
        downloadProgressDialog.show();
    }
    
    /**
     * 更新下载进度
     */
    private void updateDownloadProgress(int progress) {
        if (downloadProgressDialog != null && downloadProgressDialog.isShowing()) {
            downloadProgressDialog.setProgress(progress);
        }
    }
    
    /**
     * 隐藏下载进度对话框
     */
    private void hideDownloadProgressDialog() {
        if (downloadProgressDialog != null && downloadProgressDialog.isShowing()) {
            downloadProgressDialog.dismiss();
        }
    }
    
    @Override
    protected void onResume() {
        super.onResume();
        // 应用恢复时检查更新
        checkForUpdate();
    }
    
    @Override
    protected void onDestroy() {
        super.onDestroy();
        // 清理资源
        if (downloadProgressDialog != null) {
            downloadProgressDialog.dismiss();
            downloadProgressDialog = null;
        }
    }
}
