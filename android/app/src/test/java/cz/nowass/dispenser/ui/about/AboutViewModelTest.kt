package cz.nowass.dispenser.ui.about

import cz.nowass.dispenser.data.AppVersion
import cz.nowass.dispenser.data.update.UpdateChecker
import cz.nowass.dispenser.domain.update.ReleaseChannel
import cz.nowass.dispenser.domain.update.SemanticVersion
import cz.nowass.dispenser.domain.update.UpdateCheckResult
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.test.UnconfinedTestDispatcher
import kotlinx.coroutines.test.resetMain
import kotlinx.coroutines.test.runTest
import kotlinx.coroutines.test.setMain
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Before
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class AboutViewModelTest {
    @Before fun setUp() = Dispatchers.setMain(UnconfinedTestDispatcher())
    @After fun tearDown() = Dispatchers.resetMain()

    @Test
    fun installedVersion_channelAndCheck_passCorrectInputs() = runTest {
        var seen: Pair<SemanticVersion, ReleaseChannel>? = null
        val checker = UpdateChecker { v, c -> seen = v to c; UpdateCheckResult.RepositoryUnavailable }
        val vm = AboutViewModel(checker, AppVersion("0.4.0", 4), sourceCodeUrl = null)
        assertEquals(ReleaseChannel.STABLE, vm.state.value.channel)
        assertNull(vm.state.value.sourceCodeUrl)
        assertEquals(UpdateUiState.Idle, vm.state.value.update)
        vm.checkForUpdates()
        assertEquals(SemanticVersion.parse("0.4.0")!! to ReleaseChannel.STABLE, seen)
        assertEquals(UpdateUiState.Done(UpdateCheckResult.RepositoryUnavailable), vm.state.value.update)
    }

    @Test
    fun betaBuild_usesBetaChannel() = runTest {
        var channel: ReleaseChannel? = null
        val vm = AboutViewModel({ _, c -> channel = c; UpdateCheckResult.NoReleases }, AppVersion("0.5.0-beta.1", 6), "https://github.com/Nowass/dispenser")
        vm.checkForUpdates()
        assertEquals(ReleaseChannel.BETA, channel)
        assertEquals("https://github.com/Nowass/dispenser", vm.state.value.sourceCodeUrl)
    }

    @Test
    fun nonSemanticVersion_isReported_withoutNetwork() = runTest {
        var called = false
        val vm = AboutViewModel({ _, _ -> called = true; UpdateCheckResult.NoReleases }, AppVersion("1.0", 1), null)
        assertNull(vm.state.value.channel)
        vm.checkForUpdates()
        assertEquals(UpdateUiState.UnsupportedVersion, vm.state.value.update)
        assertEquals(false, called)
    }
}
