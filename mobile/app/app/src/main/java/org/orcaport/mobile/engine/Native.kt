package org.orcaport.mobile.engine

/** JNI entry points of liborcacore.so (mobile/core/src/Jni.cpp). Results are JSON: {"ok": ...} or {"error": "..."}. */
object Native {
    init { System.loadLibrary("orcacore") }

    @JvmStatic external fun init(host: Host, resources: String, data: String, language: String): String
    @JvmStatic external fun call(cmd: String, args: String): String
    @JvmStatic external fun tr(s: String, ctx: String?): String
    /** a stateless command (core: PureCommands), answered on this thread without the engine lock */
    @JvmStatic external fun pure(cmd: String, args: String): String

    // 3D view (mobile/core/src/Viewport.cpp): gl* on the GL thread, vp* on the UI thread
    @JvmStatic external fun glCreated()
    @JvmStatic external fun glResize(width: Int, height: Int)
    @JvmStatic external fun glDraw(): Boolean
    @JvmStatic external fun vpOrbit(dx: Double, dy: Double)
    @JvmStatic external fun vpPan(dx: Double, dy: Double)
    @JvmStatic external fun vpZoom(factor: Double, x: Double, y: Double)
    @JvmStatic external fun vpPick(x: Double, y: Double): String
    @JvmStatic external fun vpDragStart(x: Double, y: Double): Boolean
    @JvmStatic external fun vpDragMove(x: Double, y: Double)
    @JvmStatic external fun vpDragEnd(): String
    @JvmStatic external fun vpLayersPress(x: Double, y: Double): Boolean
    @JvmStatic external fun vpLayersMove(x: Double, y: Double)
    @JvmStatic external fun vpLayersRelease()
    @JvmStatic external fun vpView(name: String)
    @JvmStatic external fun vpZoomTo(what: String)
    @JvmStatic external fun vpPreview(on: Boolean, plate: Int)
    @JvmStatic external fun previewSet(args: String)
    @JvmStatic external fun vpOverlay(): String
    /** display density: the gizmo grabbers are sized in dp like Orca's FixedGrabberSize in pixels */
    @JvmStatic external fun vpUiScale(scale: Float)
    /** a hardware keyboard's Shift is held (the gizmos' snapping, wxGetKeyState(WXK_SHIFT)) */
    @JvmStatic external fun vpShift(down: Boolean)
    /** a tap with a painting gizmo: one dab of the brush or one fill; false when not on the object */
    @JvmStatic external fun vpComTap(x: Double, y: Double): Boolean
    @JvmStatic external fun vpNavTouch(x: Float, y: Float, action: Int): Boolean
    @JvmStatic external fun vpPaintTap(x: Double, y: Double): Boolean
}
