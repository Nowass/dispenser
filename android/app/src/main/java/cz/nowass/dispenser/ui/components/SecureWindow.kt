package cz.nowass.dispenser.ui.components

import android.app.Activity
import android.content.Context
import android.content.ContextWrapper
import android.view.WindowManager
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.ui.platform.LocalContext

private tailrec fun Context.findActivity(): Activity? = when (this) {
    is Activity -> this
    is ContextWrapper -> baseContext.findActivity()
    else -> null
}

/**
 * While composed, the window is FLAG_SECURE: no screenshots, screen recording or recents thumbnail
 * (docs/ANDROID_BLE_DESIGN.md 7.1). Used by every screen that shows or captures a secret: the
 * onboarding (setup code, scanned invitation, camera preview) and the owner's invitation QR code.
 */
@Composable
fun SecureWindow() {
    val activity = LocalContext.current.findActivity()
    DisposableEffect(activity) {
        val window = activity?.window
        window?.addFlags(WindowManager.LayoutParams.FLAG_SECURE)
        onDispose { window?.clearFlags(WindowManager.LayoutParams.FLAG_SECURE) }
    }
}
