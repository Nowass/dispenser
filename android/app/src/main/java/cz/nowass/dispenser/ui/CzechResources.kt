package cz.nowass.dispenser.ui

import android.annotation.SuppressLint
import android.content.res.Configuration
import androidx.annotation.PluralsRes
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalContext

/**
 * Like `pluralStringResource`, but always selects the plural category with
 * Czech rules (one / few / many / other). The UI is Czech-only, while Android
 * would otherwise pick the category by the device locale (e.g. English
 * one/other, which yields "2 dávek").
 *
 * Lint AppBundleLocaleChanges is suppressed: Czech strings live in default
 * `values/` (no `values-cs`), which is always in the base split, so Play
 * language splits cannot strip them.
 */
@SuppressLint("AppBundleLocaleChanges")
@Composable
fun czechPluralStringResource(@PluralsRes id: Int, count: Int, vararg formatArgs: Any): String {
    val context = LocalContext.current
    val configuration = LocalConfiguration.current
    val czechResources = remember(context, configuration) {
        val czech = Configuration(configuration).apply { setLocale(Format.CZECH) }
        context.createConfigurationContext(czech).resources
    }
    return czechResources.getQuantityString(id, count, *formatArgs)
}
