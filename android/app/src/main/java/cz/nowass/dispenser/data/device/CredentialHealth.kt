package cz.nowass.dispenser.data.device

import cz.nowass.dispenser.domain.DeviceAccess
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.withContext

/**
 * The credential health of one device (the recovery-required state, M1.10e.4a), re-evaluated on demand
 * (security review F3): after a successful enrollment or recovery and after "Forget device", so the UI
 * leaves or enters the recovery-required state without an application restart.
 */
class CredentialHealth(
    private val check: () -> DeviceAccess,
    private val io: CoroutineDispatcher = Dispatchers.IO,
) {
    private val value = MutableStateFlow(DeviceAccess.CHECKING)
    val state: StateFlow<DeviceAccess> = value.asStateFlow()

    suspend fun refresh(): DeviceAccess = withContext(io) { check() }.also { value.value = it }
}
