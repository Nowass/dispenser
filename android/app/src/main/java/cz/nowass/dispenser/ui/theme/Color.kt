package cz.nowass.dispenser.ui.theme

import androidx.compose.ui.graphics.Color

/*
 * Brand tokens (docs/TASK_A1_1_1_UI.md). Use these ONLY to build color schemes
 * in Theme.kt; screens must read MaterialTheme.colorScheme / DispenserTheme.extendedColors.
 */

// Dark palette — deep navy/charcoal surfaces, aqua accent.
internal val Navy950 = Color(0xFF111820) // background
internal val Navy900 = Color(0xFF17212B) // surfaceContainerLow / nav bar
internal val Navy850 = Color(0xFF202B36) // surface / cards
internal val Navy800 = Color(0xFF283541) // surfaceContainerHigh
internal val Navy700 = Color(0xFF34434F) // outlineVariant / track
internal val Teal900 = Color(0xFF1C3038) // raised surface (hero card)

internal val Aqua400 = Color(0xFF55D6C0) // primary
internal val Aqua900 = Color(0xFF003730) // onPrimary
internal val Aqua800 = Color(0xFF1F4D47) // primaryContainer
internal val Aqua100 = Color(0xFFB4F2E6) // onPrimaryContainer

internal val Mist50 = Color(0xFFF1F7F9)  // onSurface
internal val Mist300 = Color(0xFFA9BDC7) // muted text (onSurfaceVariant)
internal val Mist500 = Color(0xFF6F8591) // outline

internal val Amber300 = Color(0xFFF1B86A) // warning
internal val Amber950 = Color(0xFF3A2606) // onWarning
internal val Amber900 = Color(0xFF4A3412) // warningContainer

internal val Rose300 = Color(0xFFEF7D86)  // critical / error
internal val Rose950 = Color(0xFF41090F)  // onError
internal val Rose900 = Color(0xFF5A1E25)  // errorContainer
internal val Rose100 = Color(0xFFFFDADC)  // onErrorContainer

internal val Lime300 = Color(0xFFB7DE78)  // battery / healthy
internal val Lime950 = Color(0xFF223000)  // onBattery

// Light palette — placeholder for a future user-selectable light mode (not exposed yet).
internal val LightBackground = Color(0xFFF5FAFB)
internal val LightSurface = Color(0xFFFFFFFF)
internal val LightPrimary = Color(0xFF006B5E)
internal val LightOnSurface = Color(0xFF111820)
internal val LightMuted = Color(0xFF4A5C66)
internal val LightWarning = Color(0xFF8A5A00)
internal val LightCritical = Color(0xFFB3261E)
internal val LightBattery = Color(0xFF4C6B1A)
