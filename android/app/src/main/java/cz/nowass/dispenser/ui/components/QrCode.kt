package cz.nowass.dispenser.ui.components

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.padding
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import com.google.zxing.BarcodeFormat
import com.google.zxing.EncodeHintType
import com.google.zxing.common.BitMatrix
import com.google.zxing.qrcode.QRCodeWriter
import com.google.zxing.qrcode.decoder.ErrorCorrectionLevel

/**
 * QR matrix for [text] with ZXing core (docs/ANDROID_BLE_DESIGN.md 7.1: error correction level M; the
 * invitation's upper-case Crockford text fits the alphanumeric mode). Includes the 4-module quiet zone.
 */
fun qrMatrix(text: String): BitMatrix =
    QRCodeWriter().encode(text, BarcodeFormat.QR_CODE, 0, 0, mapOf(EncodeHintType.ERROR_CORRECTION to ErrorCorrectionLevel.M, EncodeHintType.MARGIN to 4))

/** Dark modules on white (also in the app's dark theme, so any scanner reads it). */
@Composable
fun QrCodeImage(text: String, contentDescription: String, modifier: Modifier = Modifier) {
    val matrix = remember(text) { qrMatrix(text) }
    Canvas(
        modifier
            .aspectRatio(1f)
            .background(Color.White)
            .padding(4.dp)
            .semantics { this.contentDescription = contentDescription },
    ) {
        val cell = size.width / matrix.width
        for (y in 0 until matrix.height) for (x in 0 until matrix.width) {
            if (matrix[x, y]) drawRect(Color.Black, topLeft = Offset(x * cell, y * cell), size = Size(cell + 0.5f, cell + 0.5f))
        }
    }
}
