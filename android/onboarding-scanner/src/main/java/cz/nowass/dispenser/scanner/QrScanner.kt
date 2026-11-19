package cz.nowass.dispenser.scanner

import androidx.camera.compose.CameraXViewfinder
import androidx.camera.core.CameraSelector
import androidx.camera.core.ImageAnalysis
import androidx.camera.core.ImageProxy
import androidx.camera.core.Preview
import androidx.camera.core.SurfaceRequest
import androidx.camera.lifecycle.ProcessCameraProvider
import androidx.compose.foundation.layout.Box
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.core.content.ContextCompat
import androidx.lifecycle.compose.LocalLifecycleOwner
import java.util.concurrent.Executors

/**
 * Camera preview with on-device QR decoding (ZXing core, offline, QR only). Calls [onScanned] at most
 * once per scanner session with the first QR text accepted by [acceptedPrefixes]. The camera permission
 * must already be granted (the host screen asks when the user opens the scanner).
 *
 * Resources: the use cases are bound to the host's lifecycle, so CameraX stops the camera when the app
 * goes to the background and closes it when the host is destroyed; leaving the composition unbinds
 * everything and stops the analysis thread at once. Frames are analysed in memory only; nothing is
 * stored or sent. The host screen is FLAG_SECURE.
 */
@Composable
fun QrScanner(onScanned: (String) -> Unit, modifier: Modifier = Modifier, acceptedPrefixes: List<String> = emptyList()) {
    val context = LocalContext.current
    val lifecycleOwner = LocalLifecycleOwner.current
    val gate = remember { QrResultGate(acceptedPrefixes) }
    val callback = rememberUpdatedState(onScanned)
    var surfaceRequest by remember { mutableStateOf<SurfaceRequest?>(null) }
    DisposableEffect(lifecycleOwner) {
        val executor = Executors.newSingleThreadExecutor()
        val decoder = QrFrameDecoder()
        val mainExecutor = ContextCompat.getMainExecutor(context)
        val providerFuture = ProcessCameraProvider.getInstance(context)
        var provider: ProcessCameraProvider? = null
        var disposed = false
        providerFuture.addListener({
            if (disposed) return@addListener
            val p = providerFuture.get()
            provider = p
            val preview = Preview.Builder().build().also { it.setSurfaceProvider { request -> surfaceRequest = request } }
            val analysis = ImageAnalysis.Builder()
                .setBackpressureStrategy(ImageAnalysis.STRATEGY_KEEP_ONLY_LATEST)
                .setOutputImageFormat(ImageAnalysis.OUTPUT_IMAGE_FORMAT_YUV_420_888)
                .build()
            analysis.setAnalyzer(executor) { proxy ->
                val text = proxy.use { decodeLuminance(decoder, it) }
                gate.offer(text)?.let { accepted -> mainExecutor.execute { if (!disposed) callback.value(accepted) } }
            }
            p.unbindAll()
            p.bindToLifecycle(lifecycleOwner, CameraSelector.DEFAULT_BACK_CAMERA, preview, analysis)
        }, mainExecutor)
        onDispose {
            disposed = true
            provider?.unbindAll()
            surfaceRequest = null
            executor.shutdownNow()
        }
    }
    Box(modifier) {
        surfaceRequest?.let { CameraXViewfinder(surfaceRequest = it) }
    }
}

/** The Y plane of a YUV_420_888 frame (pixel stride 1) handed to [decoder]. */
private fun decodeLuminance(decoder: QrFrameDecoder, proxy: ImageProxy): String? {
    val plane = proxy.planes.firstOrNull() ?: return null
    if (plane.pixelStride != 1) return null
    val buffer = plane.buffer.duplicate().also { it.rewind() }
    val bytes = ByteArray(buffer.remaining()).also { buffer.get(it) }
    return decoder.decode(bytes, proxy.width, proxy.height, plane.rowStride)
}
