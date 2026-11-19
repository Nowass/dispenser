package cz.nowass.dispenser.scanner

import com.google.zxing.BarcodeFormat
import com.google.zxing.EncodeHintType
import com.google.zxing.MultiFormatWriter
import com.google.zxing.common.BitMatrix
import com.google.zxing.qrcode.decoder.ErrorCorrectionLevel
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test
import kotlin.random.Random

/** ZXing decoding of camera-like luminance frames, offline and QR only (M1.10e.4c). */
class QrFrameDecoderTest {
    private val label = "DSP2:a0b1c2d3e4f5:6RS3EX1BQCN2T3DHTV7ZNQS5M8RG"
    private val invitation = "DSP2I1:06GB3GPKWKTJ08924CJ2A9H750MJMASC5MQ2YC1H68SK8D9P6WW3JEHV7GYKWFXTSM7TH1WYMKEH0K215Y9Y39YB324ZTTP0GVYPM73NTS8RP69ZW0F4AV7JEYV6188"

    /** Renders [m] into a grey frame: [scale] px per module, offset, rowStride padding and noise. */
    private fun frame(m: BitMatrix, scale: Int, width: Int, height: Int, stride: Int, noise: Int = 0): ByteArray {
        val out = ByteArray(stride * height) { 200.toByte() }
        val rnd = Random(7)
        val ox = (width - m.width * scale) / 2
        val oy = (height - m.height * scale) / 2
        for (y in 0 until height) for (x in 0 until width) {
            val mx = (x - ox) / scale
            val my = (y - oy) / scale
            val dark = x >= ox && y >= oy && mx < m.width && my < m.height && m[mx, my]
            val base = if (dark) 30 else 220
            out[y * stride + x] = (base + if (noise > 0) rnd.nextInt(-noise, noise) else 0).coerceIn(0, 255).toByte()
        }
        return out
    }

    private fun qr(text: String): BitMatrix =
        MultiFormatWriter().encode(text, BarcodeFormat.QR_CODE, 0, 0, mapOf(EncodeHintType.ERROR_CORRECTION to ErrorCorrectionLevel.M, EncodeHintType.MARGIN to 4))

    @Test
    fun decodes_a_label_and_an_invitation_from_a_padded_noisy_frame() {
        val d = QrFrameDecoder()
        assertEquals(label, d.decode(frame(qr(label), 6, 640, 480, 704, noise = 25), 640, 480, 704))
        assertEquals(invitation, d.decode(frame(qr(invitation), 5, 640, 480, 640, noise = 25), 640, 480, 640))
    }

    @Test
    fun only_qr_codes_are_decoded() {
        val code128 = MultiFormatWriter().encode(label, BarcodeFormat.CODE_128, 600, 120)
        val dataMatrix = MultiFormatWriter().encode(label, BarcodeFormat.DATA_MATRIX, 0, 0)
        val d = QrFrameDecoder()
        assertNull(d.decode(frame(code128, 1, 640, 480, 640), 640, 480, 640))
        assertNull(d.decode(frame(dataMatrix, 8, 640, 480, 640), 640, 480, 640))
    }

    @Test
    fun frames_without_a_code_and_malformed_input_return_null() {
        val d = QrFrameDecoder()
        assertNull(d.decode(ByteArray(640 * 480) { 128.toByte() }, 640, 480))
        assertNull(d.decode(ByteArray(10), 640, 480))
        assertNull(d.decode(ByteArray(0), 0, 0))
        assertNull("row stride below width", d.decode(ByteArray(640 * 480), 640, 480, 600))
    }
}
