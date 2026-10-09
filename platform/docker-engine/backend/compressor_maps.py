"""Published compressor performance maps shared with the trainer firmware.

The ZP29K6E-PFV (R-410A), YA31K1E-PFV (R-454B) and YP31K1T-PFV (R-32) tables must stay
identical to ``kCompressorMap`` / ``kYa31CompressorMap`` / ``kYp31CompressorMap`` in
``trainers/unified-master/firmware/src/PhysicsEngine.cpp`` (test_compressor_maps checks this).
"""

from __future__ import annotations

import math
from typing import NamedTuple


class CompressorPoint(NamedTuple):
    capacity_btu_per_hour: float
    power_watts: float
    current_amps: float
    mass_flow_lb_per_hour: float


# Copeland ZP29K6E-PFV, HFC-410A, chart 511570-230: 60 Hz, 20 F superheat, 15 F subcooling.
ZP29_MODEL_NAME = "Copeland ZP29K6E-PFV (performance chart 511570-230)"
ZP29_EVAPORATING_TEMPS_F = (-10.0, 0.0, 10.0, 20.0, 30.0, 40.0, 45.0, 50.0, 55.0)
ZP29_ROWS: tuple[tuple[float, int, tuple[CompressorPoint, ...]], ...] = tuple(
    (cond, first, tuple(CompressorPoint(*p) for p in points))
    for cond, first, points in (
        (80.0, 0, (
            (12600, 1560, 6.9, 151), (15850, 1560, 6.9, 187), (19800, 1555, 6.9, 231),
            (24500, 1530, 6.8, 282), (29900, 1490, 6.6, 341), (36200, 1420, 6.3, 408),
            (39600, 1375, 6.2, 444), (43300, 1320, 6.0, 483), (47100, 1255, 5.9, 524),
        )),
        (90.0, 0, (
            (11500, 1750, 7.8, 144), (14700, 1755, 7.8, 182), (18550, 1750, 7.7, 227),
            (23200, 1740, 7.7, 279), (28500, 1715, 7.5, 340), (34700, 1665, 7.4, 409),
            (38000, 1635, 7.2, 447), (41600, 1595, 7.1, 486), (45400, 1545, 7.0, 528),
        )),
        (100.0, 0, (
            (10400, 1980, 8.8, 138), (13500, 1975, 8.7, 176), (17250, 1970, 8.7, 222),
            (21700, 1960, 8.6, 276), (26900, 1945, 8.5, 338), (32900, 1910, 8.4, 408),
            (36200, 1885, 8.3, 446), (39700, 1855, 8.2, 487), (43400, 1820, 8.1, 530),
        )),
        (110.0, 1, (
            (15950, 2230, 9.8, 217), (20200, 2220, 9.8, 271), (25200, 2200, 9.6, 333),
            (31000, 2170, 9.5, 405), (34100, 2150, 9.4, 444), (37500, 2130, 9.4, 485),
            (41100, 2100, 9.3, 529),
        )),
        (115.0, 1, (
            (15300, 2380, 10.5, 214), (19450, 2360, 10.4, 268), (24300, 2340, 10.3, 331),
            (29900, 2310, 10.1, 402), (33000, 2300, 10.1, 442), (36300, 2270, 10.0, 483),
            (39800, 2250, 9.9, 527),
        )),
        (130.0, 4, (
            (21400, 2850, 12.5, 321), (26600, 2810, 12.3, 393), (29400, 2790, 12.3, 433),
            (32500, 2770, 12.2, 475), (35700, 2750, 12.1, 520),
        )),
        (140.0, 5, (
            (24200, 3230, 14.2, 386), (26800, 3200, 14.1, 425), (29700, 3170, 14.0, 467),
            (32700, 3140, 13.8, 512),
        )),
        (145.0, 5, (
            (22900, 3460, 15.3, 382), (25500, 3430, 15.1, 421), (28200, 3400, 15.0, 463),
            (31200, 3370, 14.9, 508),
        )),
    )
)

