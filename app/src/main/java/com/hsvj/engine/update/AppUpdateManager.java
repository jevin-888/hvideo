/**
 * @file AppUpdateManager.java（文件名）
 * @brief App版本更新管理器
 * 
 * 负责检查新版本、下载APK、安装更新
 */

package com.hsvj.engine.update;

import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.util.Log;

import androidx.core.content.FileProvider;

import com.hsvj.engine.BuildConfig;
import com.hsvj.engine.PlatformControlGateway;
import com.hsvj.engine.util.ProcessRunner;

import org.json.JSONObject;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.util.Arrays;
import java.io.InputStream;
import java.security.MessageDigest;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;

/**
 * App更新管理器
 * 支持检查版本、下载APK、安装更新
 */
public class AppUpdateManager {
    private static final String TAG = "AppUpdateManager";
    private static final String[] LEGACY_PACKAGE_NAMES = {
            "com.huoshan.vjplayer",
            "com.huoshan.jumukz"
    };
    
    private final Context context;
    private UpdateListener listener;
    private String lastDownloadFilePath;
    
    private volatile AppUpdateProtocol.Release selectedRelease;
    private final ExecutorService executor = Executors.newFixedThreadPool(3, r -> {
        Thread t = new Thread(r, "HSVJ-AppUpdate");
        t.setDaemon(true);
        return t;
    });
    private volatile Future<?> checkFuture;
    private volatile Future<?> monitorFuture;
    
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
    }
    
    /**
     * 设置更新监听器
     */
    public void setUpdateListener(UpdateListener listener) {
        this.listener = listener;
    }
    
    /** Check both publication sources, even when either one is unavailable. */
    public void checkForUpdate(String localUrl, String cloudBase, JSONObject device) {
        final UpdateListener callback = listener;
        if (callback != null) callback.onCheckStart();
        checkFuture = executor.submit(() -> {
            try {
                PackageInfo current = context.getPackageManager().getPackageInfo(context.getPackageName(), 0);
                int currentCode = getPackageVersionCode(current);
                AppUpdateProtocol.Result result = AppUpdateProtocol.checkBoth(localUrl, cloudBase,
                        currentCode, BuildConfig.HARDWARE, context.getPackageName(), installedSigningSha256(),
                        device, this::fetchVersionInfo, executor);
                if (Thread.currentThread().isInterrupted()) return;
                if (!result.errors.isEmpty()) Log.w(TAG, "[更新来源] " + result.errors);
                AppUpdateProtocol.Release release = result.release;
                if (release == null) {
                    if (!result.errors.isEmpty()) {
                        if (callback != null) callback.onCheckFailed(result.errors);
                    } else if (callback != null) {
                        callback.onNoNewVersion(current.versionName, currentCode, "", 0, false);
                    }
                    return;
                }
                selectedRelease = release;
                Log.i(TAG, "[更新来源] 选中=" + release.source + " version=" + release.versionName
                        + "(" + release.versionCode + ")");
                if (callback != null) callback.onNewVersionAvailable(release.versionName,
                        String.valueOf(release.versionCode), release.downloadUrl, release.releaseNotes, release.forceUpdate);
            } catch (InterruptedException error) {
                Thread.currentThread().interrupt();
            } catch (Exception error) {
                Log.w(TAG, "检查更新失败: " + error.getMessage(), error);
                if (!Thread.currentThread().isInterrupted() && callback != null) callback.onCheckFailed(error.getMessage());
            }
        });
    }

    private String fetchVersionInfo(String url, JSONObject request) throws Exception {
        Log.i(TAG, "[更新检查] 请求 " + url);
        String raw = PlatformControlGateway.requestHttpAcrossNetworks("POST", url, request.toString(), "", 5000);
        JSONObject reply = new JSONObject(raw);
        int status = reply.getInt("status");
        if (status != 200) throw new IOException("更新检查 HTTP " + status);
        String body = reply.getString("body");
        if (body.length() > 65536) throw new IOException("更新响应过大");
        return body;
    }

    @SuppressWarnings("deprecation")
    private String installedSigningSha256() throws Exception {
        int flags = Build.VERSION.SDK_INT >= 28 ? PackageManager.GET_SIGNING_CERTIFICATES : PackageManager.GET_SIGNATURES;
        PackageInfo current = context.getPackageManager().getPackageInfo(context.getPackageName(), flags);
        Signature[] signatures = Build.VERSION.SDK_INT >= 28
                ? (current.signingInfo == null ? null : current.signingInfo.getApkContentsSigners()) : current.signatures;
        if (signatures == null || signatures.length != 1) throw new IOException("无法确定当前播放器签名");
        byte[] digest = MessageDigest.getInstance("SHA-256").digest(signatures[0].toByteArray());
        StringBuilder value = new StringBuilder();
        for (byte b : digest) value.append(String.format(Locale.ROOT, "%02x", b & 255));
        return value.toString();
    }

    /** Network-bound streaming supports Wi-Fi, Ethernet and failover during a download. */
    public void downloadApk(String downloadUrl) {
        final UpdateListener callback = listener;
        final AppUpdateProtocol.Release release = selectedRelease;
        if (release == null || !downloadUrl.equals(release.downloadUrl)) {
            if (callback != null) callback.onDownloadFailed("缺少已验证的升级信息");
            return;
        }
        if (callback != null) callback.onDownloadStart(release.size);
        monitorFuture = executor.submit(() -> {
            File file = null;
            boolean verified = false;
            try {
                File directory = context.getExternalFilesDir(Environment.DIRECTORY_DOWNLOADS);
                if (directory == null || (!directory.isDirectory() && !directory.mkdirs())) {
                    throw new IOException("下载存储不可用");
                }
                file = new File(directory, "hsvj-update-" + release.sha256 + ".apk");
                if (file.exists() && !file.delete()) throw new IOException("无法清理旧下载");
                lastDownloadFilePath = file.getAbsolutePath();
                PlatformControlGateway.downloadAppUpdateAcrossNetworks(downloadUrl, file, release.size, release.sha256,
                        (bytes, total, percent) -> {
                            if (callback != null) callback.onDownloadProgress(bytes, total, percent);
                        });
                verifyDownloadedApk(file, release);
                if (Thread.currentThread().isInterrupted()) throw new InterruptedException();
                verified = true;
                if (callback != null) callback.onDownloadComplete(Uri.fromFile(file));
            } catch (InterruptedException error) {
                Thread.currentThread().interrupt();
            } catch (Exception error) {
                Log.e(TAG, "下载/校验 APK 失败", error);
                if (!Thread.currentThread().isInterrupted() && callback != null) callback.onDownloadFailed(error.getMessage());
            } finally {
                if (!verified && file != null && file.exists() && !file.delete()) {
                    Log.w(TAG, "无法清理失败下载: " + file);
                }
            }
        });
    }

    /** 取消更新检查/下载并释放线程池。 */
    public void destroy() {
        Future<?> check = checkFuture;
        if (check != null) check.cancel(true);
        Future<?> monitor = monitorFuture;
        if (monitor != null) monitor.cancel(true);
        executor.shutdownNow();
        listener = null;
    }
    
    /**
     * 安装APK
     * @param fileUri APK文件URI
     */
    public void installApk(Uri fileUri) {
        if (fileUri == null) {
            Log.e(TAG, "Cannot install APK: file URI is null");
            return;
        }

        if (listener != null) {
            listener.onInstallStart();
        }

        try {
            String apkPath = resolveApkPath(fileUri);
            if (apkPath != null && installApkSilently(apkPath)) {
                Log.i(TAG, "Silent APK installation submitted: " + apkPath);
                return;
            }

            Uri installUri;
            if ("content".equalsIgnoreCase(fileUri.getScheme())) {
                installUri = fileUri;
            } else if (apkPath != null) {
                installUri = FileProvider.getUriForFile(
                        context,
                        context.getPackageName() + ".fileprovider",
                        new File(apkPath)
                );
            } else {
                throw new IllegalArgumentException("Cannot resolve APK file: " + fileUri);
            }

            Intent intent = new Intent(Intent.ACTION_VIEW)
                    .setDataAndType(installUri, "application/vnd.android.package-archive")
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_GRANT_READ_URI_PERMISSION);
            context.startActivity(intent);
            Log.i(TAG, "Opened APK installer");

        } catch (Exception e) {
            Log.e(TAG, "Failed to install APK", e);
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

        return null;
    }

    private boolean installApkSilently(String apkPath) {
        try {
            String packageName = context.getPackageName();
            String legacyCleanupScript = buildLegacyPackageCleanupScript(packageName, "$LOG", "  ");
            String cmd = "sh -c 'cat > /data/local/tmp/hsvj_update_relaunch.sh <<\"EOF\"\n"
                    + "#!/system/bin/sh\n"
                    + "PKG=\"" + packageName + "\"\n"
                    + "TAKEOVER=\"/vendor/bin/hsvj_drm_takeover.sh\"\n"
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
                    + "BACKEND=\"$(getprop persist.hsvj.output.backend 2>/dev/null)\"\n"
                    + "case \"$BACKEND\" in surface|drm-kms) ;; *) BACKEND=surface ;; esac\n"
                    + "if [ ! -x \"$TAKEOVER\" ]; then echo \"$(date +%F_%T) takeover script missing\" >> \"$LOG\"; exit 1; fi\n"
                    + "echo \"$(date +%F_%T) restore backend=$BACKEND\" >> \"$LOG\"\n"
                    + "sh \"$TAKEOVER\" \"$BACKEND\" >> \"$LOG\" 2>&1\n"
                    + "exit $?\n"
                    + "EOF\n"
                    + "chmod 755 /data/local/tmp/hsvj_update_relaunch.sh; "
                    + "setsid sh /data/local/tmp/hsvj_update_relaunch.sh >/data/local/tmp/hsvj_update_relaunch.out 2>&1 < /dev/null &'";
            Log.i(TAG, "尝试静默安装APK: " + cmd);
            ProcessRunner.Result result = ProcessRunner.run(30_000L, "su", "0", "sh", "-c", cmd);
            if (result.timedOut) {
                Log.w(TAG, "静默安装APK超时，回退到系统安装界面");
                return false;
            }
            Log.i(TAG, "静默安装提交结果: exitCode=" + result.exitCode);
            return result.succeeded();
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return false;
        } catch (Exception e) {
            Log.w(TAG, "静默安装APK失败，回退到系统安装界面: " + e.getMessage(), e);
            return false;
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

    @SuppressWarnings("deprecation")
    private int getPackageVersionCode(PackageInfo packageInfo) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            return (int) packageInfo.getLongVersionCode();
        }
        return packageInfo.versionCode;
    }

    /** Runs on the download worker before any installer or root command is invoked. */
    @SuppressWarnings("deprecation")
    private void verifyDownloadedApk(File file, AppUpdateProtocol.Release release) throws Exception {
        if (file.length() != release.size) throw new IOException("APK 文件大小校验失败");
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        try (InputStream input = new FileInputStream(file)) {
            byte[] buffer = new byte[65536];
            int n;
            while ((n = input.read(buffer)) != -1) digest.update(buffer, 0, n);
        }
        StringBuilder hash = new StringBuilder();
        for (byte b : digest.digest()) hash.append(String.format(Locale.US, "%02x", b & 0xff));
        if (!hash.toString().equals(release.sha256)) throw new IOException("APK SHA-256 校验失败");
        PackageManager pm = context.getPackageManager();
        int flags = PackageManager.GET_META_DATA | (Build.VERSION.SDK_INT >= 28
                ? PackageManager.GET_SIGNING_CERTIFICATES : PackageManager.GET_SIGNATURES);
        PackageInfo archive = pm.getPackageArchiveInfo(file.getAbsolutePath(), flags);
        PackageInfo current = pm.getPackageInfo(context.getPackageName(), flags);
        if (archive == null || !context.getPackageName().equals(archive.packageName)
                || getPackageVersionCode(archive) != release.versionCode
                || getPackageVersionCode(archive) <= getPackageVersionCode(current)
                || archive.applicationInfo == null || archive.applicationInfo.metaData == null
                || !BuildConfig.HARDWARE.equals(archive.applicationInfo.metaData.getString("com.hsvj.engine.HARDWARE"))) {
            throw new IOException("APK 包名、版本或硬件型号校验失败");
        }
        Signature[] candidate = Build.VERSION.SDK_INT >= 28
                ? (archive.signingInfo == null ? null : archive.signingInfo.getApkContentsSigners()) : archive.signatures;
        Signature[] installed = Build.VERSION.SDK_INT >= 28
                ? (current.signingInfo == null ? null : current.signingInfo.getApkContentsSigners()) : current.signatures;
        if (candidate == null || installed == null || candidate.length == 0 || candidate.length != installed.length
                || !Arrays.asList(candidate).containsAll(Arrays.asList(installed))) {
            throw new IOException("APK 签名与当前播放器不一致");
        }
    }
}
