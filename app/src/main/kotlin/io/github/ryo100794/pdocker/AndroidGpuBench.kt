package io.github.ryo100794.pdocker

import android.content.Context
import android.opengl.EGL14
import android.opengl.EGLConfig
import android.opengl.EGLContext
import android.opengl.EGLDisplay
import android.opengl.EGLSurface
import android.opengl.GLES20
import android.opengl.GLES30
import android.opengl.GLES31
import android.os.Build
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import org.json.JSONObject

object AndroidGpuBench {
    private const val VECTOR_N = 262_144
    private const val MATMUL_N = 64
    private const val EGL_OPENGL_ES3_BIT_KHR = 0x00000040

    private data class BenchResult(
        val backend: String,
        val kernel: String,
        val problemSize: String,
        val compileMs: Double,
        val uploadMs: Double,
        val dispatchMs: Double,
        val downloadMs: Double,
        val totalMs: Double,
        val maxAbsError: Double,
        val valid: Boolean,
    ) {
        fun toJson(): JSONObject =
            JSONObject().apply {
                put("backend", backend)
                put("kernel", kernel)
                put("problem_size", problemSize)
                put("compile_ms", compileMs)
                put("upload_ms", uploadMs)
                put("dispatch_ms", dispatchMs)
                put("download_ms", downloadMs)
                put("total_ms", totalMs)
                put("max_abs_error", maxAbsError)
                put("valid", valid)
                put("device", JSONObject().apply {
                    put("manufacturer", Build.MANUFACTURER)
                    put("model", Build.MODEL)
                    put("sdk", Build.VERSION.SDK_INT)
                    put("abis", Build.SUPPORTED_ABIS.joinToString(","))
                })
            }

        fun toCsv(): String =
            listOf(
                backend,
                kernel,
                problemSize,
                "%.4f".format(compileMs),
                "%.4f".format(uploadMs),
                "%.4f".format(dispatchMs),
                "%.4f".format(downloadMs),
                "%.4f".format(totalMs),
                "%.8f".format(maxAbsError),
                valid.toString(),
            ).joinToString(",")
    }

    fun run(context: Context): String {
        val externalDir = File(context.getExternalFilesDir(null) ?: context.filesDir, "bench")
        val internalDir = File(context.filesDir, "pdocker/bench")
        val stamp = System.currentTimeMillis()
        val warnings = mutableListOf<String>()
        val results = mutableListOf(
            vectorAdd(VECTOR_N),
            saxpy(VECTOR_N),
            matmul(MATMUL_N),
        )
        runCatching { GlesComputeBench().use { results += it.runAll() } }
            .onFailure { warnings += "gles31_compute skipped: ${it.message.orEmpty()}" }
        val written = mutableListOf<Pair<File, File>>()
        written += writeResults(internalDir, stamp, results)
        if (externalDir.absolutePath != internalDir.absolutePath) {
            runCatching { writeResults(externalDir, stamp, results) }
                .onSuccess { written += it }
                .onFailure { warnings += "external bench write skipped: ${it.message.orEmpty()}" }
        }
        return buildString {
            appendLine("android-gpu-bench first pass")
            written.forEach { (jsonl, csv) ->
                appendLine("JSONL: ${jsonl.absolutePath}")
                appendLine("CSV: ${csv.absolutePath}")
            }
            warnings.forEach { appendLine(it) }
            results.forEach {
                appendLine("${it.backend} ${it.kernel} ${it.problemSize}: total=${"%.3f".format(it.totalMs)}ms valid=${it.valid}")
            }
            appendComparisons(results, this)
        }
    }

    private fun writeResults(dir: File, stamp: Long, results: List<BenchResult>): Pair<File, File> {
        dir.mkdirs()
        val jsonl = File(dir, "android-gpu-bench-$stamp.jsonl")
        val csv = File(dir, "android-gpu-bench-$stamp.csv")
        jsonl.writeText(results.joinToString("\n") { it.toJson().toString() } + "\n")
        csv.writeText(
            "backend,kernel,problem_size,compile_ms,upload_ms,dispatch_ms,download_ms,total_ms,max_abs_error,valid\n" +
                results.joinToString("\n") { it.toCsv() } + "\n",
        )
        return jsonl to csv
    }

