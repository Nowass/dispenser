package cz.nowass.dispenser

import android.content.Context

/**
 * Whether this build offers the BLE device features (pairing, joining by invitation, recovery, phones,
 * invitations): set per build type in app/build.gradle.kts (`device_features_enabled`). Debug: on.
 * Release: off until hardware acceptance (v0.5.0-beta.2 decision A1); Settings then keeps the disabled
 * "coming later" placeholder and nothing ever asks for a Bluetooth or camera permission.
 */
object DeviceFeatures {
    fun enabled(context: Context): Boolean = context.resources.getBoolean(R.bool.device_features_enabled)

    /** BENCH ONLY tools (pump configuration/readiness for the GPIO bring-up): debug builds only, never release. */
    fun benchTools(context: Context): Boolean = context.resources.getBoolean(R.bool.bench_tools_enabled)
}
