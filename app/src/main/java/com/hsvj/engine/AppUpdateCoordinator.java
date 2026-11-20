package com.hsvj.engine;

import android.app.AlertDialog;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkRequest;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Window;
import android.view.WindowManager;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;
import android.widget.Toast;

import com.hsvj.engine.update.AppUpdateManager;
import com.hsvj.engine.update.RomUpdateManager;

import org.json.JSONObject;

import java.io.File;
import java.io.FileInputStream;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/** Owns application-update policy, dialogs, download progress, and installer handoff. */
final class AppUpdateCoordinator {
    private static final String TAG = "HSVJUpdate";
    private static final long PERIODIC_CHECK_MS = 30 * 60 * 1000L;

    private final MainActivity activity;
    private final Handler retryHandler = new Handler(Looper.getMainLooper());
    private int checkAttempts;
    private volatile boolean destroyed;
    private boolean checkInProgress;
    private final Runnable scheduledCheck = this::checkForUpdate;
    private final ExecutorService configWorker = Executors.newSingleThreadExecutor();
    private ConnectivityManager connectivityManager;
    private ConnectivityManager.NetworkCallback networkCallback;
    private AppUpdateManager updateManager;
    private final RomUpdateManager romUpdateManager = new RomUpdateManager();
    private AlertDialog downloadProgressDialog;
    private ProgressBar downloadProgressBar;
    private TextView downloadProgressText;
    private TextView downloadProgressPercentText;

    AppUpdateCoordinator(MainActivity activity) {
        this.activity = activity;
        initUpdateManager();
    }

    private void initUpdateManager() {
        updateManager = new AppUpdateManager(activity);
        updateManager.setUpdateListener(new AppUpdateManager.UpdateListener() {
            @Override
            public void onCheckStart() {
                Log.i(TAG, "[更新检查] 开始检查更新...");
            }

            @Override
            public void onNewVersionAvailable(String versionName, String versionCode,
                                             String downloadUrl, String releaseNotes, boolean forceUpdate) {
                Log.i(TAG, "[更新检查] 发现新版本: " + versionName + "(" + versionCode + "), forceUpdate=" + forceUpdate);
                activity.runOnUiThread(() -> {
                    if (destroyed || updateManager == null) return;
                    Toast.makeText(activity, "发现播放器新版，开始自动下载...", Toast.LENGTH_SHORT).show();
                    updateManager.downloadApk(downloadUrl);
                });
            }

            @Override
            public void onNoNewVersion(String currentVersionName, int currentVersionCode,
                                       String serverVersionName, int serverVersionCode, boolean forceUpdate) {
                Log.i(TAG, "[更新检查] 当前已是最新版本 current=" + currentVersionName + "(" + currentVersionCode + ")"
                        + ", server=" + serverVersionName + "(" + serverVersionCode + ")"
                        + ", forceUpdate=" + forceUpdate);
                finishCheck(false);
            }

            @Override
            public void onCheckFailed(String error) {
                Log.w(TAG, "[更新检查] 检查更新失败: " + error);
                scheduleRetry();
            }

            @Override
            public void onDownloadStart(long totalBytes) {
                Log.i(TAG, "[更新下载] 开始下载更新...");
                activity.runOnUiThread(() -> { if (!destroyed) showDownloadProgressDialog(); });
            }

            @Override
            public void onDownloadProgress(long downloadedBytes, long totalBytes, int progress) {
                activity.runOnUiThread(() -> { if (!destroyed) updateDownloadProgress(progress); });
            }

            @Override
            public void onDownloadComplete(Uri fileUri) {
                Log.i(TAG, "[更新下载] 下载完成，准备安装");
                activity.runOnUiThread(() -> {
                    hideDownloadProgressDialog();
                    // 自动安装
                    if (updateManager != null) {
                        updateManager.installApk(fileUri);
                    }
                });
            }

            @Override
            public void onDownloadFailed(String error) {
                Log.e(TAG, "[更新下载] 下载失败: " + error);
                activity.runOnUiThread(() -> {
                    hideDownloadProgressDialog();
                    if (destroyed) return;
                    Toast.makeText(activity, "下载失败: " + error, Toast.LENGTH_LONG).show();
                });
                scheduleRetry();
            }

            @Override
            public void onInstallStart() {
                Log.i(TAG, "[更新安装] 开始安装更新...");
                activity.runOnUiThread(() -> Toast.makeText(activity, "正在安装更新...", Toast.LENGTH_SHORT).show());
            }
        });
    }

