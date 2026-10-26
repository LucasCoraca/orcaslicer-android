package org.orcaport.mobile.engine

import android.content.Context
import android.util.Log
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.asCoroutineDispatcher
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.util.Locale
import java.util.concurrent.Executors
import java.util.zip.ZipInputStream

class EngineException(msg: String) : Exception(msg)

/**
 * OrcaSlicer's engine (liborcacore.so) in-process. Every command runs on one engine thread, as the desktop GUI runs
 * its logic on the UI thread; long work (slicing) runs on the engine's own workers and reports through events.
 */
class Engine(private val ctx: Context) {
    val host = Host()
    private val thread = Executors.newSingleThreadExecutor { Thread(it, "orca-engine").apply { priority = Thread.NORM_PRIORITY } }
    val dispatcher: CoroutineDispatcher = thread.asCoroutineDispatcher()
    @Volatile var ready = false; private set

    val resourcesDir get() = File(ctx.filesDir, "resources")
    val dataDir get() = File(ctx.filesDir, "OrcaSlicer")
    val cacheDir: File get() = ctx.cacheDir

    /** unpack resources (first run / after an update), then load the presets. Returns the init result. */
    suspend fun start(progress: (String) -> Unit): JSONObject = withContext(dispatcher) {
        val stamp = File(ctx.filesDir, "resources.stamp")
        val want = ctx.packageManager.getPackageInfo(ctx.packageName, 0).lastUpdateTime.toString()
        if (!stamp.exists() || stamp.readText() != want || !resourcesDir.exists()) {
            progress("Unpacking resources…")
            resourcesDir.deleteRecursively()
            ctx.assets.open("resources.zip").use { unzip(it, resourcesDir) }
            // the plugin system's Python extension modules of this ABI, next to the stdlib
            runCatching { ctx.assets.open("python-dynload.zip").use { unzip(it, File(resourcesDir, "python/lib/python3.14")) } }
            // the files on disk before the stamp: a crash right after the unpack must not leave empty files behind a stamp
            runCatching { Runtime.getRuntime().exec(arrayOf("/system/bin/sync")).waitFor() }
            stamp.writeText(want)   // last: an interrupted unpack is redone
        }
        // the plugin terminal's python (PythonInterpreter::bundled_python_executable: <home>/bin/python3): the launcher
        // installed with the native libraries, which move on every update
        runCatching {
            val bin = File(resourcesDir, "python/bin").apply { mkdirs() }
            val link = File(bin, "python3")
            link.delete()
            android.system.Os.symlink(File(ctx.applicationInfo.nativeLibraryDir, "libpython3_exe.so").path, link.path)
        }
        progress("Loading printer & filament profiles")
        val lang = Locale.getDefault().language
        val r = unwrap(Native.init(host, resourcesDir.absolutePath, dataDir.absolutePath, lang)) as JSONObject
        ready = true
        r
    }

    private fun unzip(input: java.io.InputStream, dest: File) {
        ZipInputStream(input.buffered(1 shl 16)).use { zip ->
            val buf = ByteArray(1 shl 16)
            while (true) {
                val e = zip.nextEntry ?: break
                val f = File(dest, e.name)
                if (!f.canonicalPath.startsWith(dest.canonicalPath)) continue
                if (e.isDirectory) { f.mkdirs(); continue }
                f.parentFile?.mkdirs()
                f.outputStream().use { o -> while (true) { val n = zip.read(buf); if (n <= 0) break; o.write(buf, 0, n) } }
            }
        }
    }

    private fun unwrap(result: String): Any? {
        val j = JSONObject(result)
        if (j.has("error")) throw EngineException(j.getString("error"))
        return j.opt("ok")
    }

    /** run a core command on the engine thread */
    suspend fun call(cmd: String, args: JSONObject = JSONObject()): Any? = withContext(dispatcher) {
        val t0 = System.nanoTime()
        val r = unwrap(Native.call(cmd, args.toString()))
        val ms = (System.nanoTime() - t0) / 1_000_000
        if (ms > 200) Log.i("Engine", "$cmd took $ms ms")
        r
    }

    /** a stateless core command (PureCommands): answered off the engine thread, also while a dialog holds the engine */
    suspend fun pure(cmd: String, args: JSONObject = JSONObject()): Any? = withContext(kotlinx.coroutines.Dispatchers.Default) { unwrap(Native.pure(cmd, args.toString())) }

    suspend fun obj(cmd: String, args: JSONObject = JSONObject()) = call(cmd, args) as JSONObject
    suspend fun arr(cmd: String, args: JSONObject = JSONObject()) = call(cmd, args) as JSONArray
}

fun jobj(vararg pairs: Pair<String, Any?>) = JSONObject().apply { for ((k, v) in pairs) put(k, v ?: JSONObject.NULL) }
