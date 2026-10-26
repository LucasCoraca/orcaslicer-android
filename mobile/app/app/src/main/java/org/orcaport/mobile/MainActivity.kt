package org.orcaport.mobile

import android.content.Intent
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import org.orcaport.mobile.ui.MainScreen
import org.orcaport.mobile.ui.OrcaTheme
import org.orcaport.mobile.ui.onShortcutKey
import org.orcaport.mobile.ui.onKeyBeforeFocus

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        val app = (application as? OrcaApp)?.state ?: AppState(this).also { OrcaApp.fallback = it }
        if (!app.ready && app.startupError == null && !app.starting) { app.starting = true; app.start(this) }
        intent?.let { app.openIntent(this, it) }
        setContent { OrcaTheme { MainScreen(app, this) } }
    }

    /** Android may end the app in the background: the unsaved project is backed up now (restored at the next start) */
    override fun onStop() {
        super.onStop()
        (application as? OrcaApp)?.state?.let { app -> if (app.ready) app.call("project.backup_now") }
    }

    /**
     * A hardware keyboard (MainFrame's CHAR_HOOK, then the focused window): while a text field takes typing, only the
     * Ctrl / Alt shortcuts come before it and the rest after it; otherwise the 3D view has the keys, as on the desktop.
     */
    override fun dispatchKeyEvent(event: android.view.KeyEvent): Boolean {
        val app = (application as? OrcaApp)?.state ?: return super.dispatchKeyEvent(event)
        if (event.keyCode == android.view.KeyEvent.KEYCODE_SHIFT_LEFT || event.keyCode == android.view.KeyEvent.KEYCODE_SHIFT_RIGHT)
            org.orcaport.mobile.engine.Native.vpShift(event.action == android.view.KeyEvent.ACTION_DOWN)
        val typing = (getSystemService(INPUT_METHOD_SERVICE) as android.view.inputmethod.InputMethodManager).isAcceptingText
        if (!typing) return app.onShortcutKey(event) || super.dispatchKeyEvent(event)
        if (app.onKeyBeforeFocus(event)) return true
        return super.dispatchKeyEvent(event) || app.onShortcutKey(event)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        (application as? OrcaApp)?.state?.openIntent(this, intent)
    }
}
