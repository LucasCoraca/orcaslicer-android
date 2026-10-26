package org.orcaport.mobile.engine

import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.runBlocking
import org.json.JSONObject

/** A question the engine is waiting on (a message box, the unsaved changes dialog ...). */
class Ask(val request: JSONObject) {
    internal val answer = CompletableDeferred<JSONObject>()
    fun reply(a: JSONObject) { answer.complete(a) }
}

class EngineEvent(val kind: String, val data: JSONObject)

/**
 * Callbacks of the native core (mobile/core/src/Host.cpp). onEvent may come from any thread; onAsk blocks the
 * calling engine thread until the UI answers, like a modal dialog on the desktop.
 */
class Host {
    val events = MutableSharedFlow<EngineEvent>(extraBufferCapacity = 256)
    /** the questions waiting for an answer, oldest first; the UI shows the first one */
    val asks = MutableStateFlow<List<Ask>>(emptyList())

    @Suppress("unused") // called from JNI
    fun onEvent(kind: String, data: String) {
        events.tryEmit(EngineEvent(kind, runCatching { JSONObject(data) }.getOrElse { JSONObject() }))
    }

    @Suppress("unused") // called from JNI
    fun onAsk(request: String): String {
        val ask = Ask(JSONObject(request))
        asks.value = asks.value + ask
        val a = runBlocking { ask.answer.await() }
        asks.value = asks.value - ask
        return a.toString()
    }
}
