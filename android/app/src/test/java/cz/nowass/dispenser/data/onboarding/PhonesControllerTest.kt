package cz.nowass.dispenser.data.onboarding

import cz.nowass.dispenser.data.device.ConnectOutcome
import cz.nowass.dispenser.data.device.FakeDeviceRepository
import cz.nowass.dispenser.device.ops.OpResult
import cz.nowass.dispenser.device.ops.PhoneRecord
import cz.nowass.dispenser.ui.devices.PhonesViewModel
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.test.UnconfinedTestDispatcher
import kotlinx.coroutines.test.resetMain
import kotlinx.coroutines.test.runTest
import kotlinx.coroutines.test.setMain
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

/** The phone list never owns the session: neither leaving the screen nor clearing its ViewModel disconnects. */
@OptIn(ExperimentalCoroutinesApi::class)
class PhonesControllerTest {
    @Before fun setUp() = Dispatchers.setMain(UnconfinedTestDispatcher())
    @After fun tearDown() = Dispatchers.resetMain()

    private fun repo() = FakeDeviceRepository().apply {
        connectOutcome = ConnectOutcome.Connected(generation = 1, unresolved = 0)
        phonesOutcome = OpResult.Ok(listOf(PhoneRecord(0, "SM-S911B", 0, 0, isSelf = true)))
    }

    @Test
    fun reopening_the_list_reuses_the_connection_and_never_disconnects() = runTest {
        val repo = repo()
        assertTrue(PhonesController(repo).load() is PhonesOutcome.Listed)
        assertTrue(PhonesController(repo).load() is PhonesOutcome.Listed)
        assertEquals(0, repo.disconnects)
    }

    @Test
    fun clearing_the_view_model_does_not_disconnect() = runTest {
        val repo = repo()
        val store = androidx.lifecycle.ViewModelStore()
        val vm = androidx.lifecycle.ViewModelProvider.create(store, PhonesViewModel.factory { PhonesController(repo) })[PhonesViewModel::class]
        assertEquals(listOf(0), vm.state.value.phones.map { it.slot })
        store.clear() // the screen leaves the back stack: onCleared runs
        assertEquals("no disconnect, graceful or not", 0, repo.disconnects)
    }
}
