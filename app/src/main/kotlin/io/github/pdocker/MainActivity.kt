package io.github.pdocker

import android.content.Intent
import android.os.Build
import android.os.Bundle
import android.view.View
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat

class MainActivity : AppCompatActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(48, 48, 48, 48)
        }

        val status = TextView(this).apply {
            text = "pdockerd: not running"
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
}