    /** Native ready: local ROM policy is preserved; APP checks always include cloud. */
    void checkForUpdate() {
        if (destroyed || updateManager == null || checkInProgress) return;
        checkInProgress = true;
        checkAttempts++;
        retryHandler.removeCallbacks(scheduledCheck);
        watchNetworks();
        configWorker.execute(() -> {
            try {
                JSONObject config = readConfig();
                JSONObject device = new JSONObject(HSVJEngine.getAppUpdateDeviceInfo());
                String cloudBase = config.optString("licenseServerUrl", "").trim();
                if (cloudBase.isEmpty()) cloudBase = device.getString("cloud_url");
                String host = config.optString("onlineVodHost", "").trim();
                if (!host.isEmpty() && !host.matches("[A-Za-z0-9.-]+")) {
                    Log.w(TAG, "[更新检查] 点歌服务器地址无效，仍检查云端");
                    host = "";
                }
                String localOrigin = host.isEmpty() ? "" : "http://" + host + ":9898";
                String localCheck = localOrigin.isEmpty() ? "" : localOrigin + "/api/v1/player-updates/check";
                String cloud = cloudBase;
                Runnable checkApp = () -> activity.runOnUiThread(() -> {
                    hideDownloadProgressDialog();
                    if (destroyed || updateManager == null) return;
                    if (config.optBoolean("appUpdateEnabled", true)) {
                        Log.i(TAG, "[更新检查] 本地=" + localCheck + "，云端=" + cloud);
                        updateManager.checkForUpdate(localCheck, cloud, device);
                    } else {
                        finishCheck(false);
                    }
                });
                if (!localOrigin.isEmpty() && config.optBoolean("romUpdateEnabled", true)) {
                    romUpdateManager.check(localOrigin, new RomUpdateManager.Listener() {
                        @Override public void onProgress(String text, int percent) {
                            activity.runOnUiThread(() -> {
                                if (destroyed) return;
                                if (downloadProgressDialog == null) showDownloadProgressDialog();
                                updateDownloadProgress(percent);
                                if (downloadProgressText != null) downloadProgressText.setText(text);
                            });
                        }
                        @Override public void onFinished() { checkApp.run(); }
                        @Override public void onHandoffFailed() {
                            activity.runOnUiThread(() -> {
                                hideDownloadProgressDialog();
                                if (!destroyed) Toast.makeText(activity, "系统升级启动失败，请检查升级日志", Toast.LENGTH_LONG).show();
                            });
                            // Keep APK updates paused while a ROM handoff is unresolved.
                        }
                    });
                } else {
                    checkApp.run();
                }
            } catch (Exception error) {
                Log.w(TAG, "[更新检查] 读取更新配置失败", error);
                scheduleRetry();
            }
        });
    }

    private JSONObject readConfig() throws Exception {
        File file = new File(PathConfig.getRootPath(activity), "config/config.json");
        if (!file.isFile()) return new JSONObject();
        if (file.length() > 2 * 1024 * 1024) throw new IllegalStateException("配置文件过大");
        byte[] data = new byte[(int) file.length()];
        try (FileInputStream input = new FileInputStream(file)) {
            int offset = 0;
            while (offset < data.length) {
                int count = input.read(data, offset, data.length - offset);
                if (count < 0) throw new java.io.IOException("配置文件不完整");
                offset += count;
            }
        }
        return new JSONObject(new String(data, StandardCharsets.UTF_8));
    }

    private void scheduleRetry() { finishCheck(true); }

    private void finishCheck(boolean retry) {
        retryHandler.post(() -> {
            checkInProgress = false;
            if (destroyed) return;
            retryHandler.removeCallbacks(scheduledCheck);
            long delay = retry && checkAttempts < 5 ? 30000 : PERIODIC_CHECK_MS;
            if (delay == PERIODIC_CHECK_MS) checkAttempts = 0;
            retryHandler.postDelayed(scheduledCheck, delay);
        });
    }

    /** Reconnects resume checks without changing the process-wide default network. */
    private void watchNetworks() {
        if (networkCallback != null) return;
        try {
            connectivityManager = activity.getSystemService(ConnectivityManager.class);
            if (connectivityManager == null) return;
            ConnectivityManager.NetworkCallback callback = new ConnectivityManager.NetworkCallback() {
                @Override public void onAvailable(Network network) {
                    retryHandler.post(() -> {
                        if (destroyed || checkInProgress) return;
                        checkAttempts = 0;
                        retryHandler.removeCallbacks(scheduledCheck);
                        retryHandler.postDelayed(scheduledCheck, 5000);
                    });
                }
            };
            NetworkRequest request = new NetworkRequest.Builder()
                    .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
                    .addTransportType(NetworkCapabilities.TRANSPORT_ETHERNET).build();
            connectivityManager.registerNetworkCallback(request, callback);
            networkCallback = callback;
        } catch (Exception error) {
            Log.w(TAG, "更新网络监听不可用，继续定时检查", error);
        }
    }

