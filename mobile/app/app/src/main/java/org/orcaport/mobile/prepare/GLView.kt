package org.orcaport.mobile.prepare

import android.annotation.SuppressLint
import android.content.Context
import android.opengl.GLSurfaceView
import android.view.GestureDetector
import android.view.MotionEvent
import android.view.ScaleGestureDetector
import org.json.JSONArray
import org.json.JSONObject
import org.orcaport.mobile.engine.Native
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10
import kotlin.math.abs
import kotlin.math.hypot

/**
 * The 3D view (native renderer in liborcacore, Viewport.cpp). Renders on demand. Touch, as on the desktop canvas
 * with a mouse: tap selects (and picks the plate), long press opens the context menu, a drag that starts on a selected
 * object moves it, other one-finger drags orbit, two fingers pan and pinch-zoom.
 */
@SuppressLint("ViewConstructor", "ClickableViewAccessibility")
/** the GL renderer of the 3D view (OpenGLManager::get_gl_info, for the Troubleshoot Center) */
object GLInfo {
    @Volatile var renderer = ""
    @Volatile var glsl = ""
}

class GLView(ctx: Context, private val listener: Listener) : GLSurfaceView(ctx) {
    interface Listener {
        fun onPick(hit: JSONObject, add: Boolean)
        fun onLongPress(hit: JSONObject, x: Float, y: Float)
        /** a drag ended: a JSON array (world delta of a plain move) or a gizmo commit {gizmo, matrix, name} */
        fun onMoved(result: String)
        fun onDoubleTap(hit: JSONObject)
        /** a frame was drawn (UI thread): overlays that follow the camera refresh here */
        fun onFrame()
    }

    private val frameNotify = Runnable { listener.onFrame() }

    @Volatile private var animating = false

    init {
        setEGLContextClientVersion(3)
        setEGLConfigChooser(8, 8, 8, 8, 24, 8)   // stencil: the selection outline (GLVolume::render_with_outline)
        preserveEGLContextOnPause = true
        setRenderer(object : Renderer {
            override fun onSurfaceCreated(gl: GL10?, config: EGLConfig?) {
                GLInfo.renderer = android.opengl.GLES30.glGetString(android.opengl.GLES30.GL_RENDERER) ?: ""
                GLInfo.glsl = android.opengl.GLES30.glGetString(android.opengl.GLES30.GL_SHADING_LANGUAGE_VERSION) ?: ""
                Native.glCreated()
            }
            override fun onSurfaceChanged(gl: GL10?, w: Int, h: Int) = Native.glResize(w, h)
            override fun onDrawFrame(gl: GL10?) {
                animating = Native.glDraw()
                if (animating) requestRender()
                removeCallbacks(frameNotify)
                post(frameNotify)
            }
        })
        renderMode = RENDERMODE_WHEN_DIRTY
        Native.vpUiScale(ctx.resources.displayMetrics.density)
    }

    private var dragging = false        // moving the selection
    private var orbiting = false
    private var multi = false
    private var lastX = 0f
    private var lastY = 0f
    private var downX = 0f
    private var downY = 0f
    private var lastSpanX = 0f
    private var lastSpanY = 0f

    private val scale = ScaleGestureDetector(ctx, object : ScaleGestureDetector.SimpleOnScaleGestureListener() {
        override fun onScale(d: ScaleGestureDetector): Boolean {
            Native.vpZoom(d.scaleFactor.toDouble(), d.focusX.toDouble(), d.focusY.toDouble())
            requestRender()
            return true
        }
    }).apply { isQuickScaleEnabled = false }

    private val gestures = GestureDetector(ctx, object : GestureDetector.SimpleOnGestureListener() {
        override fun onSingleTapConfirmed(e: MotionEvent): Boolean {
            // GLCanvas3D::on_mouse: a center of mass marker takes the click before the selection
            if (Native.vpComTap(e.x.toDouble(), e.y.toDouble())) { requestRender(); return true }
            listener.onPick(JSONObject(Native.vpPick(e.x.toDouble(), e.y.toDouble())).put("x", e.x.toDouble()).put("y", e.y.toDouble()), false)
            return true
        }
        override fun onDoubleTap(e: MotionEvent): Boolean {
            listener.onDoubleTap(JSONObject(Native.vpPick(e.x.toDouble(), e.y.toDouble())))
            return true
        }
        override fun onLongPress(e: MotionEvent) {
            if (dragging || orbiting || multi) return
            listener.onLongPress(JSONObject(Native.vpPick(e.x.toDouble(), e.y.toDouble())), e.x, e.y)
        }
    })

