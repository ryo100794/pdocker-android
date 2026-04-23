package io.github.pdocker

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import android.util.Log
import com.chaquo.python.Python
import com.chaquo.python.android.AndroidPlatform
import java.io.File

/**
 * ForegroundService hosting pdockerd (Python via Chaquopy).
 *
 * Lifecycle:
 *  - START_STICKY so Android restarts us after OOM.
 *  - Chaquopy platform initialised once per process.
 *  - pdockerd runs on a background thread; stop flag drives clean shutdown.
 */
class PdockerdService : Service() {

    private var pdockerThread: Thread? = null
    @Volatile private var stopFlag = false

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        if (!Python.isStarted()) {
            Python.start(AndroidPlatform(this))
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        startInForeground()
        if (pdockerThread == null || !pdockerThread!!.isAlive) {
            startPdockerd()
        }
        return START_STICKY
    }

    override fun onDestroy() {
        stopFlag = true
        pdockerThread?.interrupt()
        super.onDestroy()
    }

    private fun startInForeground() {
        val channelId = "pdockerd"
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
            if (nm.getNotificationChannel(channelId) == null) {
                nm.createNotificationChannel(
                    NotificationChannel(channelId, "pdockerd",
                        NotificationManager.IMPORTANCE_LOW)
                )
            }
        }
        val notif: Notification = Notification.Builder(this, channelId)
            .setContentTitle("pdockerd")
            .setContentText("Docker daemon (PRoot, port 2375)")
            .setSmallIcon(android.R.drawable.ic_menu_manage)
            .setOngoing(true)
            .build()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            startForeground(NOTIF_ID, notif,
                ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC)
        } else {
            startForeground(NOTIF_ID, notif)
        }
    }

    private fun startPdockerd() {
        val home = File(filesDir, "pdocker").apply { mkdirs() }
        val sock = File(home, "pdockerd.sock")
        stopFlag = false

        pdockerThread = Thread({
            try {
                val py = Python.getInstance()
                val mod = py.getModule("pdockerd_bridge")
                mod.callAttr("run_daemon", sock.absolutePath, home.absolutePath)
            } catch (t: Throwable) {
                Log.e(TAG, "pdockerd crashed", t)
            }
        }, "pdockerd").also { it.start() }
    }

    companion object {
        const val ACTION_START = "io.github.pdocker.action.START"
        private const val NOTIF_ID = 1
        private const val TAG = "pdockerd"
    }
}
