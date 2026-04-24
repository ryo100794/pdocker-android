package io.github.ryo100794.pdocker

import android.os.Bundle
import android.webkit.WebView
import androidx.appcompat.app.AppCompatActivity

/**
 * WebView host for xterm.js. Picked over Termux TerminalView because
 * Termux's custom view drops CJK IME composition events.
 */
class TerminalActivity : AppCompatActivity() {
    private lateinit var webView: WebView
    private lateinit var bridge: Bridge

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        webView = WebView(this).apply {
            settings.javaScriptEnabled = true
            settings.domStorageEnabled = true
        }
        bridge = Bridge(this, webView)
        webView.addJavascriptInterface(bridge, "PdockerBridge")
        webView.loadUrl("file:///android_asset/xterm/index.html")
        setContentView(webView)
    }

    override fun onDestroy() {
        bridge.close()
        super.onDestroy()
    }
}
