package org.orcaport.mobile

import android.database.Cursor
import android.database.MatrixCursor
import android.os.CancellationSignal
import android.os.ParcelFileDescriptor
import android.provider.DocumentsContract.Document
import android.provider.DocumentsContract.Root
import android.provider.DocumentsProvider
import android.webkit.MimeTypeMap
import java.io.File

/** OrcaSlicer's data folder (presets, logs, app config) as a root of the system file manager, the way the desktop's
 *  desktop_open_datadir_folder shows it in Explorer / Finder. Document ids: "root", then "root/<relative path>". */
class DataFolderProvider : DocumentsProvider() {
    private val base: File get() = File(context!!.filesDir, "OrcaSlicer")

    private fun file(id: String): File {
        val f = File(base, id.removePrefix("root").removePrefix("/")).canonicalFile
        require(f.path.startsWith(base.canonicalPath)) { "outside the data folder" }
        return f
    }
    private fun id(f: File) = f.canonicalFile.relativeTo(base.canonicalFile).path.let { if (it.isEmpty()) "root" else "root/$it" }

    override fun onCreate() = true

    override fun queryRoots(projection: Array<out String>?): Cursor = MatrixCursor(projection ?: ROOT).apply {
        newRow().add(Root.COLUMN_ROOT_ID, "orca").add(Root.COLUMN_DOCUMENT_ID, "root")
            .add(Root.COLUMN_TITLE, context!!.applicationInfo.loadLabel(context!!.packageManager).toString())
            .add(Root.COLUMN_SUMMARY, "Configuration folder")
            .add(Root.COLUMN_FLAGS, Root.FLAG_SUPPORTS_CREATE or Root.FLAG_SUPPORTS_IS_CHILD)
            .add(Root.COLUMN_ICON, R.mipmap.ic_launcher)
    }

    private fun row(c: MatrixCursor, f: File) {
        val dir = f.isDirectory
        val mime = if (dir) Document.MIME_TYPE_DIR
            else MimeTypeMap.getSingleton().getMimeTypeFromExtension(f.extension.lowercase()) ?: when (f.extension.lowercase()) {
                "json", "ini", "conf", "log", "cereal", "0", "1", "2" -> "text/plain"; else -> "application/octet-stream" }
        var flags = Document.FLAG_SUPPORTS_DELETE or Document.FLAG_SUPPORTS_RENAME
        flags = flags or if (dir) Document.FLAG_DIR_SUPPORTS_CREATE else Document.FLAG_SUPPORTS_WRITE
        c.newRow().add(Document.COLUMN_DOCUMENT_ID, id(f)).add(Document.COLUMN_DISPLAY_NAME, if (f == base) "OrcaSlicer" else f.name)
            .add(Document.COLUMN_MIME_TYPE, mime).add(Document.COLUMN_SIZE, if (dir) null else f.length())
            .add(Document.COLUMN_LAST_MODIFIED, f.lastModified()).add(Document.COLUMN_FLAGS, flags)
    }

    override fun queryDocument(documentId: String, projection: Array<out String>?): Cursor =
        MatrixCursor(projection ?: DOC).apply { row(this, file(documentId)) }

    override fun queryChildDocuments(parentDocumentId: String, projection: Array<out String>?, sortOrder: String?): Cursor =
        MatrixCursor(projection ?: DOC).apply { file(parentDocumentId).listFiles()?.sortedBy { it.name }?.forEach { row(this, it) } }

    override fun openDocument(documentId: String, mode: String, signal: CancellationSignal?): ParcelFileDescriptor =
        ParcelFileDescriptor.open(file(documentId), ParcelFileDescriptor.parseMode(mode))

    override fun isChildDocument(parentDocumentId: String, documentId: String) =
        file(documentId).path.let { c -> val p = file(parentDocumentId).path; c == p || c.startsWith("$p/") }

    override fun createDocument(parentDocumentId: String, mimeType: String, displayName: String): String {
        var f = File(file(parentDocumentId), displayName)
        var n = 1
        while (f.exists()) f = File(file(parentDocumentId), "${displayName.substringBeforeLast('.')} (${n++})" +
            (if (displayName.contains('.')) "." + displayName.substringAfterLast('.') else ""))
        if (mimeType == Document.MIME_TYPE_DIR) f.mkdirs() else f.createNewFile()
        return id(f)
    }

    override fun deleteDocument(documentId: String) { file(documentId).deleteRecursively() }

    override fun renameDocument(documentId: String, displayName: String): String {
        val f = file(documentId)
        val to = File(f.parentFile, displayName)
        f.renameTo(to)
        return id(to)
    }

    companion object {
        private val ROOT = arrayOf(Root.COLUMN_ROOT_ID, Root.COLUMN_DOCUMENT_ID, Root.COLUMN_TITLE, Root.COLUMN_SUMMARY, Root.COLUMN_FLAGS, Root.COLUMN_ICON)
        private val DOC = arrayOf(Document.COLUMN_DOCUMENT_ID, Document.COLUMN_DISPLAY_NAME, Document.COLUMN_MIME_TYPE, Document.COLUMN_SIZE,
            Document.COLUMN_LAST_MODIFIED, Document.COLUMN_FLAGS)
    }
}
