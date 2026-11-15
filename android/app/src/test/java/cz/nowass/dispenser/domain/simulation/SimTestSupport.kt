package cz.nowass.dispenser.domain.simulation

import cz.nowass.dispenser.domain.DEFAULT_ZONE
import cz.nowass.dispenser.domain.Recurrence
import cz.nowass.dispenser.domain.Schedule
import java.time.Instant
import java.time.LocalDate
import java.time.LocalTime
import java.time.ZonedDateTime

internal fun at(y: Int, mo: Int, d: Int, h: Int, mi: Int = 0): Instant =
    ZonedDateTime.of(y, mo, d, h, mi, 0, 0, DEFAULT_ZONE).toInstant()

internal fun ml(v: Double): Long = Math.round(v * 1000)

internal fun daily(
    id: String,
    time: LocalTime,
    amountMl: Double,
    start: LocalDate = LocalDate.of(2026, 11, 1),
    interval: Int = 1,
    enabled: Boolean = true,
    end: LocalDate? = null,
    name: String = id,
) = Schedule(id, 1, name, enabled, amountMl, start, time, Recurrence.Daily(interval), end)

internal fun SimulationResult.ok(): SimulationResult.Ok = this as SimulationResult.Ok