# Copeland YA31K1E-PFV (3-ton), HFO-454B, 208/230-1-60, chart 99949-230 (printed 4/8/2025):
# 20 F superheat, 15 F subcooling, 95 F ambient air over, current @ 230 V, nominal +/-5%.
# Saturated dew-point temperatures. Rows: (condensing F, first evaporating index, points).
YA31_MODEL_NAME = "Copeland YA31K1E-PFV (performance chart 99949-230)"
YA31_EVAPORATING_TEMPS_F = (-10.0, -5.0, 0.0, 10.0, 25.0, 35.0, 55.0, 70.0, 77.0)
YA31_ROWS: tuple[tuple[float, int, tuple[CompressorPoint, ...]], ...] = tuple(
    (cond, first, tuple(CompressorPoint(*p) for p in points))
    for cond, first, points in (
        (50.0, 0, (
            (17150, 1135, 5.4, 152), (18800, 1125, 5.3, 166), (20600, 1120, 5.3, 181),
            (24800, 1100, 5.2, 216), (32800, 1050, 5.0, 282),
        )),
        (70.0, 0, (
            (13950, 1425, 6.6, 133), (15900, 1405, 6.5, 151), (17900, 1390, 6.5, 169),
            (22200, 1370, 6.4, 207), (29900, 1335, 6.2, 275), (36300, 1300, 6.1, 331),
        )),
        (80.0, 0, (
            (12250, 1635, 7.5, 122), (14350, 1610, 7.4, 142), (16450, 1585, 7.3, 162),
            (20900, 1555, 7.2, 203), (28600, 1510, 7.0, 273), (34700, 1480, 6.9, 329),
            (50700, 1380, 6.4, 472),
        )),
        (100.0, 0, (
            (8400, 2240, 10.1, 91), (10800, 2190, 9.9, 117), (13200, 2140, 9.7, 141),
            (17950, 2070, 9.3, 190), (25700, 1985, 9.0, 267), (31500, 1945, 8.8, 325),
            (46100, 1850, 8.4, 467), (60500, 1755, 8.0, 606),
        )),
        (110.0, 1, (
            (8680, 2580, 11.6, 98), (11200, 2520, 11.3, 126), (16200, 2420, 10.8, 180),
            (24000, 2300, 10.4, 262), (29800, 2240, 10.1, 322), (43800, 2140, 9.6, 464),
            (57400, 2040, 9.2, 602), (64900, 1990, 9.0, 678),
        )),
        (120.0, 3, (
            (14200, 2840, 12.7, 166), (22100, 2680, 12.0, 254), (27800, 2600, 11.7, 316),
            (41300, 2470, 11.1, 460), (54200, 2370, 10.7, 597), (61300, 2320, 10.4, 672),
        )),
        (130.0, 4, (
            (19950, 3140, 14.0, 242), (25600, 3030, 13.5, 308), (38600, 2860, 12.8, 455),
            (50900, 2750, 12.3, 591), (57600, 2700, 12.1, 666),
        )),
        (140.0, 5, (
            (23100, 3540, 15.8, 295), (35700, 3320, 14.8, 447), (47300, 3190, 14.2, 584),
            (53600, 3120, 13.9, 659),
        )),
        (145.0, 6, (
            (34100, 3580, 16.0, 441),
        )),
    )
)

# Copeland YP31K1T-PFV (3-ton), HFC-32, 208/230-1-60, chart 118323-230 (printed 6/13/2025):
# 20 F superheat, 15 F subcooling, 95 F ambient air over, current @ 230 V, nominal +/-5%.
YP31_MODEL_NAME = "Copeland YP31K1T-PFV (performance chart 118323-230)"
YP31_EVAPORATING_TEMPS_F = (-10.0, -5.0, 0.0, 10.0, 40.0, 45.0, 55.0, 70.0, 77.0)
YP31_ROWS: tuple[tuple[float, int, tuple[CompressorPoint, ...]], ...] = tuple(
    (cond, first, tuple(CompressorPoint(*p) for p in points))
    for cond, first, points in (
        (50.0, 0, (
            (15550, 939, 4.8, 114), (17150, 941, 4.8, 126), (19000, 942, 4.8, 138),
            (23300, 938, 4.7, 169),
        )),
        (70.0, 0, (
            (13700, 1385, 6.6, 107), (15350, 1385, 6.5, 120), (17100, 1380, 6.5, 133),
            (21300, 1375, 6.4, 164), (39300, 1300, 6.1, 298), (43200, 1275, 6.0, 327),
        )),
        (90.0, 0, (
            (11600, 1845, 8.4, 98), (13300, 1840, 8.4, 111), (15100, 1830, 8.3, 126),
            (19200, 1815, 8.3, 159), (36200, 1740, 8.0, 294), (39800, 1715, 7.9, 323),
            (47900, 1655, 7.6, 387),
        )),
        (100.0, 1, (
            (11950, 2100, 9.5, 104), (13800, 2090, 9.4, 120), (17950, 2070, 9.3, 155),
            (34600, 1985, 9.0, 292), (38100, 1965, 8.9, 321), (45800, 1905, 8.7, 385),
            (59500, 1775, 8.0, 498),
        )),
        (110.0, 1, (
            (10300, 2410, 10.7, 94), (12250, 2400, 10.7, 111), (16450, 2370, 10.6, 148),
            (32800, 2270, 10.2, 289), (36200, 2240, 10.2, 318), (43700, 2190, 9.9, 382),
            (56800, 2060, 9.3, 495), (63800, 1985, 8.9, 555),
        )),
        (120.0, 3, (
            (14550, 2710, 12.1, 137), (30700, 2600, 11.7, 283), (34100, 2570, 11.6, 313),
            (41300, 2510, 11.3, 377), (54000, 2390, 10.7, 491), (60700, 2310, 10.3, 551),
        )),
        (130.0, 4, (
            (28400, 2990, 13.4, 274), (31600, 2960, 13.3, 304), (38600, 2900, 13.0, 371),
            (50900, 2770, 12.4, 485), (57300, 2700, 12.0, 546),
        )),
        (140.0, 4, (
            (25500, 3450, 15.5, 260), (28700, 3420, 15.4, 292), (35600, 3350, 15.1, 360),
            (47400, 3230, 14.5, 477), (53600, 3160, 14.1, 539),
        )),
        (145.0, 4, (
            (23900, 3710, 16.6, 251), (27100, 3680, 16.5, 283), (33900, 3610, 16.2, 353),
        )),
    )
)

