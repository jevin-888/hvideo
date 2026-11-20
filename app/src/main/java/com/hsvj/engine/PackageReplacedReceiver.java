package com.hsvj.engine;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.util.Log;

import com.hsvj.engine.update.AppUpdateManager;
import com.hsvj.engine.util.ScriptInstaller;

public class PackageReplacedReceiver extends BroadcastReceiver {
    private static final String TAG = "PackageReplacedReceiver";
    private static final String PACKAGE_UPDATE_GRACE_FILE = "/data/local/tmp/hsvj_package_update_grace";

    @Override
    public void onReceive(Context context, Intent intent) {
        String action = intent != null ? intent.getAction() : null;
        if (!Intent.ACTION_MY_PACKAGE_REPLACED.equals(action)) {
            return;
        }

        final PendingResult pending = goAsync();
        try {
            StartupExecutor.get().execute(() -> {
                try {
                    handlePackageReplaced(context, action);
                } finally {
                    pending.finish();
                }
            });
        } catch (RuntimeException error) {
            pending.finish();
            Log.w(TAG, "包更新后处理任务提交失败", error);
        }
    }

    private void handlePackageReplaced(Context context, String action) {
        String markerCommand = "sh -c 'echo $(date +%F_%T) receiver action=" + action
                + " >> /data/local/tmp/hsvj_package_replaced.log; date +%s > "
                + PACKAGE_UPDATE_GRACE_FILE + "; chmod 644 " + PACKAGE_UPDATE_GRACE_FILE
                + " 2>/dev/null'";
        ScriptInstaller.execRootCommand(markerCommand, "包更新 grace 标记");

        String legacyCleanupScript = AppUpdateManager.buildLegacyPackageCleanupScript(
                context.getPackageName(), "/data/local/tmp/hsvj_package_replaced.log", "");
        String command = "sh -c 'cat > /data/local/tmp/hsvj_package_post_update.sh <<\"EOF\"\n"
                + "#!/system/bin/sh\nLOG=\"/data/local/tmp/hsvj_package_replaced.log\"\n"
                + "sleep 8\n" + legacyCleanupScript + "EOF\n"
                + "chmod 755 /data/local/tmp/hsvj_package_post_update.sh; "
                + "setsid sh /data/local/tmp/hsvj_package_post_update.sh "
                + ">/data/local/tmp/hsvj_package_replaced.out 2>&1 < /dev/null &'";
        ScriptInstaller.execRootCommand(command, "包更新后旧包清理");
        Log.i(TAG, "应用包已更新，按持久属性恢复唯一输出后端");
    }
}
