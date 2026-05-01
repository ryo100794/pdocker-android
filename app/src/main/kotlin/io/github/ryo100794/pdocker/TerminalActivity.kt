package io.github.ryo100794.pdocker

import android.content.Intent
import android.os.Bundle
import android.view.View
import android.webkit.WebView
import android.widget.Button
import android.widget.FrameLayout
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import androidx.appcompat.app.AppCompatActivity
import androidx.activity.addCallback

/**
 * Multi-session WebView host for xterm.js.
 *
 * Each tab owns one WebView and one PTY bridge, so switching tabs keeps shell
 * state alive instead of launching a separate terminal-only screen per action.
 */
class TerminalActivity : AppCompatActivity() {
    private data class Session(
        val title: String,
        val command: String,
        val webView: WebView,
        val bridge: Bridge,
    )

    private val sessions = mutableListOf<Session>()
    private lateinit var tabRow: LinearLayout
    private lateinit var terminalHost: FrameLayout
    private var current = -1

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
        }
        tabRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
        }
        terminalHost = FrameLayout(this)

        root.addView(HorizontalScrollView(this).apply { addView(tabRow) })
        root.addView(terminalHost, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT,
            0,
            1f,
        ))
        setContentView(root)

        val initialTitle = intent.getStringExtra(EXTRA_TITLE) ?: getString(R.string.terminal_shell)
        val initialCommand = intent.getStringExtra(EXTRA_COMMAND) ?: "sh"
        title = getString(R.string.terminal_title)
        addSession(initialTitle, initialCommand)

        onBackPressedDispatcher.addCallback(this) {
            showWorkspace()
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        val label = intent.getStringExtra(EXTRA_TITLE) ?: getString(R.string.terminal_shell_numbered, sessions.size + 1)
        val command = intent.getStringExtra(EXTRA_COMMAND) ?: "sh"
        if (sessions.none { it.title == label && it.command == command }) {
            addSession(label, command)
        } else {
            switchTo(sessions.indexOfFirst { it.title == label && it.command == command })
        }
    }

    override fun onDestroy() {
        sessions.forEach { it.bridge.close() }
        sessions.clear()
        super.onDestroy()
    }

    private fun addSession(label: String, command: String) {
        val webView = WebView(this).apply {
            settings.javaScriptEnabled = true
            settings.domStorageEnabled = true
            visibility = View.GONE
        }
        val bridge = Bridge(this, webView, command)
        webView.addJavascriptInterface(bridge, "PdockerBridge")
        webView.loadUrl("file:///android_asset/xterm/index.html")
        terminalHost.addView(webView, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT,
            FrameLayout.LayoutParams.MATCH_PARENT,
        ))
        sessions += Session(label, command, webView, bridge)
        switchTo(sessions.lastIndex)
        renderTabs()
    }

    private fun switchTo(index: Int) {
        if (index !in sessions.indices) return
        sessions.forEachIndexed { i, session ->
            session.webView.visibility = if (i == index) View.VISIBLE else View.GONE
        }
        current = index
        title = sessions[index].title
        renderTabs()
    }

    private fun renderTabs() {
        if (!::tabRow.isInitialized) return
        tabRow.removeAllViews()
        sessions.forEachIndexed { index, session ->
            tabRow.addView(Button(this).apply {
                text = session.title
                isAllCaps = false
                alpha = if (index == current) 1f else 0.72f
                setOnClickListener { switchTo(index) }
            })
        }
        tabRow.addView(Button(this).apply {
            text = "+"
            isAllCaps = false
            setOnClickListener { addSession(getString(R.string.terminal_shell_numbered, sessions.size + 1), "sh") }
        })
    }

    private fun showWorkspace() {
        startActivity(Intent(this, MainActivity::class.java).apply {
            addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT)
            addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP)
        })
    }

    companion object {
        const val EXTRA_COMMAND = "io.github.ryo100794.pdocker.extra.COMMAND"
        const val EXTRA_TITLE = "io.github.ryo100794.pdocker.extra.TITLE"
    }
}
