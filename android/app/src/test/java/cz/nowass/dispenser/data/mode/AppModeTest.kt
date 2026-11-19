package cz.nowass.dispenser.data.mode

import androidx.datastore.preferences.core.PreferenceDataStoreFactory
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.stringPreferencesKey
import cz.nowass.dispenser.crypto.credentials.CredentialStore
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.filterNotNull
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File

/**
 * App mode persistence and resolution (M1.10e.4a, E1): DEVICE can be selected only with a CONFIRMED
 * enrollment (a staged one is not); once selected it is restored after every restart — also while
 * disconnected and also when the credential has since become unusable (shown as recovery required,
 * never silently replaced by DEMO); mode switches never delete credentials or other device data.
 */
class AppModeTest {
    @get:Rule val tmp = TemporaryFolder()
    private val id = "a0b1c2d3e4f5"
    private val scopes = mutableListOf<CoroutineScope>()

    @After fun tearDown() = scopes.forEach { it.cancel() }

    private fun scope() = CoroutineScope(SupervisorJob() + Dispatchers.IO).also { scopes += it }

    private val credDir by lazy { tmp.newFolder("credentials") }
    private val enrollments = ConfirmedEnrollments { CredentialStore.hasConfirmedRecord(credDir, it) }

    private fun confirmed(deviceId: String = id) = File(credDir, "$deviceId.cred").writeBytes(byteArrayOf(1, 2, 3))
    private fun staged(deviceId: String = id) = File(credDir, "$deviceId.pending").writeBytes(byteArrayOf(1, 2, 3))

    /** One "process": a controller over the persisted store, resolved once. */
    private fun process(store: AppModeStore): AppModeController =
        AppModeController(store, enrollments, scope(), Dispatchers.IO).also { it.start() }

    private fun AppModeController.resolved() = runBlocking { mode.filterNotNull().first() }

    @Test
    fun resolution_rules() {
        assertEquals(ResolvedMode.Demo, AppModeResolver.resolve(StoredMode.DEFAULT))
        assertEquals(ResolvedMode.Demo, AppModeResolver.resolve(StoredMode(AppMode.DEVICE, null)))
        // A selected device stays shown whatever its credential state (checked separately: recovery required).
        assertEquals(ResolvedMode.Device(id), AppModeResolver.resolve(StoredMode(AppMode.DEVICE, id)))
        assertEquals(ResolvedMode.Demo, AppModeResolver.resolve(StoredMode(AppMode.DEMO, id)))
    }

    @Test
    fun device_mode_survives_a_restart_and_switching_deletes_nothing() {
        val file = File(tmp.root, "app_mode.preferences_pb")
        val dataStoreScope = scope()
        val store = DataStoreAppModeStore(PreferenceDataStoreFactory.create(scope = dataStoreScope) { file })

        val first = process(store)
        assertEquals("fresh install", ResolvedMode.Demo, first.resolved())
        assertEquals(ModeSwitchRefusal.NO_CONFIRMED_ENROLLMENT, runBlocking { first.enterDevice(id) })
        staged()
        assertEquals("staged only", ModeSwitchRefusal.NO_CONFIRMED_ENROLLMENT, runBlocking { first.enterDevice(id) })
        assertEquals(ResolvedMode.Demo, first.mode.value)

        confirmed()
        assertNull(runBlocking { first.enterDevice(id) })
        assertEquals(ResolvedMode.Device(id), first.mode.value)

        // Restart: DEVICE is restored without any connection or session.
        assertEquals(ResolvedMode.Device(id), process(store).resolved())

        // Back to DEMO: the selection and every file of the association stay.
        runBlocking { first.enterDemo() }
        assertEquals(ResolvedMode.Demo, first.mode.value)
        assertEquals(StoredMode(AppMode.DEMO, id), runBlocking { store.stored.first() })
        assertTrue(File(credDir, "$id.cred").isFile)
        assertTrue(File(credDir, "$id.pending").isFile)
        assertEquals(ResolvedMode.Demo, process(store).resolved())

        // And back to DEVICE without a new enrollment.
        assertNull(runBlocking { first.enterDevice(id) })
        assertEquals(ResolvedMode.Device(id), process(store).resolved())

        // The confirmed record disappears: DEVICE stays shown (recovery required, unresolved operations
        // visible), never a silent switch to DEMO; the stored selection is untouched.
        File(credDir, "$id.cred").delete()
        assertEquals(ResolvedMode.Device(id), process(store).resolved())
        assertEquals(StoredMode(AppMode.DEVICE, id), runBlocking { store.stored.first() })
        // But it cannot be selected again without a confirmed enrollment.
        runBlocking { first.enterDemo() }
        assertEquals(ModeSwitchRefusal.NO_CONFIRMED_ENROLLMENT, runBlocking { first.enterDevice(id) })
    }

    @Test
    fun unknown_or_malformed_stored_values_fall_back_to_demo() = runBlocking {
        val dataStore = PreferenceDataStoreFactory.create(scope = scope()) { File(tmp.root, "m.preferences_pb") }
        val store = DataStoreAppModeStore(dataStore)
        dataStore.edit { it[stringPreferencesKey("mode")] = "SOMETHING"; it[stringPreferencesKey("device_id")] = "../../x" }
        assertEquals(StoredMode(AppMode.DEMO, null), store.stored.first())
        dataStore.edit { it[stringPreferencesKey("mode")] = "DEVICE" }
        assertEquals("an invalid device id is ignored", StoredMode(AppMode.DEVICE, null), store.stored.first())
        assertEquals(ResolvedMode.Demo, AppModeResolver.resolve(store.stored.first()))
    }
}
