package cz.nowass.dispenser.ui.components

import android.content.ClipData
import android.content.ClipDescription
import android.content.ClipboardManager
import android.content.Context
import android.os.Build
import android.os.PersistableBundle

/**
 * Same-phone hand-over of an owner invitation between the DEBUG and the normal app (each has its own
 * Keystore identity, and the invitation QR cannot be scanned on the phone that shows it). The text
 * alone enrolls nothing: the device decides validity (10 min, single use), and joining still needs the
 * device's service window (button) and the setup code from its label.
 *
 * - copy: marked sensitive (API 33+ hides it from the clipboard preview; older keyboards may still keep
 *   a history entry — see KNOWN_ISSUES);
 * - take: reads only an invitation (`DSP2I1:` prefix) and then clears the clipboard; any other content
 *   is left untouched and nothing is returned.
 */
object SensitiveClipboard {
    const val INVITATION_PREFIX = "DSP2I1:"
    private const val EXTRA_IS_SENSITIVE_COMPAT = "android.content.extra.IS_SENSITIVE"

    fun copyInvitation(context: Context, label: String, text: String) {
        val cm = context.getSystemService(ClipboardManager::class.java) ?: return
        val clip = ClipData.newPlainText(label, text)
        clip.description.extras = PersistableBundle().apply {
            putBoolean(if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) ClipDescription.EXTRA_IS_SENSITIVE else EXTRA_IS_SENSITIVE_COMPAT, true)
        }
        cm.setPrimaryClip(clip)
    }

    /** The invitation on the clipboard (then cleared), or null when there is none. */
    fun takeInvitation(context: Context): String? {
        val cm = context.getSystemService(ClipboardManager::class.java) ?: return null
        val text = runCatching { cm.primaryClip?.takeIf { it.itemCount > 0 }?.getItemAt(0)?.coerceToText(context)?.toString() }
            .getOrNull()?.trim() ?: return null
        if (!text.startsWith(INVITATION_PREFIX)) return null
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) cm.clearPrimaryClip() else cm.setPrimaryClip(ClipData.newPlainText("", ""))
        return text
    }
}
