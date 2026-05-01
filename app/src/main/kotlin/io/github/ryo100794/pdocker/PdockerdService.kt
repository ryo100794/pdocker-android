package io.github.ryo100794.pdocker

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
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
    @Volatile private var userStopped = false

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        if (!Python.isStarted()) {
            Python.start(AndroidPlatform(this))
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            userStopped = true
            stopFlag = true
            pdockerThread?.interrupt()
            stopForeground(STOP_FOREGROUND_REMOVE)
            stopSelf()
            return START_NOT_STICKY
        }

        userStopped = false
        startInForeground()
        if (pdockerThread == null || !pdockerThread!!.isAlive) {
            startPdockerd()
        }
        return START_STICKY
    }

    override fun onTaskRemoved(rootIntent: Intent?) {
        if (!userStopped) {
            scheduleRestart()
        }
        super.onTaskRemoved(rootIntent)
    }

    override fun onDestroy() {
        stopFlag = true
        pdockerThread?.interrupt()
        if (!userStopped) {
            scheduleRestart()
        }
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
        val pendingFlags = PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
        val openIntent = PendingIntent.getActivity(
            this,
            10,
            Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
            pendingFlags,
        )
        val stopIntent = PendingIntent.getService(
            this,
            11,
            Intent(this, PdockerdService::class.java).setAction(ACTION_STOP),
            pendingFlags,
        )
        val notif: Notification = Notification.Builder(this, channelId)
            .setContentTitle("pdockerd")
            .setContentText("Docker daemon (PRoot, port 2375)")
            .setSmallIcon(android.R.drawable.ic_menu_manage)
            .setContentIntent(openIntent)
            .setOngoing(true)
            .addAction(android.R.drawable.ic_menu_close_clear_cancel, "Stop", stopIntent)
            .build()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            startForeground(NOTIF_ID, notif,
                ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC)
        } else {
            startForeground(NOTIF_ID, notif)
        }
    }

    private fun startPdockerd() {
        val runtime = PdockerdRuntime.prepare(this)
        val home = File(filesDir, "pdocker").apply { mkdirs() }
        val sock = File(home, "pdockerd.sock")
        stopFlag = false

        pdockerThread = Thread({
            try {
                val py = Python.getInstance()
                val mod = py.getModule("pdockerd_bridge")
                mod.callAttr(
                    "run_daemon",
                    sock.absolutePath,
                    home.absolutePath,
                    runtime.absolutePath,
                )
            } catch (t: Throwable) {
                Log.e(TAG, "pdockerd crashed", t)
            }
        }, "pdockerd").also { it.start() }
    }

    private fun scheduleRestart() {
        val intent = Intent(applicationContext, PdockerdService::class.java)
            .setAction(ACTION_START)
        val pendingFlags = PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
        val pendingIntent = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            PendingIntent.getForegroundService(applicationContext, 12, intent, pendingFlags)
        } else {
            PendingIntent.getService(applicationContext, 12, intent, pendingFlags)
        }
        val alarm = getSystemService(Context.ALARM_SERVICE) as android.app.AlarmManager
        alarm.set(
            android.app.AlarmManager.RTC_WAKEUP,
            System.currentTimeMillis() + RESTART_DELAY_MS,
            pendingIntent,
        )
    }

    companion object {
        const val ACTION_START = "io.github.ryo100794.pdocker.action.START"
        const val ACTION_STOP = "io.github.ryo100794.pdocker.action.STOP"
        private const val NOTIF_ID = 1
        private const val RESTART_DELAY_MS = 2_000L
        private const val TAG = "pdockerd"
    }
}
