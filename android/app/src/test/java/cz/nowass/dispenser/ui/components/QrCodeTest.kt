package cz.nowass.dispenser.ui.components

import com.google.zxing.BinaryBitmap
import com.google.zxing.RGBLuminanceSource
import com.google.zxing.common.HybridBinarizer
import com.google.zxing.qrcode.QRCodeReader
import com.google.zxing.qrcode.decoder.ErrorCorrectionLevel
import com.google.zxing.qrcode.decoder.Mode
import com.google.zxing.qrcode.encoder.Encoder
import org.junit.Assert.assertEquals
import org.junit.Test

/** Invitation QR codes (7.1: alphanumeric mode, error correction M) render and read back. */
class QrCodeTest {
    private val reference = "DSP2I1:06GB3GPKWKTJ08924CJ2A9H750MJMASC5MQ2YC1H68SK8D9P6WW3JEHV7GYKWFXTSM7TH1WYMKEH0K215Y9Y39YB324ZTTP0GVYPM73NTS8RP69ZW0F4AV7JEYV6188"

    @Test
    fun the_reference_invitation_uses_alphanumeric_mode_and_level_m() {
        val code = Encoder.encode(reference, ErrorCorrectionLevel.M)
        assertEquals(Mode.ALPHANUMERIC, code.mode)
        assertEquals(ErrorCorrectionLevel.M, code.ecLevel)
    }

    @Test
    fun a_rendered_matrix_reads_back_exactly() {
        val m = qrMatrix(reference)
        val scale = 4
        val w = m.width * scale
        val pixels = IntArray(w * w) { i -> if (m[(i % w) / scale, (i / w) / scale]) 0xFF000000.toInt() else 0xFFFFFFFF.toInt() }
        val text = QRCodeReader().decode(BinaryBitmap(HybridBinarizer(RGBLuminanceSource(w, w, pixels)))).text
        assertEquals(reference, text)
    }
}
