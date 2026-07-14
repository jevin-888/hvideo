package com.hsvj.engine;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.util.Log;

import com.hsvj.engine.update.AppUpdateManager;

public class PackageReplacedReceiver extends BroadcastReceiver {
    private static final String TAG = "PackageReplacedReceiver";
    private static final String PACKAGE_UPDATE_GRACE_FILE = "/data/local/tmp/hsvj_package_update_grace";

    @Override
    public void onReceive(Context context, Intent intent) {
        String action = intent != null ? intent.getAction() : null;
        if (!Intent.ACTION_MY_PACKAGE_REPLACED.equals(action)
                && !Intent.ACTION_PACKAGE_REPLACED.equals(action)
                && !Intent.ACTION_PACKAGE_ADDED.equals(action)) {
            return;
        }
        if (Intent.ACTION_PACKAGE_REPLACED.equals(action)
                || Intent.ACTION_PACKAGE_ADDED.equals(action)) {
            Uri data = intent.getData();
            String packageName = data != null ? data.getSchemeSpecificPart() : null;
            if (!context.getPackageName().equals(packageName)) {
                return;
            }
            if (Intent.ACTION_PACKAGE_ADDED.equals(action)
                    && !intent.getBooleanExtra(Intent.EXTRA_REPLACING, false)) {
                return;
            }
        }

        try {
            Runtime.getRuntime().exec(new String[] { "sh", "-c",
                    "su 0 sh -c 'echo $(date +%F_%T) receiver action=" + action
                            + " >> /data/local/tmp/hsvj_package_replaced.log; date +%s > "
                            + PACKAGE_UPDATE_GRACE_FILE
                            + "; chmod 666 "
                            + PACKAGE_UPDATE_GRACE_FILE
                            + " 2>/dev/null'" });
        } catch (Exception ignored) {
        }

        Log.i(TAG, "应用包已更新，按持久属性恢复唯一输出后端");

        try {
            String legacyCleanupScript = AppUpdateManager.buildLegacyPackageCleanupScript(
                    context.getPackageName(),
                    "/data/local/tmp/hsvj_package_replaced.log",
                    "");
            Runtime.getRuntime().exec(new String[] { "sh", "-c",
                    "su 0 sh -c 'cat > /data/local/tmp/hsvj_package_post_update.sh <<\"EOF\"\n"
                            + "#!/system/bin/sh\n"
                            + "LOG=\"/data/local/tmp/hsvj_package_replaced.log\"\n"
                            + "sleep 8\n"
                            + legacyCleanupScript
                            + "EOF\n"
                            + "chmod 755 /data/local/tmp/hsvj_package_post_update.sh; "
                            + "setsid sh /data/local/tmp/hsvj_package_post_update.sh "
                            + ">/data/local/tmp/hsvj_package_replaced.out 2>&1 < /dev/null &'" });
        } catch (Exception error) {
            Log.w(TAG, "旧包清理命令提交失败: " + error.getMessage(), error);
        }

        OutputBackendController.installAndStart(context, "package-replaced");
    }
}
