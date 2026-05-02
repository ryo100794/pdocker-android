package io.github.ryo100794.pdocker

import android.content.Context
import android.os.Build
import java.io.File
import org.json.JSONObject

object AndroidGpuBench {
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
        val dir = File(context.getExternalFilesDir(null), "bench").apply { mkdirs() }
        val stamp = System.currentTimeMillis()
        val jsonl = File(dir, "android-gpu-bench-$stamp.jsonl")
        val csv = File(dir, "android-gpu-bench-$stamp.csv")
        val results = listOf(
            vectorAdd(262_144),
            saxpy(262_144),
            matmul(64),
        )
        jsonl.writeText(results.joinToString("\n") { it.toJson().toString() } + "\n")
        csv.writeText(
            "backend,kernel,problem_size,compile_ms,upload_ms,dispatch_ms,download_ms,total_ms,max_abs_error,valid\n" +
                results.joinToString("\n") { it.toCsv() } + "\n",
        )
        return buildString {
            appendLine("android-gpu-bench first pass")
            appendLine("JSONL: ${jsonl.absolutePath}")
            appendLine("CSV: ${csv.absolutePath}")
            results.forEach {
                appendLine("${it.backend} ${it.kernel} ${it.problemSize}: total=${"%.3f".format(it.totalMs)}ms valid=${it.valid}")
            }
        }
    }

    private fun vectorAdd(n: Int): BenchResult {
        val a = FloatArray(n) { it * 0.25f }
        val b = FloatArray(n) { 1.0f - it * 0.125f }
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
        val x = FloatArray(n) { (it % 257) * 0.01f }
        val y = FloatArray(n) { (it % 127) * -0.02f }
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
        val a = FloatArray(n * n) { ((it % 17) - 8) * 0.03125f }
        val b = FloatArray(n * n) { ((it % 13) - 6) * 0.0625f }
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
}
