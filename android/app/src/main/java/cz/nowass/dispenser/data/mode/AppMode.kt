package cz.nowass.dispenser.data.mode

import androidx.datastore.core.DataStore
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.stringPreferencesKey
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext

/** Application mode (decision E1, docs/ANDROID_BLE_DESIGN.md 2.3). DEMO is the default of a fresh install. */
enum class AppMode { DEMO, DEVICE }

/**
 * The persisted selection: the mode the user chose and the selected device (single-device UI). The
 * device ID is kept while in DEMO, so switching back needs no new enrollment.
 */
data class StoredMode(val mode: AppMode, val deviceId: String?) {
    companion object {
        val DEFAULT = StoredMode(AppMode.DEMO, null)
    }
}

/**
 * Persistence of the mode selection: its own DataStore file, never part of the JSON backup or of a
 * restore (docs/ANDROID_BLE_DESIGN.md 11.2), excluded from system backup like every app file.
 */
interface AppModeStore {
    val stored: Flow<StoredMode>
    suspend fun write(mode: StoredMode)
}

class DataStoreAppModeStore(private val dataStore: DataStore<Preferences>) : AppModeStore {
    override val stored: Flow<StoredMode> = dataStore.data.map { p ->
        StoredMode(
            mode = when (p[KEY_MODE]) {
                AppMode.DEVICE.name -> AppMode.DEVICE
                else -> AppMode.DEMO // absent or unknown value: the safe default
            },
            deviceId = p[KEY_DEVICE]?.takeIf { DEVICE_ID.matches(it) },
        )
    }.distinctUntilChanged()

    override suspend fun write(mode: StoredMode) {
        dataStore.edit { p ->
            p[KEY_MODE] = mode.mode.name
            if (mode.deviceId == null) p.remove(KEY_DEVICE) else p[KEY_DEVICE] = mode.deviceId
        }
    }

    companion object {
        const val FILE_NAME = "app_mode"
        private val KEY_MODE = stringPreferencesKey("mode")
        private val KEY_DEVICE = stringPreferencesKey("device_id")
        private val DEVICE_ID = Regex("[0-9a-f]{12}")
    }
}

class InMemoryAppModeStore(initial: StoredMode = StoredMode.DEFAULT) : AppModeStore {
    private val state = MutableStateFlow(initial)
    override val stored: Flow<StoredMode> = state.asStateFlow()
    override suspend fun write(mode: StoredMode) { state.value = mode }
}

/** Whether a CONFIRMED enrollment record exists (a staged, unconfirmed enrollment does not count). */
fun interface ConfirmedEnrollments {
    fun has(deviceId: String): Boolean
}

/** The mode the app actually shows. */
sealed interface ResolvedMode {
    data object Demo : ResolvedMode
    data class Device(val deviceId: String) : ResolvedMode
}

/**
 * Pure resolution rule (M1.10e.4a, review of 4a):
 * - DEVICE can only be SELECTED with a confirmed enrollment ([AppModeController.enterDevice]); a staged
 *   enrollment whose result was never confirmed is not an enrollment.
 * - Once selected, DEVICE is restored after every restart — also while disconnected, and also when the
 *   credential has since become unusable (key missing, record unreadable, record gone). DEMO is never
 *   substituted silently, because that would hide the device's cached values and unresolved
 *   operations; the credential state is shown instead (recovery required, operations disabled).
 * - Resolution never rewrites the stored selection and never deletes anything.
 */
object AppModeResolver {
    fun resolve(stored: StoredMode): ResolvedMode {
        val id = stored.deviceId
        return if (stored.mode == AppMode.DEVICE && id != null) ResolvedMode.Device(id) else ResolvedMode.Demo
    }
}

/** Why a switch to DEVICE mode was refused. */
enum class ModeSwitchRefusal { NO_CONFIRMED_ENROLLMENT }

/**
 * Owns the effective mode. [mode] is null until the first resolution finished (the UI shows nothing
 * from either source until then). Switching never deletes credentials, staged enrollments, the device
 * cache or the pending-operations journal: those belong to the device association, not to the mode.
 */
class AppModeController(
    private val store: AppModeStore,
    private val enrollments: ConfirmedEnrollments,
    private val scope: CoroutineScope,
    private val io: CoroutineDispatcher = Dispatchers.IO,
) {
    private val state = MutableStateFlow<ResolvedMode?>(null)
    val mode: StateFlow<ResolvedMode?> = state.asStateFlow()
    private val lock = Mutex()

    /** Resolves once at startup (the persisted selection only; no Keystore access). */
    fun start() {
        scope.launch { lock.withLock { state.value = AppModeResolver.resolve(store.stored.first()) } }
    }

    /** Selects DEVICE mode for [deviceId]; refused unless a confirmed enrollment exists. */
    suspend fun enterDevice(deviceId: String): ModeSwitchRefusal? = lock.withLock {
        if (!withContext(io) { enrollments.has(deviceId) }) return@withLock ModeSwitchRefusal.NO_CONFIRMED_ENROLLMENT
        val selected = StoredMode(AppMode.DEVICE, deviceId)
        store.write(selected)
        state.value = AppModeResolver.resolve(selected)
        null
    }

    /** Selects DEMO mode; the device selection (and everything stored for it) is kept. */
    suspend fun enterDemo() = lock.withLock {
        val selected = store.stored.first().copy(mode = AppMode.DEMO)
        store.write(selected)
        state.value = ResolvedMode.Demo
    }
}
