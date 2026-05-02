package io.github.ryo100794.pdocker

import android.content.Context
import android.graphics.Color
import android.graphics.Typeface
import android.text.Editable
import android.text.InputType
import android.text.Spannable
import android.text.TextUtils
import android.text.TextWatcher
import android.text.method.ReplacementTransformationMethod
import android.text.style.ForegroundColorSpan
import android.view.Gravity
import android.view.MotionEvent
import android.view.ScaleGestureDetector
import android.widget.Button
import android.widget.EditText
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import android.widget.TextView
import java.io.File

class CodeEditorView(
    context: Context,
    private val file: File,
    private val maxBytes: Int,
    private val defaultContent: (String) -> String,
) : LinearLayout(context) {
    private val message: TextView
    private val lineNumbers: TextView
    private val editor: EditText
    private val searchField: EditText
    private val replaceField: EditText
    private var spacesMode = true
    private var tabWidth = 4
    private var highlighting = false
    private var editorFontSize = 14f
    private val scaleDetector = ScaleGestureDetector(context, object : ScaleGestureDetector.SimpleOnScaleGestureListener() {
        override fun onScale(detector: ScaleGestureDetector): Boolean {
            editorFontSize = (editorFontSize * detector.scaleFactor).coerceIn(10f, 24f)
            applyEditorFontSize()
            return true
        }
    })

    init {
        orientation = VERTICAL
        setPadding(0, 8, 0, 0)

        val pathView = TextView(context).apply {
            text = file.absolutePath
            textSize = 12f
            alpha = 0.72f
            setSingleLine(true)
            ellipsize = TextUtils.TruncateAt.MIDDLE
            setPadding(10, 0, 0, 0)
        }
        message = TextView(context).apply {
            textSize = 12f
            alpha = 0.72f
            setSingleLine(true)
            ellipsize = TextUtils.TruncateAt.MIDDLE
        }
        searchField = smallField(context.getString(R.string.editor_find_hint))
        replaceField = smallField(context.getString(R.string.editor_replace_hint))
        lineNumbers = TextView(context).apply {
            typeface = Typeface.MONOSPACE
            gravity = Gravity.END or Gravity.TOP
            alpha = 0.58f
            setPadding(0, 8, 10, 8)
        }
        editor = EditText(context).apply {
            setTextIsSelectable(true)
            setHorizontallyScrolling(true)
            gravity = Gravity.START or Gravity.TOP
            typeface = Typeface.MONOSPACE
            inputType = InputType.TYPE_CLASS_TEXT or
                InputType.TYPE_TEXT_FLAG_MULTI_LINE or
                InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
            minLines = 12
            transformationMethod = VisibleWhitespaceTransformation()
            setOnTouchListener { view, event ->
                val scaling = event.pointerCount > 1
                if (scaling) {
                    view.parent?.requestDisallowInterceptTouchEvent(true)
                    scaleDetector.onTouchEvent(event)
                    if (event.actionMasked == MotionEvent.ACTION_POINTER_UP ||
                        event.actionMasked == MotionEvent.ACTION_UP ||
                        event.actionMasked == MotionEvent.ACTION_CANCEL) {
                        view.parent?.requestDisallowInterceptTouchEvent(false)
                    }
                    true
                } else {
                    scaleDetector.onTouchEvent(event)
                    false
                }
            }
            addTextChangedListener(object : TextWatcher {
                override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) = Unit
                override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) = Unit
                override fun afterTextChanged(s: Editable) {
                    updateLineNumbers()
                    applyHighlighting(s)
                }
            })
        }

        addView(toolPalette(pathView))
        addView(searchPalette())
        addView(message)
        applyEditorFontSize()
        addView(LinearLayout(context).apply {
            orientation = HORIZONTAL
            addView(lineNumbers, LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.MATCH_PARENT))
            addView(HorizontalScrollView(context).apply { addView(editor) }, LayoutParams(
                0,
                LayoutParams.MATCH_PARENT,
                1f,
            ))
        }, LayoutParams(LayoutParams.MATCH_PARENT, 0, 1f))
        load()
    }

    private fun applyEditorFontSize() {
        editor.textSize = editorFontSize
        lineNumbers.textSize = editorFontSize
    }

    private fun toolPalette(pathView: TextView): LinearLayout =
        LinearLayout(context).apply {
            orientation = HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            addToolButton(context.getString(R.string.button_save)) { save() }
            addToolButton(context.getString(R.string.button_reload)) { load() }
            addToolButton(modeLabel()) { toggleIndentMode() }
            addToolButton(widthLabel()) { cycleTabWidth() }
            addToolButton(context.getString(R.string.editor_indent)) { indentSelection() }
            addToolButton(context.getString(R.string.editor_outdent)) { outdentSelection() }
            addView(pathView, LayoutParams(0, LayoutParams.WRAP_CONTENT, 1f))
        }

    private fun searchPalette(): LinearLayout =
        LinearLayout(context).apply {
            orientation = HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            addView(searchField, LayoutParams(0, LayoutParams.WRAP_CONTENT, 1f))
            addToolButton(context.getString(R.string.editor_find_next)) { findNext() }
            addView(replaceField, LayoutParams(0, LayoutParams.WRAP_CONTENT, 1f))
            addToolButton(context.getString(R.string.editor_replace_one)) { replaceCurrent() }
            addToolButton(context.getString(R.string.editor_replace_all)) { replaceAllMatches() }
        }

    private fun smallField(hintText: String): EditText =
        EditText(context).apply {
            hint = hintText
            setSingleLine(true)
            textSize = 12f
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
            minWidth = 0
            setPadding(10, 0, 10, 0)
        }

    private fun LinearLayout.addToolButton(label: String, action: () -> Unit) {
        addView(Button(context).apply {
            text = label
            isAllCaps = false
            minWidth = 0
            setPadding(12, 0, 12, 0)
            setOnClickListener { action() }
        })
    }

    fun load() {
        file.parentFile?.mkdirs()
        if (!file.exists()) file.writeText(defaultContent(file.name))
        val size = file.length()
        if (size > maxBytes) {
            editor.setText("")
            message.text = context.getString(R.string.editor_file_too_large_fmt, size)
            return
        }
        editor.setText(file.readText())
        editor.setSelection(editor.text.length)
        message.text = context.getString(R.string.editor_loaded_fmt, file.length())
        updateLineNumbers()
    }

    fun save() {
        file.parentFile?.mkdirs()
        file.writeText(editor.text.toString())
        message.text = context.getString(R.string.editor_saved_fmt, file.length())
    }

    private fun toggleIndentMode() {
        spacesMode = !spacesMode
        replaceAllText(convertIndentation(editor.text.toString(), spacesMode))
        refreshToolbar()
    }

    private fun cycleTabWidth() {
        tabWidth = when (tabWidth) {
            2 -> 4
            4 -> 8
            else -> 2
        }
        if (spacesMode) replaceAllText(convertIndentation(editor.text.toString(), true))
        refreshToolbar()
    }

    private fun indentSelection() {
        transformSelectedLines { line ->
            if (spacesMode) " ".repeat(tabWidth) + line else "\t$line"
        }
    }

    private fun outdentSelection() {
        transformSelectedLines { line ->
            val normalized = normalizeLeadingSpaces(line)
            when {
                normalized.startsWith("\t") -> normalized.drop(1)
                normalized.startsWith(" ".repeat(tabWidth)) -> normalized.drop(tabWidth)
                normalized.startsWith(" ") -> normalized.drop(normalized.takeWhile { it == ' ' }.length)
                else -> normalized
            }
        }
    }

    private fun transformSelectedLines(transform: (String) -> String) {
        val text = editor.text.toString()
        val start = lineStart(text, minOf(editor.selectionStart, editor.selectionEnd).coerceAtLeast(0))
        val end = lineEnd(text, maxOf(editor.selectionStart, editor.selectionEnd).coerceAtLeast(0))
        val block = text.substring(start, end)
        val trailingNewline = block.endsWith("\n")
        val lines = block.removeSuffix("\n").split("\n")
        val replacement = lines.joinToString("\n") { transform(it) } + if (trailingNewline) "\n" else ""
        editor.text.replace(start, end, replacement)
        editor.setSelection(start, (start + replacement.length).coerceAtMost(editor.text.length))
    }

    private fun convertIndentation(text: String, toSpaces: Boolean): String =
        text.lines().joinToString("\n") { line ->
            val indent = line.takeWhile { it == ' ' || it == '\t' }
            val rest = line.drop(indent.length)
            val columns = indent.fold(0) { acc, ch -> acc + if (ch == '\t') tabWidth else 1 }
            val adjustedColumns = ((columns + tabWidth - 1) / tabWidth) * tabWidth
            val converted = if (toSpaces) {
                " ".repeat(adjustedColumns)
            } else {
                "\t".repeat(adjustedColumns / tabWidth)
            }
            converted + rest
        }

    private fun normalizeLeadingSpaces(line: String): String {
        val indent = line.takeWhile { it == ' ' }
        if (indent.isEmpty() || indent.length % tabWidth == 0) return line
        val adjusted = (indent.length / tabWidth) * tabWidth
        return " ".repeat(adjusted) + line.drop(indent.length)
    }

    private fun replaceAllText(text: String) {
        val cursor = editor.selectionStart.coerceAtLeast(0)
        editor.setText(text)
        editor.setSelection(cursor.coerceAtMost(editor.text.length))
    }

    private fun refreshToolbar() {
        val parent = getChildAt(0) as? LinearLayout ?: return
        (parent.getChildAt(2) as? Button)?.text = modeLabel()
        (parent.getChildAt(3) as? Button)?.text = widthLabel()
    }

    private fun findNext() {
        val query = searchField.text.toString()
        if (query.isEmpty()) {
            message.text = context.getString(R.string.editor_find_empty)
            return
        }
        val src = editor.text.toString()
        val start = maxOf(editor.selectionEnd, 0).coerceAtMost(src.length)
        val first = src.indexOf(query, start, ignoreCase = true)
        val index = if (first >= 0) first else src.indexOf(query, 0, ignoreCase = true)
        if (index < 0) {
            message.text = context.getString(R.string.editor_find_no_match_fmt, query)
            return
        }
        editor.requestFocus()
        editor.setSelection(index, index + query.length)
        message.text = context.getString(R.string.editor_find_match_fmt, index + 1)
    }

    private fun replaceCurrent() {
        val query = searchField.text.toString()
        if (query.isEmpty()) {
            message.text = context.getString(R.string.editor_find_empty)
            return
        }
        val start = minOf(editor.selectionStart, editor.selectionEnd).coerceAtLeast(0)
        val end = maxOf(editor.selectionStart, editor.selectionEnd).coerceAtLeast(0)
        val selected = editor.text.substring(start, end)
        if (!selected.equals(query, ignoreCase = true)) {
            findNext()
            return
        }
        val replacement = replaceField.text.toString()
        editor.text.replace(start, end, replacement)
        editor.setSelection(start, (start + replacement.length).coerceAtMost(editor.text.length))
        message.text = context.getString(R.string.editor_replaced_one)
    }

    private fun replaceAllMatches() {
        val query = searchField.text.toString()
        if (query.isEmpty()) {
            message.text = context.getString(R.string.editor_find_empty)
            return
        }
        val replacement = replaceField.text.toString()
        val src = editor.text.toString()
        val regex = Regex(Regex.escape(query), RegexOption.IGNORE_CASE)
        val count = regex.findAll(src).count()
        if (count == 0) {
            message.text = context.getString(R.string.editor_find_no_match_fmt, query)
            return
        }
        replaceAllText(regex.replace(src, replacement))
        message.text = context.getString(R.string.editor_replaced_all_fmt, count)
    }

    private fun modeLabel(): String =
        if (spacesMode) context.getString(R.string.editor_spaces_mode)
        else context.getString(R.string.editor_tabs_mode)

    private fun widthLabel(): String = context.getString(R.string.editor_tab_width_fmt, tabWidth)

    private fun updateLineNumbers() {
        val count = editor.text.count { it == '\n' } + 1
        lineNumbers.text = (1..count).joinToString("\n")
    }

    private fun applyHighlighting(text: Editable) {
        if (highlighting) return
        highlighting = true
        text.getSpans(0, text.length, ForegroundColorSpan::class.java).forEach { text.removeSpan(it) }
        val src = text.toString()
        highlight(src, text, Regex("\"([^\"\\\\]|\\\\.)*\"|'([^'\\\\]|\\\\.)*'"), Color.rgb(170, 98, 25))
        highlight(src, text, Regex("(?m)#.*$|//.*$"), Color.rgb(96, 128, 96))
        val keywordPattern = when (file.name.lowercase()) {
            "dockerfile" -> "\\b(FROM|RUN|CMD|ENTRYPOINT|COPY|ADD|WORKDIR|ENV|ARG|EXPOSE|VOLUME|USER|LABEL)\\b"
            else -> "\\b(services|image|build|command|volumes|ports|environment|container_name|depends_on)\\b(?=\\s*:)"
        }
        highlight(src, text, Regex(keywordPattern, RegexOption.IGNORE_CASE), Color.rgb(32, 92, 190))
        highlighting = false
    }

    private fun highlight(src: String, editable: Editable, regex: Regex, color: Int) {
        regex.findAll(src).forEach { m ->
            editable.setSpan(
                ForegroundColorSpan(color),
                m.range.first,
                m.range.last + 1,
                Spannable.SPAN_EXCLUSIVE_EXCLUSIVE,
            )
        }
    }

    private fun lineStart(text: String, pos: Int): Int =
        text.lastIndexOf('\n', (pos - 1).coerceAtLeast(0)).let { if (it < 0) 0 else it + 1 }

    private fun lineEnd(text: String, pos: Int): Int =
        text.indexOf('\n', pos.coerceAtMost(text.length)).let { if (it < 0) text.length else it }

    private class VisibleWhitespaceTransformation : ReplacementTransformationMethod() {
        override fun getOriginal(): CharArray = charArrayOf(' ', '\t', '\u3000')
        override fun getReplacement(): CharArray = charArrayOf('·', '→', '□')
    }
}
