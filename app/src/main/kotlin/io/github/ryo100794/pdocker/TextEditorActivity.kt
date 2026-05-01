package io.github.ryo100794.pdocker

import android.os.Bundle
import android.text.InputType
import android.text.TextUtils
import android.view.Gravity
import android.widget.Button
import android.widget.EditText
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import java.io.File

class TextEditorActivity : AppCompatActivity() {
    private lateinit var pathView: TextView
    private lateinit var editor: EditText
    private lateinit var message: TextView
    private lateinit var target: File

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(24, 24, 24, 24)
        }
        val toolbar = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        toolbar.addView(Button(this).apply {
            text = getString(R.string.button_save)
            isAllCaps = false
            setOnClickListener { save() }
        })
        toolbar.addView(Button(this).apply {
            text = getString(R.string.button_reload)
            isAllCaps = false
            setOnClickListener { load() }
        })
        toolbar.addView(Button(this).apply {
            text = getString(R.string.button_close)
            isAllCaps = false
            setOnClickListener { finish() }
        })

        pathView = TextView(this).apply {
            textSize = 13f
            setSingleLine(true)
            ellipsize = TextUtils.TruncateAt.MIDDLE
            setPadding(0, 10, 0, 10)
        }
        message = TextView(this).apply {
            textSize = 12f
            alpha = 0.72f
            setPadding(0, 0, 0, 8)
        }
        editor = EditText(this).apply {
            setTextIsSelectable(true)
            setHorizontallyScrolling(true)
            gravity = Gravity.START or Gravity.TOP
            typeface = android.graphics.Typeface.MONOSPACE
            textSize = 14f
            inputType = InputType.TYPE_CLASS_TEXT or
                InputType.TYPE_TEXT_FLAG_MULTI_LINE or
                InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
            minLines = 20
        }

        root.addView(toolbar)
        root.addView(HorizontalScrollView(this).apply { addView(pathView) })
        root.addView(message)
        root.addView(editor, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT,
            0,
            1f,
        ))
        setContentView(root)

        val requested = intent.getStringExtra(EXTRA_PATH).orEmpty()
        target = resolveProjectFile(requested)
        pathView.text = target.absolutePath
        load()
    }

    private fun load() {
        if (!target.exists()) {
            target.parentFile?.mkdirs()
            target.writeText(defaultContent(target.name))
        }
        val size = target.length()
        if (size > MAX_EDIT_BYTES) {
            editor.setText("")
            message.text = getString(R.string.editor_file_too_large_fmt, size)
            return
        }
        editor.setText(target.readText())
        editor.setSelection(editor.text.length)
        message.text = getString(R.string.editor_loaded_fmt, target.length())
    }

    private fun save() {
        target.parentFile?.mkdirs()
        target.writeText(editor.text.toString())
        message.text = getString(R.string.editor_saved_fmt, target.length())
    }

    private fun resolveProjectFile(requested: String): File {
        val projects = File(filesDir, "pdocker/projects").apply { mkdirs() }.canonicalFile
        val candidate = if (requested.isBlank()) {
            File(projects, "default/Dockerfile")
        } else {
            File(requested)
        }
        val canonical = candidate.canonicalFile
        require(canonical.toPath().startsWith(projects.toPath())) {
            getString(R.string.editor_path_outside_fmt, projects.absolutePath)
        }
        return canonical
    }

    private fun defaultContent(name: String): String =
        when (name) {
            "compose.yaml", "compose.yml", "docker-compose.yaml", "docker-compose.yml" ->
                "services:\n  app:\n    image: ubuntu:22.04\n    command: [\"/bin/bash\", \"-lc\", \"echo hello from compose\"]\n"
            "Dockerfile" ->
                "FROM ubuntu:22.04\nCMD [\"/bin/bash\", \"-lc\", \"echo hello from Dockerfile\"]\n"
            else -> ""
        }

    companion object {
        const val EXTRA_PATH = "io.github.ryo100794.pdocker.extra.EDITOR_PATH"
        private const val MAX_EDIT_BYTES = 512 * 1024
    }
}
