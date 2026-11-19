package cz.nowass.dispenser

import android.content.Context
import android.content.pm.PackageManager
import androidx.test.core.app.ApplicationProvider
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotSame
import org.junit.Assert.assertSame
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import java.io.File
import java.util.concurrent.CyclicBarrier
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger

/**
 * Review of 4d7e3b2, point 2: the operation reservations of DeviceDispenserRepository are
 * instance-local, which is sufficient only because there is never more than one instance per device
 * in the app. This pins that invariant: one process; one AppContainer per process; one repository per
 * device inside it (PerKeyRegistry: created once, also under a concurrent first access, and reused,
 * e.g. across mode changes); no other production code constructs the repository.
 * (The device stack itself is not built here: it needs the native provider, which must not be loaded
 * inside Robolectric's sandbox class loader — the plain JVM tests load it.)
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35])
class DeviceRepositoryInstanceTest {
    private val context: Context = ApplicationProvider.getApplicationContext()
    private val src = File(checkNotNull(System.getProperty("dsp.source.dir")))

    @Test
    fun the_app_runs_in_one_process() {
        val info = context.packageManager.getPackageInfo(context.packageName,
            PackageManager.GET_ACTIVITIES or PackageManager.GET_SERVICES or PackageManager.GET_RECEIVERS or PackageManager.GET_PROVIDERS)
        assertEquals(context.packageName, context.applicationInfo.processName)
        val components = listOfNotNull(info.activities, info.services, info.receivers, info.providers).flatMap { it.toList() }
        for (c in components) assertEquals("${c.name} runs in another process", context.packageName, c.processName)
    }

    @Test
    fun one_container_per_process() {
        val app = context as DispenserApplication
        assertSame(app.container, app.container)
    }

    @Test
    fun the_registry_creates_one_value_per_key_and_reuses_it() {
        val created = AtomicInteger()
        val registry = PerKeyRegistry<String, Any> { created.incrementAndGet(); Any() }
        val first = registry.get("a0b1c2d3e4f5")
        assertSame("reused (e.g. DEVICE -> DEMO -> DEVICE)", first, registry.get("a0b1c2d3e4f5"))
        assertNotSame("another device gets its own", first, registry.get("ffffffffffff"))
        assertEquals(2, created.get())
    }

    @Test
    fun a_concurrent_first_access_creates_one_value() {
        repeat(20) {
            val created = AtomicInteger()
            val registry = PerKeyRegistry<String, Any> { Thread.sleep(2); created.incrementAndGet(); Any() }
            val threads = 8
            val barrier = CyclicBarrier(threads)
            val pool = Executors.newFixedThreadPool(threads)
            try {
                val got = (1..threads).map { pool.submit<Any> { barrier.await(); registry.get("a0b1c2d3e4f5") } }.map { it.get(30, TimeUnit.SECONDS) }
                assertEquals(1, got.distinctBy { System.identityHashCode(it) }.size)
                assertEquals(1, created.get())
            } finally {
                pool.shutdownNow()
            }
        }
    }

    @Test
    fun only_the_container_registry_constructs_the_repository() {
        val constructing = src.walk().filter { it.isFile && it.extension == "kt" }
            .filter { Regex("""\bDeviceDispenserRepository\(""").containsMatchIn(it.readText().replace("class DeviceDispenserRepository(", "")) }
            .map { it.relativeTo(src).path }.toList()
        assertEquals(listOf("cz/nowass/dispenser/AppContainer.kt"), constructing)
        val container = File(src, "cz/nowass/dispenser/AppContainer.kt").readText()
        // built only by buildDeviceUi, which is only reachable through the registry
        assertEquals(1, Regex("""\bDeviceDispenserRepository\(""").findAll(container).count())
        assertEquals(listOf("PerKeyRegistry(::buildDeviceUi)", "fun buildDeviceUi("),
            Regex("""PerKeyRegistry\(::buildDeviceUi\)|fun buildDeviceUi\(|buildDeviceUi\(""").findAll(container).map { it.value }.toList())
    }
}
