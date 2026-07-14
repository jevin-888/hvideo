/**
 * @file AppUpdateManager.java（文件名）
 * @brief App版本更新管理器
 * 
 * 负责检查新版本、下载APK、安装更新
 */

package com.hsvj.engine.update;

import android.app.DownloadManager;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.util.Log;

import androidx.core.content.FileProvider;

import com.hsvj.engine.BuildConfig;

import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.BufferedReader;
import java.net.HttpURLConnection;
import java.net.URL;
import java.security.MessageDigest;
import java.util.Locale;

/**
 * App更新管理器
 * 支持检查版本、下载APK、安装更新
 */
public class AppUpdateManager {
    private static final String TAG = "AppUpdateManager";
    private static final String PREFS_NAME = "app_update_prefs";
    private static final String KEY_LAST_CHECK_TIME = "last_check_time";
    private static final String KEY_SKIP_VERSION = "skip_version";
    private static final String[] LEGACY_PACKAGE_NAMES = {
            "com.huoshan.vjplayer",
            "com.huoshan.jumukz"
    };
    
    // 更新检查间隔（毫秒）- 默认6小时
    private static final long CHECK_INTERVAL_MS = 6 * 60 * 60 * 1000;
    
    private final Context context;
    private final SharedPreferences prefs;
    private UpdateListener listener;
    private long downloadId = -1;
    private String lastDownloadFilePath;
    
    // 下载管理器
    private DownloadManager downloadManager;
    
    /**
     * 更新监听器接口
     */
    public interface UpdateListener {
        void onCheckStart();
        void onNewVersionAvailable(String versionName, String versionCode, String downloadUrl, String releaseNotes, boolean forceUpdate);
        void onNoNewVersion(String currentVersionName, int currentVersionCode, String serverVersionName, int serverVersionCode, boolean forceUpdate);
        void onCheckFailed(String error);
        void onDownloadStart(long totalBytes);
        void onDownloadProgress(long downloadedBytes, long totalBytes, int progress);
        void onDownloadComplete(Uri fileUri);
        void onDownloadFailed(String error);
        void onInstallStart();
    }
    
