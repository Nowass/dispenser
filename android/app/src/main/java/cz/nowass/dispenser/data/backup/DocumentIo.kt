package cz.nowass.dispenser.data.backup

import android.content.ContentResolver
import androidx.core.net.toUri
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.ByteArrayOutputStream
import java.io.IOException

/** Reads/writes user-chosen documents (Storage Access Framework URIs as strings). */
interface DocumentIo {
    suspend fun write(uri: String, bytes: ByteArray)

    /** Returns at most [maxBytes] + 1 bytes, so callers can detect oversized files without reading them fully. */
    suspend fun read(uri: String, maxBytes: Int): ByteArray
}

/** SAF implementation: no storage permissions; access is granted per document by the system picker. */
class ContentResolverDocumentIo(private val resolver: ContentResolver) : DocumentIo {

    override suspend fun write(uri: String, bytes: ByteArray) = withContext(Dispatchers.IO) {
        val target = uri.toUri()
        // "wt" truncates an existing document; some providers only support "w".
        val stream = runCatching { resolver.openOutputStream(target, "wt") }.getOrNull()
            ?: resolver.openOutputStream(target, "w")
            ?: throw IOException("cannot open document for writing")
        stream.use { it.write(bytes); it.flush() }
    }

    override suspend fun read(uri: String, maxBytes: Int): ByteArray = withContext(Dispatchers.IO) {
        val input = resolver.openInputStream(uri.toUri()) ?: throw IOException("cannot open document")
        input.use { stream ->
            val out = ByteArrayOutputStream()
            val buffer = ByteArray(64 * 1024)
            while (out.size() <= maxBytes) {
                val n = stream.read(buffer)
                if (n < 0) break
                out.write(buffer, 0, n)
            }
            out.toByteArray()
        }
    }
}