# Refrigerants with their own published compressor chart.
COMPRESSOR_PROFILES = {
    "R410A": (ZP29_MODEL_NAME, ZP29_EVAPORATING_TEMPS_F, ZP29_ROWS),
    "R454B": (YA31_MODEL_NAME, YA31_EVAPORATING_TEMPS_F, YA31_ROWS),
    "R32": (YP31_MODEL_NAME, YP31_EVAPORATING_TEMPS_F, YP31_ROWS),
}

# Fault factors matching the firmware PhysicsEngine (fault 44 bypass, fault 45 worn valves).
BYPASS_AMPS_FACTOR = 0.5
INEFFICIENT_AMPS_FACTOR = 0.7 / 0.9


def _lerp(a: CompressorPoint, b: CompressorPoint, fraction: float) -> CompressorPoint:
    return CompressorPoint(*(x + (y - x) * fraction for x, y in zip(a, b)))


def _evaluate_row(row, evaps, evap_f: float) -> CompressorPoint:
    _, first, points = row
    temps = evaps[first : first + len(points)]
    if evap_f <= temps[0]:
        return points[0]
    if evap_f >= temps[-1]:
        return points[-1]
    for i in range(len(points) - 1):
        if evap_f <= temps[i + 1]:
            return _lerp(points[i], points[i + 1], (evap_f - temps[i]) / (temps[i + 1] - temps[i]))
    return points[-1]


def evaluate_compressor(refrigerant: str, evap_f: float, cond_f: float) -> CompressorPoint | None:
    """Interpolates the refrigerant's compressor chart; inputs beyond the chart clamp to its edge."""
    profile = COMPRESSOR_PROFILES.get((refrigerant or "").upper())
    if profile is None:
        return None
    _, evaps, rows = profile
    if cond_f <= rows[0][0]:
        return _evaluate_row(rows[0], evaps, evap_f)
    if cond_f >= rows[-1][0]:
        return _evaluate_row(rows[-1], evaps, evap_f)
    for lower, upper in zip(rows, rows[1:]):
        if cond_f <= upper[0]:
            fraction = (cond_f - lower[0]) / (upper[0] - lower[0])
            return _lerp(_evaluate_row(lower, evaps, evap_f), _evaluate_row(upper, evaps, evap_f), fraction)
    return _evaluate_row(rows[-1], evaps, evap_f)


def compressor_model_name(refrigerant: str) -> str | None:
    profile = COMPRESSOR_PROFILES.get((refrigerant or "").upper())
    return profile[0] if profile else None


class CompressorElectricalSpec(NamedTuple):
    model: str
    run_cap_uf: float
    run_cap_volts: float
    start_cap_uf_low: float
    start_cap_uf_high: float
    start_cap_volts: float
    start_winding_ohms: float  # C-S
    run_winding_ohms: float  # C-R
    potential_relay: str


