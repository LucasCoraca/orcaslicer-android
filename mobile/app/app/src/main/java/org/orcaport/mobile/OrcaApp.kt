package org.orcaport.mobile

import android.app.Application

/** Holds the engine and UI state for the process lifetime (the activity is recreated on configuration changes). */
class OrcaApp : Application() {
    val state by lazy { AppState(this) }

    companion object {
        var fallback: AppState? = null
    }
}