    private fun vectorAdd(n: Int): BenchResult {
        val (a, b) = vectorInputs(n)
        val out = FloatArray(n)
        val elapsed = elapsedMs {
            for (i in 0 until n) out[i] = a[i] + b[i]
        }
        var maxErr = 0.0
        for (i in 0 until n) {
            maxErr = maxOf(maxErr, kotlin.math.abs(out[i] - (a[i] + b[i])).toDouble())
        }
        return result("vector_add", "n=$n", elapsed, maxErr)
    }

    private fun saxpy(n: Int): BenchResult {
        val alpha = 1.75f
        val (x, y) = saxpyInputs(n)
        val out = FloatArray(n)
        val elapsed = elapsedMs {
            for (i in 0 until n) out[i] = alpha * x[i] + y[i]
        }
        var maxErr = 0.0
        for (i in 0 until n) {
            maxErr = maxOf(maxErr, kotlin.math.abs(out[i] - (alpha * x[i] + y[i])).toDouble())
        }
        return result("saxpy", "n=$n", elapsed, maxErr)
    }

    private fun matmul(n: Int): BenchResult {
        val (a, b) = matmulInputs(n)
        val c = FloatArray(n * n)
        val elapsed = elapsedMs {
            for (row in 0 until n) {
                for (col in 0 until n) {
                    var sum = 0.0f
                    for (k in 0 until n) {
                        sum += a[row * n + k] * b[k * n + col]
                    }
                    c[row * n + col] = sum
                }
            }
        }
        val valid = c.all { it.isFinite() }
        return result("matmul_fp32", "n=${n}x$n", elapsed, if (valid) 0.0 else Double.POSITIVE_INFINITY)
    }

    private fun vectorInputs(n: Int): Pair<FloatArray, FloatArray> =
        FloatArray(n) { it * 0.25f } to FloatArray(n) { 1.0f - it * 0.125f }

    private fun saxpyInputs(n: Int): Pair<FloatArray, FloatArray> =
        FloatArray(n) { (it % 257) * 0.01f } to FloatArray(n) { (it % 127) * -0.02f }

    private fun matmulInputs(n: Int): Pair<FloatArray, FloatArray> =
        FloatArray(n * n) { ((it % 17) - 8) * 0.03125f } to
            FloatArray(n * n) { ((it % 13) - 6) * 0.0625f }

    private fun result(kernel: String, size: String, dispatchMs: Double, maxErr: Double): BenchResult =
        BenchResult(
            backend = "cpu_scalar",
            kernel = kernel,
            problemSize = size,
            compileMs = 0.0,
            uploadMs = 0.0,
            dispatchMs = dispatchMs,
            downloadMs = 0.0,
            totalMs = dispatchMs,
            maxAbsError = maxErr,
            valid = maxErr <= 0.0001,
        )

    private fun elapsedMs(block: () -> Unit): Double {
        val start = System.nanoTime()
        block()
        return (System.nanoTime() - start) / 1_000_000.0
    }

    private fun appendComparisons(results: List<BenchResult>, out: StringBuilder) {
        val cpu = results.filter { it.backend == "cpu_scalar" && it.valid }.associateBy { it.kernel }
        val gpu = results.filter { it.backend == "gles31_compute" && it.valid }.associateBy { it.kernel }
        gpu.keys.sorted().forEach { kernel ->
            val c = cpu[kernel] ?: return@forEach
            val g = gpu[kernel] ?: return@forEach
            val speedup = c.totalMs / g.totalMs
            out.appendLine("compare $kernel: cpu/gles31=${"%.2f".format(speedup)}x")
        }
    }

    private fun FloatArray.toNativeBuffer(): ByteBuffer =
        ByteBuffer.allocateDirect(size * 4).order(ByteOrder.nativeOrder()).also { buffer ->
            buffer.asFloatBuffer().put(this)
            buffer.position(0)
        }

    private class GlesComputeBench : AutoCloseable {
        private val display: EGLDisplay
        private val surface: EGLSurface
        private val context: EGLContext

