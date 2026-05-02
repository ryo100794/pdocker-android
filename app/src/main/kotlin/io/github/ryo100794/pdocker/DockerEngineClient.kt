package io.github.ryo100794.pdocker

import android.net.LocalSocket
import android.net.LocalSocketAddress
import org.json.JSONArray
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.io.File
import java.net.URLEncoder
import java.util.Locale

class DockerEngineClient(private val socket: File) {
    data class Response(
        val status: Int,
        val headers: Map<String, String>,
        val body: ByteArray,
    ) {
        val text: String get() = body.toString(Charsets.UTF_8)
    }

    fun request(
        method: String,
        path: String,
        body: ByteArray = ByteArray(0),
        contentType: String = "application/json",
    ): Response {
        LocalSocket().use { ls ->
            ls.connect(LocalSocketAddress(socket.absolutePath, LocalSocketAddress.Namespace.FILESYSTEM))
            ls.soTimeout = 30_000
            val header = buildString {
                append(method).append(' ').append(path).append(" HTTP/1.1\r\n")
                append("Host: pdocker\r\n")
                append("Connection: close\r\n")
                if (body.isNotEmpty()) {
                    append("Content-Type: ").append(contentType).append("\r\n")
                    append("Content-Length: ").append(body.size).append("\r\n")
                }
                append("\r\n")
            }.toByteArray(Charsets.UTF_8)
            ls.outputStream.write(header)
            if (body.isNotEmpty()) ls.outputStream.write(body)
            ls.outputStream.flush()
            val raw = ls.inputStream.readBytes()
            return parse(raw)
        }
    }

    fun getArray(path: String): JSONArray {
        val resp = request("GET", path)
        require(resp.status in 200..299) { resp.text.ifBlank { "HTTP ${resp.status}" } }
        return JSONArray(resp.text.ifBlank { "[]" })
    }

    fun post(path: String): Response {
        val resp = request("POST", path)
        require(resp.status in 200..299) { resp.text.ifBlank { "HTTP ${resp.status}" } }
        return resp
    }

    fun postJson(path: String, json: JSONObject): Response {
        val resp = request("POST", path, json.toString().toByteArray(Charsets.UTF_8))
        require(resp.status in 200..299) { resp.text.ifBlank { "HTTP ${resp.status}" } }
        return resp
    }

    fun buildImage(contextDir: File, tag: String, dockerfile: String = "Dockerfile"): String {
        val tar = createTar(contextDir)
        val path = "/build?t=${encodeQuery(tag)}&dockerfile=${encodeQuery(dockerfile)}"
        val resp = request("POST", path, tar, "application/x-tar")
        require(resp.status in 200..299) { resp.text.ifBlank { "HTTP ${resp.status}" } }
        val text = decodeJsonStream(resp.text)
        require(!text.lines().any { it.startsWith("ERROR:") || it == "build failed" }) { text }
        return text
    }

    fun createContainer(name: String?, config: JSONObject): String {
        val path = if (name.isNullOrBlank()) "/containers/create"
        else "/containers/create?name=${encodeQuery(name)}"
        val resp = postJson(path, config)
        return JSONObject(resp.text).getString("Id")
    }

    fun logs(containerId: String, tail: Int = 200): String {
        val resp = request("GET", "/containers/${encodePath(containerId)}/logs?stdout=1&stderr=1&tail=$tail")
        require(resp.status in 200..299) { resp.text.ifBlank { "HTTP ${resp.status}" } }
        return decodeRawStream(resp.body)
    }