    /**
     * 显示下载进度对话框
     */
    private void showDownloadProgressDialog() {
        if (activity.isFinishing() || activity.isDestroyed()) {
            return;
        }

        if (downloadProgressDialog == null) {
            LinearLayout layout = new LinearLayout(activity);
            layout.setOrientation(LinearLayout.VERTICAL);
            float density = activity.getResources().getDisplayMetrics().density;
            int paddingH = (int) (48 * density);
            int paddingV = (int) (36 * density);
            layout.setPadding(paddingH, paddingV, paddingH, paddingV);

            GradientDrawable background = new GradientDrawable(
                    GradientDrawable.Orientation.TOP_BOTTOM,
                    new int[] { Color.rgb(30, 34, 42), Color.rgb(16, 19, 26) });
            background.setCornerRadius(24 * density);
            background.setStroke((int) (1 * density), Color.argb(120, 90, 210, 255));
            layout.setBackground(background);

            TextView titleText = new TextView(activity);
            titleText.setText("正在更新系统");
            titleText.setTextColor(Color.WHITE);
            titleText.setTextSize(24);
            titleText.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
            layout.addView(titleText, new LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT,
                    LinearLayout.LayoutParams.WRAP_CONTENT));

            downloadProgressText = new TextView(activity);
            downloadProgressText.setText("正在下载新版本，请勿断电或退出");
            downloadProgressText.setTextColor(Color.rgb(190, 199, 214));
            downloadProgressText.setTextSize(15);
            LinearLayout.LayoutParams descParams = new LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT,
                    LinearLayout.LayoutParams.WRAP_CONTENT);
            descParams.topMargin = (int) (12 * density);
            descParams.bottomMargin = (int) (24 * density);
            layout.addView(downloadProgressText, descParams);

            downloadProgressPercentText = new TextView(activity);
            downloadProgressPercentText.setText("0%");
            downloadProgressPercentText.setTextColor(Color.rgb(88, 214, 255));
            downloadProgressPercentText.setTextSize(42);
            downloadProgressPercentText.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
            downloadProgressPercentText.setGravity(android.view.Gravity.CENTER);
            layout.addView(downloadProgressPercentText, new LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT,
                    LinearLayout.LayoutParams.WRAP_CONTENT));

            downloadProgressBar = new ProgressBar(activity, null, android.R.attr.progressBarStyleHorizontal);
            downloadProgressBar.setMax(100);
            LinearLayout.LayoutParams barParams = new LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT,
                    (int) (18 * density));
            barParams.topMargin = (int) (22 * density);
            layout.addView(downloadProgressBar, barParams);

            downloadProgressDialog = new AlertDialog.Builder(activity)
                    .setView(layout)
                    .create();
            downloadProgressDialog.setCancelable(false);
            downloadProgressDialog.setOnShowListener(dialog -> {
                Window window = downloadProgressDialog.getWindow();
                if (window != null) {
                    window.setBackgroundDrawableResource(android.R.color.transparent);
                    WindowManager.LayoutParams params = new WindowManager.LayoutParams();
                    params.copyFrom(window.getAttributes());
                    params.width = (int) (620 * density);
                    params.height = WindowManager.LayoutParams.WRAP_CONTENT;
                    window.setAttributes(params);
                    window.setDimAmount(0.72f);
                }
            });
        }

        if (downloadProgressBar != null) {
            downloadProgressBar.setProgress(0);
        }
        if (downloadProgressText != null) {
            downloadProgressText.setText("正在下载新版本，请勿断电或退出");
        }
        if (downloadProgressPercentText != null) {
            downloadProgressPercentText.setText("0%");
        }
        downloadProgressDialog.show();
    }

    /**
     * 更新下载进度
     */
    private void updateDownloadProgress(int progress) {
        if (downloadProgressDialog != null && downloadProgressDialog.isShowing()) {
            if (downloadProgressBar != null) {
                downloadProgressBar.setProgress(progress);
            }
            if (downloadProgressText != null) {
                downloadProgressText.setText(progress >= 100 ? "下载完成，正在准备安装..." : "正在下载新版本，请勿断电或退出");
            }
            if (downloadProgressPercentText != null) {
                downloadProgressPercentText.setText(progress + "%");
            }
        }
    }

    /**
     * 隐藏下载进度对话框
     */
    private void hideDownloadProgressDialog() {
        if (downloadProgressDialog != null && downloadProgressDialog.isShowing()) {
            downloadProgressDialog.dismiss();
        }
        downloadProgressDialog = null;
        downloadProgressBar = null;
        downloadProgressText = null;
        downloadProgressPercentText = null;
    }

    void destroy() {
        destroyed = true;
        if (connectivityManager != null && networkCallback != null) {
            connectivityManager.unregisterNetworkCallback(networkCallback);
            networkCallback = null;
        }
        configWorker.shutdownNow();
        romUpdateManager.destroy();
        retryHandler.removeCallbacksAndMessages(null);
        hideDownloadProgressDialog();
        if (updateManager != null) {
            updateManager.destroy();
        }
        updateManager = null;
    }
}