        init {
            display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY)
            check(display != EGL14.EGL_NO_DISPLAY) { "EGL display unavailable" }
            check(EGL14.eglInitialize(display, IntArray(2), 0, IntArray(2), 0)) { "eglInitialize failed" }
            val configs = arrayOfNulls<EGLConfig>(1)
            val count = IntArray(1)
            val attrs = intArrayOf(
                EGL14.EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
                EGL14.EGL_SURFACE_TYPE, EGL14.EGL_PBUFFER_BIT,
                EGL14.EGL_RED_SIZE, 8,
                EGL14.EGL_GREEN_SIZE, 8,
                EGL14.EGL_BLUE_SIZE, 8,
                EGL14.EGL_NONE,
            )
            check(EGL14.eglChooseConfig(display, attrs, 0, configs, 0, 1, count, 0) && count[0] > 0) {
                "EGL ES3 config unavailable"
            }
            val config = configs[0] ?: error("EGL config missing")
            context = EGL14.eglCreateContext(
                display,
                config,
                EGL14.EGL_NO_CONTEXT,
                intArrayOf(EGL14.EGL_CONTEXT_CLIENT_VERSION, 3, EGL14.EGL_NONE),
                0,
            )
            check(context != EGL14.EGL_NO_CONTEXT) { "EGL ES3 context unavailable" }
            surface = EGL14.eglCreatePbufferSurface(
                display,
                config,
                intArrayOf(EGL14.EGL_WIDTH, 1, EGL14.EGL_HEIGHT, 1, EGL14.EGL_NONE),
                0,
            )
            check(surface != EGL14.EGL_NO_SURFACE) { "EGL pbuffer unavailable" }
            check(EGL14.eglMakeCurrent(display, surface, surface, context)) { "eglMakeCurrent failed" }
            val version = GLES20.glGetString(GLES20.GL_VERSION).orEmpty()
            check("OpenGL ES 3." in version || "OpenGL ES 4." in version) { "OpenGL ES 3.1 unavailable: $version" }
        }

        fun runAll(): List<BenchResult> =
            listOf(vectorAdd(VECTOR_N), saxpy(VECTOR_N), matmul(MATMUL_N))

        private fun vectorAdd(n: Int): BenchResult {
            val (a, b) = vectorInputs(n)
            val out = runVectorKernel(
                kernel = "vector_add",
                n = n,
                srcA = a,
                srcB = b,
                shader = """
                    #version 310 es
                    layout(local_size_x = 256) in;
                    layout(std430, binding = 0) readonly buffer A { float a[]; };
                    layout(std430, binding = 1) readonly buffer B { float b[]; };
                    layout(std430, binding = 2) writeonly buffer O { float o[]; };
                    uniform int n;
                    void main() {
                      uint i = gl_GlobalInvocationID.x;
                      if (i < uint(n)) { o[i] = a[i] + b[i]; }
                    }
                """.trimIndent(),
            )
            var maxErr = 0.0
            for (i in 0 until n) maxErr = maxOf(maxErr, kotlin.math.abs(out.values[i] - (a[i] + b[i])).toDouble())
            return gpuResult("vector_add", "n=$n", out, maxErr)
        }

        private fun saxpy(n: Int): BenchResult {
            val alpha = 1.75f
            val (x, y) = saxpyInputs(n)
            val out = runVectorKernel(
                kernel = "saxpy",
                n = n,
                srcA = x,
                srcB = y,
                alpha = alpha,
                shader = """
                    #version 310 es
                    layout(local_size_x = 256) in;
                    layout(std430, binding = 0) readonly buffer X { float x[]; };
                    layout(std430, binding = 1) readonly buffer Y { float y[]; };
                    layout(std430, binding = 2) writeonly buffer O { float o[]; };
                    uniform int n;
                    uniform float alpha;
                    void main() {
                      uint i = gl_GlobalInvocationID.x;
                      if (i < uint(n)) { o[i] = alpha * x[i] + y[i]; }
                    }
                """.trimIndent(),
            )
            var maxErr = 0.0
            for (i in 0 until n) maxErr = maxOf(maxErr, kotlin.math.abs(out.values[i] - (alpha * x[i] + y[i])).toDouble())
            return gpuResult("saxpy", "n=$n", out, maxErr)
        }

