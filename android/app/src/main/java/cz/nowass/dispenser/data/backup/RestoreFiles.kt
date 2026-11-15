package cz.nowass.dispenser.data.backup

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.put
import java.io.File
import java.io.FileOutputStream
import java.nio.channels.FileChannel
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import java.nio.file.StandardOpenOption
import java.time.Instant

/**
 * Internal pre-restore safety snapshot: exact copies of all stored rows and the raw settings
 * value, so a rollback restores the previous state including unreadable records (R1, R4).
 */
data class SafetySnapshot(
    val id: String,
    val createdAt: Instant,
    val schedules: RawScheduleRows,
    val simulation: RawSimulationRows,
    val displayNameRaw: String?,
)

/** Internal files of the restore protocol (snapshot + journal) in app-private storage. */
object SafetySnapshotCodec {
    private const val SNAPSHOT_FORMAT = "dispenser-safety-snapshot"
    private const val JOURNAL_FORMAT = "dispenser-restore-journal"
    private const val VERSION = 1

    fun encode(s: SafetySnapshot): ByteArray = Json.encodeToString(
        JsonElement.serializer(),
        buildJsonObject {
            put("format", SNAPSHOT_FORMAT)
            put("version", VERSION)
            put("id", s.id)
            put("createdAt", s.createdAt.toString())
            put("schedules", JsonArray(s.schedules.rows))
            put("simulationState", JsonArray(s.simulation.state))
            put("simulationProcessed", JsonArray(s.simulation.processed))
            put("simulationEvents", JsonArray(s.simulation.events))
            put("displayNameRaw", s.displayNameRaw)
        },
    ).toByteArray(Charsets.UTF_8)

    /** Throws on any malformed or foreign content. */
    fun decode(bytes: ByteArray): SafetySnapshot {
        val o = Json.parseToJsonElement(bytes.toString(Charsets.UTF_8)).jsonObject
        require(o["format"]?.jsonPrimitive?.content == SNAPSHOT_FORMAT) { "not a safety snapshot" }
        require(o["version"]?.jsonPrimitive?.content == VERSION.toString()) { "unsupported snapshot version" }
        fun rows(k: String) = requireNotNull(o[k]) { "missing $k" }.jsonArray.map { it.jsonObject }
        val name = requireNotNull(o["displayNameRaw"]) { "missing displayNameRaw" }
        return SafetySnapshot(
            id = requireNotNull(o["id"]).jsonPrimitive.content,
            createdAt = Instant.parse(requireNotNull(o["createdAt"]).jsonPrimitive.content),
            schedules = RawScheduleRows(rows("schedules")),
            simulation = RawSimulationRows(rows("simulationState"), rows("simulationProcessed"), rows("simulationEvents")),
            displayNameRaw = if (name is JsonNull) null else (name as JsonPrimitive).also { require(it.isString) }.content,
        )
    }

    fun encodeJournal(snapshotId: String): ByteArray = Json.encodeToString(
        JsonElement.serializer(),
        buildJsonObject {
            put("format", JOURNAL_FORMAT)
            put("version", VERSION)
            put("snapshotId", snapshotId)
        },
    ).toByteArray(Charsets.UTF_8)

    /** Snapshot id referenced by the journal; throws if malformed. */
    fun decodeJournal(bytes: ByteArray): String {
        val o: JsonObject = Json.parseToJsonElement(bytes.toString(Charsets.UTF_8)).jsonObject
        require(o["format"]?.jsonPrimitive?.content == JOURNAL_FORMAT) { "not a restore journal" }
        return requireNotNull(o["snapshotId"]).jsonPrimitive.content
    }
}

/**
 * Durable storage for the restore protocol. Writes are atomic (temp file + fsync + rename),
 * so a reader sees either the complete previous file or the complete new one.
 */
interface RestoreFiles {
    /** Cheap check, safe on the main thread at startup. */
    fun journalExists(): Boolean
    suspend fun writeSnapshot(bytes: ByteArray)
    suspend fun readSnapshot(): ByteArray?
    suspend fun writeJournal(bytes: ByteArray)
    suspend fun readJournal(): ByteArray?
    suspend fun deleteJournal()
}

/** [RestoreFiles] in an app-private directory (`files/backup/`). */
class DirectoryRestoreFiles(private val dir: File) : RestoreFiles {
    private val snapshot get() = File(dir, SNAPSHOT_FILE)
    private val journal get() = File(dir, JOURNAL_FILE)

    override fun journalExists(): Boolean = journal.exists()

    override suspend fun writeSnapshot(bytes: ByteArray) = writeAtomically(snapshot, bytes)
    override suspend fun readSnapshot(): ByteArray? = read(snapshot)
    override suspend fun writeJournal(bytes: ByteArray) = writeAtomically(journal, bytes)
    override suspend fun readJournal(): ByteArray? = read(journal)

    override suspend fun deleteJournal() = withContext(Dispatchers.IO) {
        Files.deleteIfExists(journal.toPath())
        syncDirectory()
    }

    private suspend fun read(file: File): ByteArray? = withContext(Dispatchers.IO) { if (file.exists()) file.readBytes() else null }

    private suspend fun writeAtomically(target: File, bytes: ByteArray) = withContext(Dispatchers.IO) {
        if (!dir.isDirectory && !dir.mkdirs()) throw java.io.IOException("cannot create backup directory")
        val tmp = File(dir, target.name + ".tmp")
        FileOutputStream(tmp).use { out ->
            out.write(bytes)
            out.flush()
            out.fd.sync() // content durable before it becomes visible under the final name
        }
        Files.move(tmp.toPath(), target.toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING)
        syncDirectory()
    }

    /** Best effort: persists the rename/delete itself (not supported on every file system). */
    private fun syncDirectory() {
        runCatching { FileChannel.open(dir.toPath(), StandardOpenOption.READ).use { it.force(true) } }
    }

    companion object {
        const val DIRECTORY = "backup"
        const val SNAPSHOT_FILE = "pre-restore.json"
        const val JOURNAL_FILE = "restore.journal"
    }
}