    public AppUpdateManager(Context context) {
        this.context = context.getApplicationContext();
        this.prefs = this.context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE);
        this.downloadManager = (DownloadManager) this.context.getSystemService(Context.DOWNLOAD_SERVICE);
    }
    
    /**
     * 设置更新监听器
     */
    public void setUpdateListener(UpdateListener listener) {
        this.listener = listener;
    }
    
    /**
     * 检查是否需要更新（基于时间间隔）
     */
    public boolean shouldCheckForUpdate() {
        long lastCheckTime = prefs.getLong(KEY_LAST_CHECK_TIME, 0);
        long currentTime = System.currentTimeMillis();
        return (currentTime - lastCheckTime) >= CHECK_INTERVAL_MS;
    }
    
    /**
     * 检查新版本
     * @param checkUrl 版本检查接口URL
     */
    public void checkForUpdate(final String checkUrl) {
        if (listener != null) {
            listener.onCheckStart();
        }
        
        new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    // 记录检查时间
                    prefs.edit().putLong(KEY_LAST_CHECK_TIME, System.currentTimeMillis()).apply();
                    
                    // 获取当前版本信息
                    PackageInfo packageInfo = context.getPackageManager()
                            .getPackageInfo(context.getPackageName(), 0);
                    String currentVersionName = packageInfo.versionName;
                    int currentVersionCode = getPackageVersionCode(packageInfo);
                    
                    String hardware = BuildConfig.HARDWARE;
                    String signingSha256 = getCurrentSigningSha256();

                    Log.i(TAG, "当前版本: " + currentVersionName + " (" + currentVersionCode + ")"
                            + ", hardware=" + hardware + ", signingSha256=" + signingSha256);
                    
                    // 从服务器获取版本信息（POST请求，发送当前版本号）
                    String response = fetchVersionInfo(checkUrl, currentVersionCode, currentVersionName, hardware, signingSha256);
                    if (response == null) {
                        Log.i(TAG, "未获取到版本信息，跳过本次更新检查");
                        return;
                    }
                    
                    // 解析服务器响应
                    JSONObject json = new JSONObject(response);

                    String serverVersionName = json.optString("version_name", json.optString("versionName", ""));
                    int serverVersionCode = json.optInt("version_code", json.optInt("versionCode", 0));
                    String downloadUrl = json.optString("download_url", json.optString("downloadUrl", ""));
                    String releaseNotes = json.optString("release_notes", json.optString("releaseNotes", ""));
                    boolean forceUpdate = json.optBoolean("force_update",
                            json.optBoolean("forceUpdate", json.optBoolean("is_force", false)));
                    boolean hasUpdate = json.optBoolean("has_update", json.optBoolean("hasUpdate", false));

                    if (serverVersionCode > currentVersionCode) {
                        hasUpdate = true;
                    }
                    if (forceUpdate && serverVersionCode >= currentVersionCode && !downloadUrl.isEmpty()) {
                        hasUpdate = true;
                    }

                    if (!hasUpdate) {
                        Log.i(TAG, "已是最新版本: current=" + currentVersionName + "(" + currentVersionCode + ")"
                                + ", server=" + serverVersionName + "(" + serverVersionCode + ")"
                                + ", forceUpdate=" + forceUpdate);
                        if (listener != null) {
                            listener.onNoNewVersion(currentVersionName, currentVersionCode, serverVersionName, serverVersionCode, forceUpdate);
                        }
                        return;
                    }
                    
                    if (downloadUrl.isEmpty()) {
                        if (listener != null) {
                            listener.onCheckFailed("发现更新但下载地址为空: server=" + serverVersionName + "(" + serverVersionCode + ")");
                        }
                        return;
                    }
                    
                    Log.i(TAG, "发现新版本: " + serverVersionName + "(" + serverVersionCode + "), forceUpdate=" + forceUpdate);
                    if (listener != null) {
                        listener.onNewVersionAvailable(serverVersionName, String.valueOf(serverVersionCode), downloadUrl, releaseNotes, forceUpdate);
                    }
                    
                } catch (Exception e) {
                    Log.w(TAG, "检查更新失败，跳过本次检查: " + e.getMessage());
                    if (listener != null) {
                        listener.onCheckFailed("检查更新失败: " + e.getMessage());
                    }
                }
            }
        }).start();
    }
    
    /**
     * 从服务器获取版本信息
     * @param urlStr 检查更新的URL
     * @param currentVersionCode 当前版本号
     */
    private String fetchVersionInfo(String urlStr, int currentVersionCode, String currentVersionName,
                                    String hardware, String signingSha256) {
        HttpURLConnection connection = null;
        try {
            URL url = new URL(urlStr);
            connection = (HttpURLConnection) url.openConnection();
            connection.setConnectTimeout(10000);
            connection.setReadTimeout(10000);
            connection.setRequestMethod("POST");
            connection.setDoOutput(true);
            connection.setRequestProperty("Content-Type", "application/json");
            
            JSONObject request = new JSONObject();
            request.put("current_version_code", currentVersionCode);
            request.put("current_version_name", currentVersionName);
            request.put("hardware", hardware);
            request.put("signing_sha256", signingSha256);
            request.put("package_name", context.getPackageName());
            String requestBody = request.toString();
            try (java.io.OutputStream os = connection.getOutputStream()) {
                os.write(requestBody.getBytes("UTF-8"));
                os.flush();
            }
            
            int responseCode = connection.getResponseCode();
            if (responseCode == HttpURLConnection.HTTP_OK) {
                try (InputStream inputStream = connection.getInputStream();
                     java.util.Scanner scanner = new java.util.Scanner(inputStream).useDelimiter("\\A")) {
                    return scanner.hasNext() ? scanner.next() : "";
                }
            } else {
                Log.w(TAG, "更新检查HTTP响应异常，跳过: " + responseCode);
                return null;
            }
        } catch (Exception e) {
            Log.w(TAG, "更新检查网络不可用或服务器不可达，跳过: " + e.getMessage());
            return null;
        } finally {
            if (connection != null) {
                connection.disconnect();
            }
        }
    }
    
    /**
     * 下载APK文件
     * @param downloadUrl APK下载地址
     * @param fileName 文件名
     */
    public void downloadApk(String downloadUrl, String fileName) {
        if (listener != null) {
            listener.onDownloadStart(0);
        }
        
        try {
            // 创建下载请求
            Uri uri = Uri.parse(downloadUrl);
            DownloadManager.Request request = new DownloadManager.Request(uri);
            File downloadFile = new File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS), fileName);
            lastDownloadFilePath = downloadFile.getAbsolutePath();
            
            // 设置下载参数
            request.setTitle("HSVJEngine 更新");
            request.setDescription("正在下载新版本...");
            request.setNotificationVisibility(DownloadManager.Request.VISIBILITY_VISIBLE_NOTIFY_COMPLETED);
            request.setDestinationInExternalPublicDir(Environment.DIRECTORY_DOWNLOADS, fileName);
            request.setAllowedOverMetered(true);
            request.setAllowedOverRoaming(false);
            request.setMimeType("application/vnd.android.package-archive");
            
            // 开始下载
            downloadId = downloadManager.enqueue(request);
            Log.i(TAG, "开始下载，downloadId: " + downloadId);
            
            // 监听下载进度
            monitorDownloadProgress();
            
        } catch (Exception e) {
            Log.e(TAG, "下载APK失败", e);
            if (listener != null) {
                listener.onDownloadFailed("下载失败: " + e.getMessage());
            }
        }
    }
    
    /**
     * 监听下载进度
     */
    private void monitorDownloadProgress() {
        new Thread(new Runnable() {
            @Override
            public void run() {
                boolean downloading = true;
                
                while (downloading) {
                    DownloadManager.Query query = new DownloadManager.Query();
                    query.setFilterById(downloadId);
                    
                    try (Cursor cursor = downloadManager.query(query)) {
                        if (cursor != null && cursor.moveToFirst()) {
                            int statusIndex = cursor.getColumnIndex(DownloadManager.COLUMN_STATUS);
                            int bytesDownloadedIndex = cursor.getColumnIndex(DownloadManager.COLUMN_BYTES_DOWNLOADED_SO_FAR);
                            int bytesTotalIndex = cursor.getColumnIndex(DownloadManager.COLUMN_TOTAL_SIZE_BYTES);
                            
                            if (statusIndex >= 0 && bytesDownloadedIndex >= 0 && bytesTotalIndex >= 0) {
                                int status = cursor.getInt(statusIndex);
                                long bytesDownloaded = cursor.getLong(bytesDownloadedIndex);
                                long bytesTotal = cursor.getLong(bytesTotalIndex);
                                
                                if (status == DownloadManager.STATUS_SUCCESSFUL) {
                                    downloading = false;
                                    // 获取下载完成的文件URI
                                    Uri fileUri = downloadManager.getUriForDownloadedFile(downloadId);
                                    
                                    if (listener != null) {
                                        listener.onDownloadProgress(bytesTotal, bytesTotal, 100);
                                        listener.onDownloadComplete(fileUri);
                                    }
                                } else if (status == DownloadManager.STATUS_FAILED) {
                                    downloading = false;
                                    if (listener != null) {
                                        listener.onDownloadFailed("下载失败");
                                    }
                                } else if (bytesTotal > 0) {
                                    int progress = (int) ((bytesDownloaded * 100) / bytesTotal);
                                    if (listener != null) {
                                        listener.onDownloadProgress(bytesDownloaded, bytesTotal, progress);
                                    }
                                }
                            }
                        }
                    }
                    
                    try {
                        Thread.sleep(500); // 每500ms检查一次
                    } catch (InterruptedException e) {
                        Thread.currentThread().interrupt();
                        downloading = false;
                    }
                }
            }
        }).start();
    }
    
    /**
     * 安装APK
     * @param fileUri APK文件URI
     */
    public void installApk(Uri fileUri) {
        if (listener != null) {
            listener.onInstallStart();
        }
        
        try {
            String apkPath = resolveApkPath(fileUri);
            if (apkPath != null && installApkSilently(apkPath)) {
                Log.i(TAG, "静默安装APK成功: " + apkPath);
                return;
            }

            Intent intent = new Intent(Intent.ACTION_VIEW);
            
            // Android 7.0+ 需要使用FileProvider
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
                intent.setFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_GRANT_READ_URI_PERMISSION);
                Uri contentUri = FileProvider.getUriForFile(
                        context,
                        context.getPackageName() + ".fileprovider",
                        new File(fileUri.getPath())
                );
                intent.setDataAndType(contentUri, "application/vnd.android.package-archive");
            } else {
                intent.setDataAndType(fileUri, "application/vnd.android.package-archive");
                intent.setFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            }
            
            context.startActivity(intent);
            Log.i(TAG, "启动安装界面");
            
        } catch (Exception e) {
            Log.e(TAG, "安装APK失败", e);
        }
    }

    private String resolveApkPath(Uri fileUri) {
        if (fileUri == null) {
            return null;
        }

        String path = fileUri.getPath();
        if (lastDownloadFilePath != null && new File(lastDownloadFilePath).exists()) {
            return lastDownloadFilePath;
        }

        if (path != null && new File(path).exists()) {
            return path;
        }

        if (downloadId != -1) {
            DownloadManager.Query query = new DownloadManager.Query();
            query.setFilterById(downloadId);
            Cursor cursor = downloadManager.query(query);
            if (cursor != null) {
                try {
                    if (cursor.moveToFirst()) {
                        int localUriIndex = cursor.getColumnIndex(DownloadManager.COLUMN_LOCAL_URI);
                        if (localUriIndex >= 0) {
                            String localUri = cursor.getString(localUriIndex);
                            if (localUri != null) {
                                Uri uri = Uri.parse(localUri);
                                if ("file".equals(uri.getScheme()) && uri.getPath() != null) {
                                    return uri.getPath();
                                }
                            }
                        }
                    }
                } finally {
                    cursor.close();
                }
            }
        }

        return path;
    }

    private boolean installApkSilently(String apkPath) {
        Process process = null;
        try {
            String packageName = context.getPackageName();
            String legacyCleanupScript = buildLegacyPackageCleanupScript(packageName, "$LOG", "  ");
            String cmd = "su 0 sh -c 'cat > /data/local/tmp/hsvj_update_relaunch.sh <<\"EOF\"\n"
                    + "#!/system/bin/sh\n"
                    + "PKG=\"" + packageName + "\"\n"
                    + "TAKEOVER=\"/data/local/tmp/hsvj_drm_takeover.sh\"\n"
                    + "APK=\"" + apkPath + "\"\n"
                    + "LOG=\"/data/local/tmp/hsvj_update_relaunch.log\"\n"
                    + "PACKAGE_UPDATE_GRACE=\"/data/local/tmp/hsvj_package_update_grace\"\n"
                    + "echo \"$(date +%F_%T) supervisor start: $APK\" >> \"$LOG\"\n"
                    + "date +%s > \"$PACKAGE_UPDATE_GRACE\"\n"
                    + "chmod 666 \"$PACKAGE_UPDATE_GRACE\" 2>/dev/null\n"
                    + "(\n"
                    + "  echo \"$(date +%F_%T) install child start\" >> \"$LOG\"\n"
                    + "  date +%s > \"$PACKAGE_UPDATE_GRACE\"\n"
                    + "  pm install -r --dont-kill \"$APK\" >> \"$LOG\" 2>&1\n"
                    + "  CHILD_CODE=$?\n"
                    + "  date +%s > \"$PACKAGE_UPDATE_GRACE\"\n"
                    + "  echo \"$(date +%F_%T) install child exitCode=$CHILD_CODE\" >> \"$LOG\"\n"
                    + "  if [ \"$CHILD_CODE\" -eq 0 ]; then\n"
                    + legacyCleanupScript
                    + "  fi\n"
                    + ") &\n"
                    + "INSTALL_PID=$!\n"
                    + "echo \"$(date +%F_%T) install child pid=$INSTALL_PID\" >> \"$LOG\"\n"
                    + "wait \"$INSTALL_PID\"\n"
                    + "echo \"$(date +%F_%T) install wait finished\" >> \"$LOG\"\n"
                    + "sleep 8\n"
                    + "rm -f /data/local/tmp/hsvj_watchdog.pid\n"
                    + "BACKEND=\"$(getprop persist.hsvj.output.backend 2>/dev/null)\"\n"
                    + "case \"$BACKEND\" in surface|drm-kms) ;; *) BACKEND=drm-kms ;; esac\n"
                    + "if [ ! -x \"$TAKEOVER\" ]; then echo \"$(date +%F_%T) takeover script missing\" >> \"$LOG\"; exit 1; fi\n"
                    + "echo \"$(date +%F_%T) restore backend=$BACKEND\" >> \"$LOG\"\n"
                    + "sh \"$TAKEOVER\" \"$BACKEND\" >> \"$LOG\" 2>&1\n"
                    + "exit $?\n"
                    + "EOF\n"
                    + "chmod 755 /data/local/tmp/hsvj_update_relaunch.sh; "
                    + "setsid sh /data/local/tmp/hsvj_update_relaunch.sh >/data/local/tmp/hsvj_update_relaunch.out 2>&1 < /dev/null &'";
            Log.i(TAG, "尝试静默安装APK: " + cmd);
            process = Runtime.getRuntime().exec(new String[] { "sh", "-c", cmd });
            int exitCode = process.waitFor();
            String output = readProcessOutput(process.getInputStream()) + readProcessOutput(process.getErrorStream());
            Log.i(TAG, "静默安装结果: exitCode=" + exitCode + ", output=" + output);
            return exitCode == 0;
        } catch (Exception e) {
            Log.w(TAG, "静默安装APK失败，回退到系统安装界面: " + e.getMessage(), e);
            return false;
        } finally {
            if (process != null) {
                process.destroy();
            }
        }
    }

    public static String buildLegacyPackageCleanupScript(String currentPackage,
                                                         String logFile,
                                                         String indent) {
        StringBuilder script = new StringBuilder();
        script.append(indent).append("for LEGACY_PKG in");
        for (String legacyPackage : LEGACY_PACKAGE_NAMES) {
            if (!legacyPackage.equals(currentPackage)) {
                script.append(" ").append(legacyPackage);
            }
        }
        script.append("; do\n");
        script.append(indent)
                .append("  if pm list packages \"$LEGACY_PKG\" | grep -q \"^package:$LEGACY_PKG$\"; then\n");
        script.append(indent)
                .append("    echo \"$(date +%F_%T) uninstall legacy package $LEGACY_PKG\" >> \"")
                .append(logFile)
                .append("\"\n");
        script.append(indent)
                .append("    am force-stop \"$LEGACY_PKG\" >/dev/null 2>&1 || true\n");
        script.append(indent)
                .append("    pm uninstall --user 0 \"$LEGACY_PKG\" >> \"")
                .append(logFile)
                .append("\" 2>&1 || pm uninstall \"$LEGACY_PKG\" >> \"")
                .append(logFile)
                .append("\" 2>&1 || true\n");
        script.append(indent).append("  fi\n");
        script.append(indent).append("done\n");
        return script.toString();
    }

    private String readProcessOutput(InputStream inputStream) {
        StringBuilder output = new StringBuilder();
        try (BufferedReader reader = new BufferedReader(new InputStreamReader(inputStream))) {
            String line;
            while ((line = reader.readLine()) != null) {
                output.append(line).append('\n');
            }
        } catch (Exception ignored) {
        }
        return output.toString();
    }
    
    /**
     * 跳过指定版本
     */
    public void skipVersion(String versionName) {
        prefs.edit().putString(KEY_SKIP_VERSION, versionName).apply();
        Log.i(TAG, "已跳过版本: " + versionName);
    }
    
    /**
     * 取消下载
     */
    public void cancelDownload() {
        if (downloadId != -1) {
            downloadManager.remove(downloadId);
            downloadId = -1;
            Log.i(TAG, "下载已取消");
        }
    }
    
    /**
     * 获取当前版本信息
     */
    public String getCurrentVersionInfo() {
        try {
            PackageInfo packageInfo = context.getPackageManager()
                    .getPackageInfo(context.getPackageName(), 0);
            return packageInfo.versionName + " (" + getPackageVersionCode(packageInfo) + ")";
        } catch (Exception e) {
            return "未知";
        }
    }

    @SuppressWarnings("deprecation")
    private int getPackageVersionCode(PackageInfo packageInfo) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            return (int) packageInfo.getLongVersionCode();
        }
        return packageInfo.versionCode;
    }

    private String getCurrentSigningSha256() {
        try {
            Signature[] signatures = Build.VERSION.SDK_INT >= Build.VERSION_CODES.P
                    ? getCurrentSigningCertificateSignatures()
                    : getLegacyCurrentPackageSignatures();
            if (signatures == null || signatures.length == 0) {
                return "";
            }

            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            byte[] hash = digest.digest(signatures[0].toByteArray());
            StringBuilder sb = new StringBuilder(hash.length * 2);
            for (byte b : hash) {
                sb.append(String.format(Locale.US, "%02x", b & 0xff));
            }
            return sb.toString();
        } catch (Exception e) {
            Log.w(TAG, "读取当前APK签名指纹失败: " + e.getMessage());
            return "";
        }
    }

    private Signature[] getCurrentSigningCertificateSignatures() throws PackageManager.NameNotFoundException {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.P) {
            return getLegacyCurrentPackageSignatures();
        }
        PackageInfo packageInfo = context.getPackageManager()
                .getPackageInfo(context.getPackageName(), PackageManager.GET_SIGNING_CERTIFICATES);
        if (packageInfo.signingInfo == null) {
            return new Signature[0];
        }
        return packageInfo.signingInfo.hasMultipleSigners()
                ? packageInfo.signingInfo.getApkContentsSigners()
                : packageInfo.signingInfo.getSigningCertificateHistory();
    }

    @SuppressWarnings("deprecation")
    private Signature[] getLegacyCurrentPackageSignatures() throws PackageManager.NameNotFoundException {
        PackageInfo packageInfo = context.getPackageManager()
                .getPackageInfo(context.getPackageName(), PackageManager.GET_SIGNATURES);
        return packageInfo.signatures;
    }
}
