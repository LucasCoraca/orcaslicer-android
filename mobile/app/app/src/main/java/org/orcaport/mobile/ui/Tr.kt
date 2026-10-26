package org.orcaport.mobile.ui

import org.orcaport.mobile.engine.Native

/**
 * _L() / _L_CONTEXT() for the app's own UI strings: Orca's msgids looked up in the engine's gettext catalog of the
 * chosen language (the same one the engine and the web pages use). English until the engine has started.
 */
object Tr {
    @Volatile var ready = false
    private val cache = java.util.concurrent.ConcurrentHashMap<String, String>()
    fun reset() = cache.clear()
    fun get(s: String, ctx: String? = null): String {
        if (!ready || s.isEmpty()) return s
        val key = if (ctx == null) s else "$ctx\u0004$s"
        return cache.getOrPut(key) { runCatching { Native.tr(s, ctx) }.getOrDefault(s) }
    }
}

fun tr(s: String) = Tr.get(s)
fun trc(s: String, ctx: String) = Tr.get(s, ctx)