# Copeland electrical component sheets (208/230-1-60). Must match PhysicsEngine.cpp.
ZP29_ELECTRICAL = CompressorElectricalSpec(
    "ZP29K6E-PFV", 45.0, 370.0, 88.0, 106.0, 330.0, 1.58, 0.92, "040-0166-37"
)
YA31_ELECTRICAL = CompressorElectricalSpec(
    "YA31K1E-PFV", 40.0, 370.0, 88.0, 106.0, 330.0, 1.43, 0.72, "040-0166-37"
)
YP31_ELECTRICAL = CompressorElectricalSpec(
    "YP31K1T-PFV", 40.0, 370.0, 88.0, 106.0, 330.0, 1.43, 0.72, "040-0166-37"
)
ELECTRICAL_SPECS = {"R454B": YA31_ELECTRICAL, "R32": YP31_ELECTRICAL}

# Running-circuit model constants, mirrored from PhysicsEngine.cpp.
NO_LOAD_LINE_VOLTS = 241.5
LINE_DROP_VOLTS_PER_AMP = 0.1
LOCKED_ROTOR_AMPS_THRESHOLD = 100.0
RUNNING_CAP_VOLTS_RATIO_BASE = 1.48
RUNNING_CAP_VOLTS_RATIO_PER_AMP = 0.012
RUNNING_CAP_VOLTS_RATIO_MIN = 1.25
RUNNING_CAP_VOLTS_RATIO_MAX = 1.5
LOCKED_ROTOR_CAP_VOLTS_RATIO = 0.6
IN_CIRCUIT_CAP_FRACTION = 0.985
MICROFARAD_CONSTANT = 2652.0
WINDING_PHASE_COS = -0.17365
WINDING_PHASE_SIN = 0.98481


def compressor_electrical_spec(refrigerant: str) -> CompressorElectricalSpec:
    """YA31K1E for R454B, YP31K1T for R32; every other refrigerant runs the ZP29K6E."""
    return ELECTRICAL_SPECS.get((refrigerant or "").upper(), ZP29_ELECTRICAL)


def compressor_electrical_reading(
    refrigerant: str, comp_amps: float, total_line_amps: float
) -> dict[str, float | str]:
    """Telemetry fields for the compressor circuit, identical to firmware getStatusJSON."""
    spec = compressor_electrical_spec(refrigerant)
    comp_amps = max(float(comp_amps or 0.0), 0.0)
    line_volts = NO_LOAD_LINE_VOLTS - LINE_DROP_VOLTS_PER_AMP * max(float(total_line_amps or 0.0), 0.0)
    cap_volts = cap_uf = start_amps = run_amps = 0.0
    if comp_amps >= 0.5:
        if comp_amps >= LOCKED_ROTOR_AMPS_THRESHOLD:
            ratio = LOCKED_ROTOR_CAP_VOLTS_RATIO
            uf_in_circuit = spec.run_cap_uf + 0.5 * (spec.start_cap_uf_low + spec.start_cap_uf_high)
        else:
            ratio = min(
                max(RUNNING_CAP_VOLTS_RATIO_BASE - RUNNING_CAP_VOLTS_RATIO_PER_AMP * comp_amps,
                    RUNNING_CAP_VOLTS_RATIO_MIN),
                RUNNING_CAP_VOLTS_RATIO_MAX,
            )
            uf_in_circuit = spec.run_cap_uf
        cap_uf = uf_in_circuit * IN_CIRCUIT_CAP_FRACTION
        cap_volts = line_volts * ratio
        start_amps = cap_volts * cap_uf / MICROFARAD_CONSTANT
        run_amps = -start_amps * WINDING_PHASE_COS + math.sqrt(
            max(comp_amps**2 - (start_amps * WINDING_PHASE_SIN) ** 2, 0.0)
        )
    return {
        "compressor_electrical_model": spec.model,
        "run_cap_rated_uf": spec.run_cap_uf,
        "run_cap_rated_volts": spec.run_cap_volts,
        "start_cap_uf_low": spec.start_cap_uf_low,
        "start_cap_uf_high": spec.start_cap_uf_high,
        "start_cap_rated_volts": spec.start_cap_volts,
        "winding_start_ohms": spec.start_winding_ohms,
        "winding_run_ohms": spec.run_winding_ohms,
        "potential_relay": spec.potential_relay,
        "line_volts": round(line_volts, 1),
        "run_cap_volts": round(cap_volts, 1),
        "run_cap_uf": round(cap_uf, 1),
        "comp_start_amps": round(start_amps, 1),
        "comp_run_amps": round(run_amps, 1),
    }
