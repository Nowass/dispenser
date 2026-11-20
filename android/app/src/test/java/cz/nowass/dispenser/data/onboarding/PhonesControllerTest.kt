package cz.nowass.dispenser.data.onboarding

import cz.nowass.dispenser.data.device.ConnectOutcome
import cz.nowass.dispenser.data.device.FakeDeviceRepository
import cz.nowass.dispenser.device.ops.OpResult
import cz.nowass.dispenser.device.ops.PhoneRecord
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/** Leaving the phone list is not an explicit "Done": it must never end the device's service window. */
class PhonesControllerTest {
    @Test
    fun leaving_the_screen_disconnects_without_end_session_and_the_list_can_be_loaded_again() = runBlocking {
        val repo = FakeDeviceRepository()
        repo.connectOutcome = ConnectOutcome.Connected(generation = 1, unresolved = 0)
        repo.phonesOutcome = OpResult.Ok(listOf(PhoneRecord(0, "SM-S911B", 0, 0, isSelf = true)))
        val phones = PhonesController(repo)
        assertTrue(phones.load() is PhonesOutcome.Listed)
        phones.finish()
        assertEquals("plain disconnect only (graceful = false: no END_SESSION)", listOf(false), repo.disconnectModes)
        repo.connectOutcome = ConnectOutcome.Connected(generation = 2, unresolved = 0)
        assertTrue(PhonesController(repo).load() is PhonesOutcome.Listed)
    }
}
