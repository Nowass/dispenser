package cz.nowass.dispenser.ui.theme

import androidx.compose.material3.ColorScheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.ReadOnlyComposable
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.graphics.Color

private val DarkColors: ColorScheme = darkColorScheme(
    primary = Aqua400,
    onPrimary = Aqua900,
    primaryContainer = Aqua800,
    onPrimaryContainer = Aqua100,
    secondary = Aqua400,
    onSecondary = Aqua900,
    tertiary = Lime300,
    onTertiary = Lime950,
    background = Navy950,
    onBackground = Mist50,
    surface = Navy850,
    onSurface = Mist50,
    surfaceVariant = Navy800,
    onSurfaceVariant = Mist300,
    surfaceContainerLowest = Navy950,
    surfaceContainerLow = Navy900,
    surfaceContainer = Navy850,
    surfaceContainerHigh = Navy800,
    surfaceContainerHighest = Navy700,
    surfaceTint = Color.Transparent,
    outline = Mist500,
    outlineVariant = Navy700,
    error = Rose300,
    onError = Rose950,
    errorContainer = Rose900,
    onErrorContainer = Rose100,
)

/** Not selectable yet; kept so a future light-mode setting only flips a flag. */
private val LightColors: ColorScheme = lightColorScheme(
    primary = LightPrimary,
    background = LightBackground,
    onBackground = LightOnSurface,
    surface = LightSurface,
    onSurface = LightOnSurface,
    onSurfaceVariant = LightMuted,
    tertiary = LightBattery,
    error = LightCritical,
)

/** Semantic roles Material 3 does not provide. */
@Immutable
data class ExtendedColors(
    /** Raised hero surface (e.g. next dose card). */
    val raisedSurface: Color,
    val warning: Color,
    val onWarning: Color,
    val warningContainer: Color,
    val battery: Color,
    val onBattery: Color,
)

private val DarkExtended = ExtendedColors(
    raisedSurface = Teal900,
    warning = Amber300,
    onWarning = Amber950,
    warningContainer = Amber900,
    battery = Lime300,
    onBattery = Lime950,
)

private val LightExtended = ExtendedColors(
    raisedSurface = Color(0xFFE3F4F1),
    warning = LightWarning,
    onWarning = Color.White,
    warningContainer = Color(0xFFFFE6BF),
    battery = LightBattery,
    onBattery = Color.White,
)

private val LocalExtendedColors = staticCompositionLocalOf { DarkExtended }

/**
 * App theme. Dark by default regardless of the system setting, and without
 * dynamic (wallpaper) color so brand colors stay intact (TASK_A1_1_1_UI.md).
 */
@Composable
fun DispenserTheme(
    darkTheme: Boolean = true,
    content: @Composable () -> Unit,
) {
    CompositionLocalProvider(LocalExtendedColors provides if (darkTheme) DarkExtended else LightExtended) {
        MaterialTheme(
            colorScheme = if (darkTheme) DarkColors else LightColors,
            typography = Typography,
            shapes = Shapes,
            content = content,
        )
    }
}

object DispenserTheme {
    val extendedColors: ExtendedColors
        @Composable @ReadOnlyComposable get() = LocalExtendedColors.current
}
