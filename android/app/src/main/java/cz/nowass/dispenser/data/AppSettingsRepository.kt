package cz.nowass.dispenser.data

import androidx.datastore.core.DataStore
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.stringPreferencesKey
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.map

/**
 * Lightweight user-facing app settings (A1.4a). Holds only presentation preferences;
 * never schedules, simulation or device data (those live in their own stores).
 */
interface AppSettingsRepository {
    /** Custom aquarium/device display name, or null to use the default ("Moje akvárium"). */
    val displayName: Flow<String?>

    /** Stores a normalized name; null or blank restores the default. */
    suspend fun setDisplayName(name: String?)

    /** Stored display-name value exactly as persisted (safety snapshot before a restore). */
    suspend fun captureRaw(): String?

    /** Writes back a value taken by [captureRaw], unchanged (rollback). */
    suspend fun restoreRaw(value: String?)
}

/** Validation and normalization of the display name. */
object DisplayNameRules {
    const val MAX_LENGTH = 30

    enum class Error { TOO_LONG }

    /** Trims, removes control characters and collapses whitespace; blank -> null (default). */
    fun normalize(input: String?): String? =
        input
            ?.filterNot { it.isISOControl() }
            ?.trim()
            ?.replace(Regex("\\s+"), " ")
            ?.takeIf { it.isNotEmpty() }

    fun validate(input: String?): Error? =
        if ((normalize(input)?.length ?: 0) > MAX_LENGTH) Error.TOO_LONG else null
}

/** DataStore Preferences implementation (file `app_settings.preferences_pb`). */
class DataStoreAppSettingsRepository(private val dataStore: DataStore<Preferences>) : AppSettingsRepository {

    override val displayName: Flow<String?> =
        dataStore.data.map { DisplayNameRules.normalize(it[KEY_DISPLAY_NAME]) }.distinctUntilChanged()

    override suspend fun setDisplayName(name: String?) {
        require(DisplayNameRules.validate(name) == null) { "display name too long" }
        val normalized = DisplayNameRules.normalize(name)
        dataStore.edit { prefs ->
            if (normalized == null) prefs.remove(KEY_DISPLAY_NAME) else prefs[KEY_DISPLAY_NAME] = normalized
        }
    }

    override suspend fun captureRaw(): String? = dataStore.data.first()[KEY_DISPLAY_NAME]

    override suspend fun restoreRaw(value: String?) {
        dataStore.edit { prefs -> if (value == null) prefs.remove(KEY_DISPLAY_NAME) else prefs[KEY_DISPLAY_NAME] = value }
    }

    companion object {
        const val FILE_NAME = "app_settings"
        private val KEY_DISPLAY_NAME = stringPreferencesKey("display_name")
    }
}

/** For tests and previews. */
class InMemoryAppSettingsRepository(initialName: String? = null) : AppSettingsRepository {
    private val name = MutableStateFlow(DisplayNameRules.normalize(initialName))
    override val displayName: Flow<String?> = name.asStateFlow()
    override suspend fun setDisplayName(name: String?) {
        require(DisplayNameRules.validate(name) == null) { "display name too long" }
        this.name.value = DisplayNameRules.normalize(name)
    }
    override suspend fun captureRaw(): String? = name.value
    override suspend fun restoreRaw(value: String?) { name.value = value }
}

/** Installed app version, shown in Settings. */
data class AppVersion(val name: String, val code: Long)
