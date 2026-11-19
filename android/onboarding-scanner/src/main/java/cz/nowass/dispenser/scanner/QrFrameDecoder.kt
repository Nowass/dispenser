package cz.nowass.dispenser.scanner

import com.google.zxing.BarcodeFormat
import com.google.zxing.BinaryBitmap
import com.google.zxing.DecodeHintType
import com.google.zxing.NotFoundException
import com.google.zxing.ChecksumException
import com.google.zxing.FormatException
import com.google.zxing.PlanarYUVLuminanceSource
import com.google.zxing.common.HybridBinarizer
import com.google.zxing.qrcode.QRCodeReader

/**
 * Decodes one camera frame's luminance (the Y plane) with ZXing core, QR codes only. Pure JVM logic,
 * fully offline (unit tested with generated codes). Returns null when the frame holds no readable QR
 * code; never throws for unreadable input.
 */
class QrFrameDecoder {
    private val reader = QRCodeReader()
    private val hints = mapOf(
        DecodeHintType.POSSIBLE_FORMATS to listOf(BarcodeFormat.QR_CODE),
        DecodeHintType.TRY_HARDER to true,
    )

    /**
     * [luminance] holds [height] rows of [rowStride] bytes, the first [width] of each row being pixels
     * (CameraX's Y plane, pixel stride 1).
     */
    fun decode(luminance: ByteArray, width: Int, height: Int, rowStride: Int = width): String? {
        if (width <= 0 || height <= 0 || rowStride < width || luminance.size < rowStride * (height - 1) + width) return null
        val source = PlanarYUVLuminanceSource(luminance, rowStride, height, 0, 0, width, height, false)
        return try {
            reader.decode(BinaryBitmap(HybridBinarizer(source)), hints).text
        } catch (e: NotFoundException) {
            null
        } catch (e: ChecksumException) {
            null
        } catch (e: FormatException) {
            null
        } finally {
            reader.reset()
        }
    }
}
