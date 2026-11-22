package cz.nowass.dispenser

import android.content.Context

/**
 * Per-build-type feature switches (app/build.gradle.kts resValues).
 * - [enabled]: the BLE device features (pairing, joining by invitation, recovery, phones, invitations,
 *   pump settings and readiness, automatic connection). Debug and release (daily-use app, 2026-11-22).
 * - [demoSwitch]: switching from an owned device back to DEMO (simulation): a developer tool, debug only.
 * The pump settings screen is the same in both builds; release replaces its BENCH texts (src/release/res).
 */
object DeviceFeatures {
    fun enabled(context: Context): Boolean = context.resources.getBoolean(R.bool.device_features_enabled)

    fun demoSwitch(context: Context): Boolean = context.resources.getBoolean(R.bool.demo_switch_enabled)
}
