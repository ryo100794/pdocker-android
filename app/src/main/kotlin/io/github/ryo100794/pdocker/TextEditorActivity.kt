package io.github.ryo100794.pdocker

import android.os.Bundle
import androidx.appcompat.app.AppCompatActivity
import java.io.File

class TextEditorActivity : AppCompatActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val requested = intent.getStringExtra(EXTRA_PATH).orEmpty()
        setContentView(CodeEditorView(this, resolveProjectFile(requested), MAX_EDIT_BYTES, ::defaultContent))
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