        private fun matmul(n: Int): BenchResult {
            val (a, b) = matmulInputs(n)
            val out = runMatmulKernel(n, a, b)
            var maxErr = 0.0
            for (row in 0 until n) {
                for (col in 0 until n) {
                    var expected = 0.0f
                    for (k in 0 until n) expected += a[row * n + k] * b[k * n + col]
                    maxErr = maxOf(maxErr, kotlin.math.abs(out.values[row * n + col] - expected).toDouble())
                }
            }
            return gpuResult("matmul_fp32", "n=${n}x$n", out, maxErr)
        }

        private data class GpuRun(
            val values: FloatArray,
            val compileMs: Double,
            val uploadMs: Double,
            val dispatchMs: Double,
            val downloadMs: Double,
        ) {
            val totalMs: Double get() = compileMs + uploadMs + dispatchMs + downloadMs
        }

        private fun gpuResult(kernel: String, size: String, run: GpuRun, maxErr: Double): BenchResult =
            BenchResult(
                backend = "gles31_compute",
                kernel = kernel,
                problemSize = size,
                compileMs = run.compileMs,
                uploadMs = run.uploadMs,
                dispatchMs = run.dispatchMs,
                downloadMs = run.downloadMs,
                totalMs = run.totalMs,
                maxAbsError = maxErr,
                valid = maxErr <= 0.001,
            )

        private fun runVectorKernel(
            kernel: String,
            n: Int,
            srcA: FloatArray,
            srcB: FloatArray,
            alpha: Float? = null,
            shader: String,
        ): GpuRun {
            val compileStart = System.nanoTime()
            val program = createProgram(shader)
            val compileMs = sinceMs(compileStart)
            val buffers = IntArray(3)
            GLES20.glGenBuffers(3, buffers, 0)
            val uploadStart = System.nanoTime()
            uploadBuffer(buffers[0], 0, srcA)
            uploadBuffer(buffers[1], 1, srcB)
            uploadBuffer(buffers[2], 2, FloatArray(n))
            GLES20.glFinish()
            val uploadMs = sinceMs(uploadStart)
            GLES20.glUseProgram(program)
            GLES20.glUniform1i(GLES20.glGetUniformLocation(program, "n"), n)
            if (alpha != null) GLES20.glUniform1f(GLES20.glGetUniformLocation(program, "alpha"), alpha)
            val dispatchMs = elapsedMs {
                GLES31.glDispatchCompute((n + 255) / 256, 1, 1)
                GLES31.glMemoryBarrier(GLES31.GL_SHADER_STORAGE_BARRIER_BIT)
                GLES20.glFinish()
            }
            val downloadStart = System.nanoTime()
            val out = downloadBuffer(buffers[2], n)
            val downloadMs = sinceMs(downloadStart)
            GLES20.glDeleteBuffers(3, buffers, 0)
            GLES20.glDeleteProgram(program)
            checkGl("$kernel complete")
            return GpuRun(out, compileMs, uploadMs, dispatchMs, downloadMs)
        }

        private fun runMatmulKernel(n: Int, a: FloatArray, b: FloatArray): GpuRun {
            val shader = """
                #version 310 es
                layout(local_size_x = 8, local_size_y = 8) in;
                layout(std430, binding = 0) readonly buffer A { float a[]; };
                layout(std430, binding = 1) readonly buffer B { float b[]; };
                layout(std430, binding = 2) writeonly buffer C { float c[]; };
                uniform int n;
                void main() {
                  uint col = gl_GlobalInvocationID.x;
                  uint row = gl_GlobalInvocationID.y;
                  if (row >= uint(n) || col >= uint(n)) { return; }
                  float sum = 0.0;
                  for (int k = 0; k < n; ++k) {
                    sum += a[int(row) * n + k] * b[k * n + int(col)];
                  }
                  c[int(row) * n + int(col)] = sum;
                }
            """.trimIndent()
            val compileStart = System.nanoTime()
            val program = createProgram(shader)
            val compileMs = sinceMs(compileStart)
            val buffers = IntArray(3)
            GLES20.glGenBuffers(3, buffers, 0)
            val uploadStart = System.nanoTime()
            uploadBuffer(buffers[0], 0, a)
            uploadBuffer(buffers[1], 1, b)
            uploadBuffer(buffers[2], 2, FloatArray(n * n))
            GLES20.glFinish()
            val uploadMs = sinceMs(uploadStart)
            GLES20.glUseProgram(program)
            GLES20.glUniform1i(GLES20.glGetUniformLocation(program, "n"), n)
            val dispatchMs = elapsedMs {
                GLES31.glDispatchCompute((n + 7) / 8, (n + 7) / 8, 1)
                GLES31.glMemoryBarrier(GLES31.GL_SHADER_STORAGE_BARRIER_BIT)
                GLES20.glFinish()
            }
            val downloadStart = System.nanoTime()
            val out = downloadBuffer(buffers[2], n * n)
            val downloadMs = sinceMs(downloadStart)
            GLES20.glDeleteBuffers(3, buffers, 0)
            GLES20.glDeleteProgram(program)
            checkGl("matmul complete")
            return GpuRun(out, compileMs, uploadMs, dispatchMs, downloadMs)
        }

