package cz.nowass.dispenser.ui

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** Detail routes hide the shell header + dock, so only their own top bar is shown. */
class FullScreenRouteTest {
    @Test
    fun detailRoutes_areFullScreen_mainPagerIsNot() {
        listOf(
            BACKUP_ROUTE, RESERVOIR_ROUTE, ABOUT_ROUTE, ABOUT_CHANGELOG_ROUTE, ABOUT_LICENSES_ROUTE,
            "$ABOUT_LICENSE_TEXT_ROUTE?title={title}&path={path}", "$SCHEDULE_ROUTE?scheduleId={scheduleId}&date={date}",
            "$ONBOARDING_ROUTE?purpose={purpose}", INVITE_ROUTE, PHONES_ROUTE, FIRMWARE_ROUTE,
        ).forEach { assertTrue(it, isFullScreenRoute(it)) }
        assertFalse(isFullScreenRoute("$MAIN_ROUTE?tab={tab}&date={date}"))
        assertFalse(isFullScreenRoute(null))
    }
}
