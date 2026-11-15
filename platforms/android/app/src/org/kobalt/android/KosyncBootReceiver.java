package org.kobalt.android;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import androidx.work.WorkManager;

/** Initialize WorkManager after boot even if its optional receiver was disabled. */
public final class KosyncBootReceiver extends BroadcastReceiver {
    @Override public void onReceive(Context context, Intent intent) {
        if (!Intent.ACTION_BOOT_COMPLETED.equals(intent.getAction())) return;
        PendingResult pending = goAsync();
        try {
            // Initialization reconciles persisted work with JobScheduler. Keep
            // the broadcast alive until its serial executor has read the work.
            WorkManager.getInstance(context).getWorkInfosByTag("kobalt-kosync-pause")
                    .addListener(pending::finish, Runnable::run);
        } catch (RuntimeException unavailable) {
            pending.finish();
        }
    }
}