    companion object {
        fun encodePath(value: String): String =
            URLEncoder.encode(value, "UTF-8").replace("+", "%20")

        fun encodeQuery(value: String): String =
            URLEncoder.encode(value, "UTF-8").replace("+", "%20")

        fun decodeJsonStream(text: String): String =
            text.lineSequence()
                .map { it.trim() }
                .filter { it.isNotEmpty() }
                .map { line ->
                    runCatching {
                        val obj = JSONObject(line)
                        obj.optString("stream")
                            .ifBlank { obj.optString("status") }
                            .ifBlank { obj.optString("error") }
                            .ifBlank { line }
                    }.getOrDefault(line)
                }
                .joinToString("")

        private fun parse(raw: ByteArray): Response {
            val marker = "\r\n\r\n".toByteArray(Charsets.ISO_8859_1)
            val split = raw.indexOf(marker)
            val headBytes = if (split >= 0) raw.copyOfRange(0, split) else raw
            val body = if (split >= 0) raw.copyOfRange(split + marker.size, raw.size) else ByteArray(0)
            val lines = headBytes.toString(Charsets.ISO_8859_1).split("\r\n")
            val status = lines.firstOrNull()?.split(" ")?.getOrNull(1)?.toIntOrNull() ?: 0
            val headers = lines.drop(1)
                .mapNotNull { line ->
                    val pos = line.indexOf(':')
                    if (pos <= 0) null else line.substring(0, pos).lowercase() to line.substring(pos + 1).trim()
                }
                .toMap()
            return Response(status, headers, body)
        }

        private fun ByteArray.indexOf(needle: ByteArray): Int {
            if (needle.isEmpty() || size < needle.size) return -1
            for (i in 0..(size - needle.size)) {
                var ok = true
                for (j in needle.indices) {
                    if (this[i + j] != needle[j]) {
                        ok = false
                        break
                    }
                }
                if (ok) return i
            }
            return -1
        }

        private fun decodeRawStream(body: ByteArray): String {
            val out = StringBuilder()
            var i = 0
            while (i + 8 <= body.size) {
                val length = ((body[i + 4].toInt() and 0xff) shl 24) or
                    ((body[i + 5].toInt() and 0xff) shl 16) or
                    ((body[i + 6].toInt() and 0xff) shl 8) or
                    (body[i + 7].toInt() and 0xff)
                i += 8
                if (length < 0 || i + length > body.size) break
                out.append(body.copyOfRange(i, i + length).toString(Charsets.UTF_8))
                i += length
            }
            if (out.isEmpty() && body.isNotEmpty()) out.append(body.toString(Charsets.UTF_8))
            return out.toString()
        }

        private fun createTar(root: File): ByteArray {
            val out = ByteArrayOutputStream()
            root.walkTopDown()
                .filter { it != root }
                .filter { it.isFile }
                .forEach { file ->
                    val rel = root.toPath().relativize(file.toPath()).joinToString("/")
                    writeTarEntry(out, rel, file.readBytes(), file.lastModified() / 1000)
                }
            out.write(ByteArray(1024))
            return out.toByteArray()
        }

        private fun writeTarEntry(out: ByteArrayOutputStream, name: String, data: ByteArray, mtime: Long) {
            val header = ByteArray(512)
            putString(header, 0, 100, name)
            putOctal(header, 100, 8, 420)
            putOctal(header, 108, 8, 0)
            putOctal(header, 116, 8, 0)
            putOctal(header, 124, 12, data.size.toLong())
            putOctal(header, 136, 12, mtime)
            for (i in 148 until 156) header[i] = 0x20
            header[156] = '0'.code.toByte()
            putString(header, 257, 6, "ustar")
            putString(header, 263, 2, "00")
            val sum = header.sumOf { it.toInt() and 0xff }
            val chk = String.format(Locale.US, "%06o\u0000 ", sum).toByteArray(Charsets.US_ASCII)
            chk.copyInto(header, 148, 0, chk.size.coerceAtMost(8))
            out.write(header)
            out.write(data)
            val pad = (512 - (data.size % 512)) % 512
            if (pad > 0) out.write(ByteArray(pad))
        }

        private fun putString(buf: ByteArray, offset: Int, length: Int, value: String) {
            val bytes = value.toByteArray(Charsets.UTF_8)
            bytes.copyInto(buf, offset, 0, bytes.size.coerceAtMost(length))
        }

        private fun putOctal(buf: ByteArray, offset: Int, length: Int, value: Long) {
            val s = String.format(Locale.US, "%0${length - 1}o\u0000", value)
            putString(buf, offset, length, s)
        }
    }
}