        private fun uploadBuffer(id: Int, binding: Int, values: FloatArray) {
            GLES20.glBindBuffer(GLES31.GL_SHADER_STORAGE_BUFFER, id)
            GLES20.glBufferData(GLES31.GL_SHADER_STORAGE_BUFFER, values.size * 4, values.toNativeBuffer(), GLES20.GL_STATIC_DRAW)
            GLES30.glBindBufferBase(GLES31.GL_SHADER_STORAGE_BUFFER, binding, id)
            checkGl("upload buffer $binding")
        }

        private fun downloadBuffer(id: Int, n: Int): FloatArray {
            GLES20.glBindBuffer(GLES31.GL_SHADER_STORAGE_BUFFER, id)
            val mapped = GLES30.glMapBufferRange(
                GLES31.GL_SHADER_STORAGE_BUFFER,
                0,
                n * 4,
                GLES30.GL_MAP_READ_BIT,
            ) as? ByteBuffer ?: error("glMapBufferRange returned null")
            mapped.order(ByteOrder.nativeOrder())
            val out = FloatArray(n)
            mapped.asFloatBuffer().get(out)
            GLES30.glUnmapBuffer(GLES31.GL_SHADER_STORAGE_BUFFER)
            checkGl("download buffer")
            return out
        }

        private fun createProgram(source: String): Int {
            val shader = GLES31.glCreateShader(GLES31.GL_COMPUTE_SHADER)
            GLES20.glShaderSource(shader, source)
            GLES20.glCompileShader(shader)
            val status = IntArray(1)
            GLES20.glGetShaderiv(shader, GLES20.GL_COMPILE_STATUS, status, 0)
            if (status[0] == 0) {
                val log = GLES20.glGetShaderInfoLog(shader)
                GLES20.glDeleteShader(shader)
                error("compute shader compile failed: $log")
            }
            val program = GLES20.glCreateProgram()
            GLES20.glAttachShader(program, shader)
            GLES20.glLinkProgram(program)
            GLES20.glDeleteShader(shader)
            GLES20.glGetProgramiv(program, GLES20.GL_LINK_STATUS, status, 0)
            if (status[0] == 0) {
                val log = GLES20.glGetProgramInfoLog(program)
                GLES20.glDeleteProgram(program)
                error("compute program link failed: $log")
            }
            checkGl("create compute program")
            return program
        }

        private fun checkGl(label: String) {
            val err = GLES20.glGetError()
            check(err == GLES20.GL_NO_ERROR) { "$label GL error 0x${err.toString(16)}" }
        }

        override fun close() {
            EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT)
            EGL14.eglDestroySurface(display, surface)
            EGL14.eglDestroyContext(display, context)
            EGL14.eglTerminate(display)
        }
    }

    private inline fun <T : AutoCloseable, R> T.use(block: (T) -> R): R {
        var failure: Throwable? = null
        try {
            return block(this)
        } catch (t: Throwable) {
            failure = t
            throw t
        } finally {
            if (failure == null) close() else runCatching { close() }
        }
    }

    private fun sinceMs(startNs: Long): Double =
        (System.nanoTime() - startNs) / 1_000_000.0
}