    private var layersEditing = false   // the finger is on the variable layer height bar

    private var navigating = false   // the finger is on the 3D navigator (GLCanvas3D::_render_3d_navigator)

    override fun onTouchEvent(e: MotionEvent): Boolean {
        val navAction = when (e.actionMasked) { MotionEvent.ACTION_DOWN -> 0; MotionEvent.ACTION_MOVE -> 1; MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> 2; else -> -1 }
        val wasNavigating = navigating
        if (navAction == 0) navigating = Native.vpNavTouch(e.x, e.y, 0)
        else if (navigating && navAction > 0) {
            Native.vpNavTouch(e.x, e.y, navAction)
            if (navAction == 2) navigating = false
        }
        if (navigating || wasNavigating) { requestRender(); return true }
        // GLCanvas3D: a press inside the layer thickness bar edits the profile until the finger lifts
        if (e.actionMasked == MotionEvent.ACTION_DOWN && Native.vpLayersPress(e.x.toDouble(), e.y.toDouble())) { layersEditing = true; requestRender(); return true }
        if (layersEditing) {
            when (e.actionMasked) {
                MotionEvent.ACTION_MOVE -> Native.vpLayersMove(e.x.toDouble(), e.y.toDouble())
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> { Native.vpLayersRelease(); layersEditing = false; listener.onMoved("{\"gizmo\":\"layers\"}") }
            }
            requestRender()
            return true
        }
        scale.onTouchEvent(e)
        if (e.pointerCount == 1 && !multi) gestures.onTouchEvent(e)
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                lastX = e.x; lastY = e.y; downX = e.x; downY = e.y
                dragging = false; orbiting = false; multi = false
            }
            MotionEvent.ACTION_POINTER_DOWN -> {
                if (dragging) { finishDrag() }
                multi = true
                lastSpanX = focusX(e); lastSpanY = focusY(e)
            }
            MotionEvent.ACTION_MOVE -> {
                if (multi && e.pointerCount >= 2) {
                    val fx = focusX(e); val fy = focusY(e)
                    Native.vpPan((fx - lastSpanX).toDouble(), (fy - lastSpanY).toDouble())
                    lastSpanX = fx; lastSpanY = fy
                    requestRender()
                } else if (!multi) {
                    val moved = hypot(e.x - downX, e.y - downY) > touchSlop
                    if (!dragging && !orbiting && moved) {
                        dragging = Native.vpDragStart(downX.toDouble(), downY.toDouble())
                        orbiting = !dragging
                    }
                    if (dragging) Native.vpDragMove(e.x.toDouble(), e.y.toDouble())
                    else if (orbiting) Native.vpOrbit((e.x - lastX).toDouble(), (e.y - lastY).toDouble())
                    lastX = e.x; lastY = e.y
                    requestRender()
                }
            }
            MotionEvent.ACTION_POINTER_UP -> {
                // keep panning with the remaining fingers; a lone finger does nothing until lifted
                val idx = if (e.actionIndex == 0) 1 else 0
                lastSpanX = focusX(e, e.actionIndex); lastSpanY = focusY(e, e.actionIndex)
                lastX = e.getX(idx); lastY = e.getY(idx)
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                if (dragging) finishDrag()
                dragging = false; orbiting = false; multi = false
            }
        }
        return true
    }

    private fun finishDrag() {
        val d = Native.vpDragEnd()
        dragging = false
        val arr = runCatching { JSONArray(d) }.getOrNull()
        if (arr != null) { if (abs(arr.optDouble(0)) + abs(arr.optDouble(1)) > 1e-6) listener.onMoved(d) }
        else if (d.trim().startsWith("{")) listener.onMoved(d)
        requestRender()
    }

    private val touchSlop = android.view.ViewConfiguration.get(ctx).scaledTouchSlop.toFloat()

    private fun focusX(e: MotionEvent, skip: Int = -1): Float {
        var s = 0f; var n = 0
        for (i in 0 until e.pointerCount) if (i != skip) { s += e.getX(i); n++ }
        return if (n == 0) 0f else s / n
    }
    private fun focusY(e: MotionEvent, skip: Int = -1): Float {
        var s = 0f; var n = 0
        for (i in 0 until e.pointerCount) if (i != skip) { s += e.getY(i); n++ }
        return if (n == 0) 0f else s / n
    }
}
