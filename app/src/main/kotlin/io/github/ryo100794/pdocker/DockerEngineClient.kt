package io.github.ryo100794.pdocker

import android.net.LocalSocket
import android.net.LocalSocketAddress
import org.json.JSONArray
import java.io.File
import java.net.URLEncoder

class DockerEngineClient(private val socket: File) {
    data class Response(
        val status: Int,
        val headers: Map<String, String>,
        val body: ByteArray,
    ) {
        val text: String get() = body.toString(Charsets.UTF_8)
    }

    fun request(method: String, path: String, body: ByteArray = ByteArray(0)): Response {
        LocalSocket().use { ls ->
            ls.connect(LocalSocketAddress(socket.absolutePath, LocalSocketAddress.Namespace.FILESYSTEM))
            ls.soTimeout = 30_000
            val header = buildString {
                append(method).append(' ').append(path).append(" HTTP/1.1\r\n")
                append("Host: pdocker\r\n")
                append("Connection: close\r\n")
                if (body.isNotEmpty()) {
                    append("Content-Type: application/json\r\n")
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

    fun logs(containerId: String, tail: Int = 200): String {
        val resp = request("GET", "/containers/${encodePath(containerId)}/logs?stdout=1&stderr=1&tail=$tail")
        require(resp.status in 200..299) { resp.text.ifBlank { "HTTP ${resp.status}" } }
        return decodeRawStream(resp.body)
    }

    companion object {
        fun encodePath(value: String): String =
            URLEncoder.encode(value, "UTF-8").replace("+", "%20")

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
    }
}
