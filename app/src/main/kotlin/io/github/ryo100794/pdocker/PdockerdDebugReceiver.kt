package io.github.ryo100794.pdocker

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.pm.ApplicationInfo
import android.os.Build
import java.io.File
import kotlin.concurrent.thread

class PdockerdDebugReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        val debuggable = (context.applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE) != 0
        if (!debuggable) return
        when (intent.action) {
            ACTION_SMOKE_START -> {
                val service = Intent(context, PdockerdService::class.java)
                    .setAction(PdockerdService.ACTION_START)
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                    context.startForegroundService(service)
                } else {
                    context.startService(service)
                }
            }
            ACTION_SMOKE_GPU_BENCH -> {
                val pending = goAsync()
                val benchDir = File(context.filesDir, "pdocker/bench").apply { mkdirs() }
                File(benchDir, "android-gpu-bench-receiver.txt").writeText("received\n")
                thread(isDaemon = true, name = "android-gpu-bench-broadcast") {
                    runCatching { AndroidGpuBench.run(context.applicationContext) }
                        .onSuccess { File(benchDir, "android-gpu-bench-receiver.txt").writeText("complete\n") }
                        .onFailure { File(benchDir, "android-gpu-bench-error.txt").writeText(it.stackTraceToString()) }
                    pending.finish()
                }
            }
        }
    }

    companion object {
        const val ACTION_SMOKE_START = "io.github.ryo100794.pdocker.action.SMOKE_START"
        const val ACTION_SMOKE_GPU_BENCH = "io.github.ryo100794.pdocker.action.SMOKE_GPU_BENCH"
    }
}
