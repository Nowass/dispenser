package cz.nowass.dispenser.ui.history

import cz.nowass.dispenser.data.MockSeed
import org.junit.Assert.assertEquals
import org.junit.Test
import java.time.LocalDate

class HistoryViewModelTest {
    @Test
    fun grouping_isNewestFirst_byDateThenTimeThenSeq() {
        val seed = MockSeed.default()
        val days = HistoryViewModel.group(seed.events, seed.device.zone)
        assertEquals(listOf(LocalDate.of(2026, 11, 13), LocalDate.of(2026, 11, 12), LocalDate.of(2026, 11, 2)), days.map { it.date })
        assertEquals(listOf(3L, 2L), days[1].events.map { it.seq }) // 19:30 before 08:00
        // Same instant: higher seq (later processed) first.
        val sameInstant = listOf(seed.events[1], seed.events[1].copy(id = "x", seq = 9))
        assertEquals(listOf(9L, 2L), HistoryViewModel.group(sameInstant, seed.device.zone).single().events.map { it.seq })
    }
}
