package cz.nowass.dispenser.device.session

import android.app.Activity
import android.app.Application
import android.os.Bundle
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

/**
 * Background grace of the application-wide session (docs/ANDROID_BLE_DESIGN.md 4.4, E9). The live
 * session belongs to the process ([DeviceSessionManager]), never to a screen: navigation keeps it.
 * When the last activity stops, a [graceMs] timer starts; an activity starting again before it expires
 * cancels it and the session continues unchanged. On expiry [onExpired] runs once, which must be a
 * plain disconnect (no END_SESSION: the device's service window continues under its own timers).
 *
 * Every transition takes a new token under the lock and an expiry acts only if its token is still the
 * current one and the app is still in the background, so quick foreground/background toggles can never
 * fire a stale timer. Not a keepalive: nothing is sent while waiting. No foreground service (v1, E9).
 */
class BackgroundGrace(
    private val scope: CoroutineScope,
    private val graceMs: Long = DEFAULT_GRACE_MS,
    private val onExpired: suspend () -> Unit,
) {
    private var started = 0
    private var token = 0L
    private var job: Job? = null
    private var expiredToken = -1L

    /** True while no activity is started (the grace timer runs or has expired). */
    val inBackground: Boolean @Synchronized get() = started == 0 && token > 0

    /** True after the grace expired and until an activity starts again. */
    val expired: Boolean @Synchronized get() = started == 0 && expiredToken == token

    @Synchronized
    fun activityStarted() {
        if (started++ == 0) {
            token++
            job?.cancel()
            job = null
        }
    }

    @Synchronized
    fun activityStopped() {
        if (started == 0) return // unbalanced callback: ignore
        if (--started == 0) {
            val mine = ++token
            job?.cancel()
            job = scope.launch {
                delay(graceMs)
                if (markExpired(mine)) onExpired()
            }
        }
    }

    @Synchronized
    private fun markExpired(t: Long): Boolean {
        if (started != 0 || token != t) return false // a newer transition superseded this timer
        expiredToken = t
        return true
    }

    /** Forwards the process's activity start/stop callbacks (all activities, one counter). */
    val callbacks: Application.ActivityLifecycleCallbacks = object : Application.ActivityLifecycleCallbacks {
        override fun onActivityStarted(activity: Activity) = activityStarted()
        override fun onActivityStopped(activity: Activity) = activityStopped()
        override fun onActivityCreated(activity: Activity, savedInstanceState: Bundle?) = Unit
        override fun onActivityResumed(activity: Activity) = Unit
        override fun onActivityPaused(activity: Activity) = Unit
        override fun onActivitySaveInstanceState(activity: Activity, outState: Bundle) = Unit
        override fun onActivityDestroyed(activity: Activity) = Unit
    }

    companion object {
        /**
         * 30 s: long enough for a short app switch (reading a code, answering a message), short
         * enough that a backgrounded phone does not hold the device's only connection for long.
         */
        const val DEFAULT_GRACE_MS = 30_000L
    }
}
