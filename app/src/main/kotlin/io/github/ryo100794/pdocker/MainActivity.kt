package io.github.ryo100794.pdocker

import android.content.Intent
import android.net.LocalSocket
import android.net.LocalSocketAddress
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import java.io.File
import kotlin.concurrent.thread

class MainActivity : AppCompatActivity() {
    private val ui = Handler(Looper.getMainLooper())
    private val pollTask = object : Runnable {
        override fun run() {
            refreshStatus()
            ui.postDelayed(this, 2000)
        }
    }
    private lateinit var status: TextView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(48, 48, 48, 48)
        }

        status = TextView(this).apply {
            text = "pdockerd: unknown"
            textSize = 16f
        }

        val startBtn = Button(this).apply {
            text = "Start pdockerd"
            setOnClickListener {
                val intent = Intent(this@MainActivity, PdockerdService::class.java)
                    .setAction(PdockerdService.ACTION_START)
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                    startForegroundService(intent)
                } else {
                    startService(intent)
                }
                status.text = "pdockerd: starting..."
            }
        }

        val termBtn = Button(this).apply {
            text = "Open terminal"
            setOnClickListener {
                startActivity(Intent(this@MainActivity, TerminalActivity::class.java))
            }
        }

        val stopBtn = Button(this).apply {
            text = "Stop pdockerd"
            setOnClickListener {
                stopService(Intent(this@MainActivity, PdockerdService::class.java))
                status.text = "pdockerd: stopped"
            }
        }

        root.addView(status)
        root.addView(startBtn)
        root.addView(termBtn)
        root.addView(stopBtn)
        setContentView(root)
    }

    override fun onResume() {
        super.onResume()
        ui.post(pollTask)
    }

    override fun onPause() {
        super.onPause()
        ui.removeCallbacks(pollTask)
    }

    // Off-thread health check: speak Docker Engine API /_ping over the
    // AF_UNIX socket and look for "OK" back. Uses Android's LocalSocket
    // with FILESYSTEM namespace so the path resolves to our filesDir.
    private fun refreshStatus() {
        val sock = File(filesDir, "pdocker/pdockerd.sock")
        if (!sock.exists()) {
            status.text = "pdockerd: socket absent"
            return
        }
        thread(isDaemon = true, name = "pdockerd-ping") {
            val msg = runCatching {
                LocalSocket().use { ls ->
                    ls.connect(LocalSocketAddress(sock.absolutePath,
                        LocalSocketAddress.Namespace.FILESYSTEM))
                    ls.soTimeout = 500
                    ls.outputStream.write(
                        "GET /_ping HTTP/1.0\r\nHost: pdocker\r\n\r\n".toByteArray()
                    )
                    ls.outputStream.flush()
                    val resp = ls.inputStream.readBytes().toString(Charsets.US_ASCII)
                    if ("200 OK" in resp && resp.trimEnd().endsWith("OK")) "running"
                    else "socket up, unexpected response"
                }
            }.getOrElse { "socket up, ping failed: ${it.message}" }
            ui.post { status.text = "pdockerd: $msg" }
        }
    }
}
