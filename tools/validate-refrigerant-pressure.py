import argparse
import re
from pathlib import Path

import CoolProp
from CoolProp.CoolProp import PropsSI


HEADER_PATH = (
    Path(__file__).resolve().parents[1]
    / "trainers"
    / "unified-master"
    / "firmware"
    / "src"
    / "RefrigerantPressure.h"
)
TABLES = {
    "kR410A": "R410A",
    "kR22": "R22",
    "kR32": "R32",
    "kR454B": "R454B.MIX",
    "kR134A": "R134a",
    "kR404A": "R404A",
    "kR407C": "R407C",
}
MIN_TEMPERATURE_F = -20
TEMPERATURE_STEP_F = 5
POINT_COUNT = 31
PSI_PER_PA = 1 / 6894.757293168
ATMOSPHERIC_PRESSURE_PSIG = 14.6959
MAX_ALLOWED_ERROR_PSI = 0.051
MAX_ALLOWED_INTERPOLATION_ERROR_PSI = 0.25
INTERPOLATION_SAMPLE_STEP_F = 0.5
EXPECTED_COOLPROP_VERSION = "8.0.0"


def parse_table(source: str, table_name: str) -> list[tuple[int, int]]:
    match = re.search(
        rf"static constexpr SaturationPoint {table_name}\[\] = \{{(.*?)\}};",
        source,
        re.DOTALL,
    )
    if match is None:
        raise ValueError(f"Missing table {table_name}")

    points = [
        (int(bubble), int(dew))
        for bubble, dew in re.findall(r"\{(-?\d+),\s*(-?\d+)\}", match.group(1))
    ]
    if len(points) != POINT_COUNT:
        raise ValueError(
            f"{table_name} has {len(points)} points; expected {POINT_COUNT}"
        )
    return points


def saturation_pressure_psig(fluid: str, temperature_f: float, quality: int) -> float:
    temperature_k = (temperature_f - 32.0) * 5.0 / 9.0 + 273.15
    pressure_pa = PropsSI("P", "T", temperature_k, "Q", quality, fluid)
    return pressure_pa * PSI_PER_PA - ATMOSPHERIC_PRESSURE_PSIG


def render_header() -> str:
    lines = [
        "#ifndef REFRIGERANT_PRESSURE_H",
        "#define REFRIGERANT_PRESSURE_H",
        "",
        "#include <Arduino.h>",
        "#include <stdint.h>",
        "",
        "namespace refrigerant_pressure {",
        "",
        f"// CoolProp {CoolProp.__version__} bubble/dew saturation pressure, stored in tenths of psig.",
        "struct SaturationPoint {",
        "  int16_t bubble;",
        "  int16_t dew;",
        "};",
        "",
        f"constexpr float kMinTemperatureF = {MIN_TEMPERATURE_F}.0f;",
        f"constexpr float kTemperatureStepF = {TEMPERATURE_STEP_F}.0f;",
        f"constexpr size_t kPointCount = {POINT_COUNT};",
        "",
    ]

    for table_name, fluid in TABLES.items():
        lines.append(f"static constexpr SaturationPoint {table_name}[] = {{")
        points = []
        for index in range(POINT_COUNT):
            temperature_f = MIN_TEMPERATURE_F + index * TEMPERATURE_STEP_F
            bubble = round(saturation_pressure_psig(fluid, temperature_f, 0) * 10)
            dew = round(saturation_pressure_psig(fluid, temperature_f, 1) * 10)
            points.append(f"{{{bubble}, {dew}}}")
        for offset in range(0, POINT_COUNT, 5):
            lines.append("  " + ", ".join(points[offset : offset + 5]) + ",")
        lines.extend(("};", ""))

    lines.extend(
        [
            "inline const SaturationPoint* tableFor(const String& refrigerant) {",
            '  if (refrigerant.equalsIgnoreCase("R410A")) return kR410A;',
            '  if (refrigerant.equalsIgnoreCase("R22")) return kR22;',
            '  if (refrigerant.equalsIgnoreCase("R32")) return kR32;',
            '  if (refrigerant.equalsIgnoreCase("R454B")) return kR454B;',
            '  if (refrigerant.equalsIgnoreCase("R134a")) return kR134A;',
            '  if (refrigerant.equalsIgnoreCase("R404A")) return kR404A;',
            '  if (refrigerant.equalsIgnoreCase("R407C")) return kR407C;',
            "  return nullptr;",
            "}",
            "",
            "inline bool saturationPressurePsig(const String& refrigerant, float temperatureF,",
            "                                   bool dewPoint, float& pressurePsig) {",
            "  const SaturationPoint* table = tableFor(refrigerant);",
            "  if (table == nullptr || temperatureF < kMinTemperatureF ||",
            "      temperatureF > 130.0f) {",
            "    return false;",
            "  }",
            "",
            "  const float position = (temperatureF - kMinTemperatureF) / kTemperatureStepF;",
            "  size_t lowerIndex = static_cast<size_t>(position);",
            "  float fraction = position - lowerIndex;",
            "  if (lowerIndex >= kPointCount - 1) {",
            "    lowerIndex = kPointCount - 2;",
            "    fraction = 1.0f;",
            "  }",
            "  const float lowerPressure = dewPoint ? table[lowerIndex].dew : table[lowerIndex].bubble;",
            "  const float upperPressure = dewPoint ? table[lowerIndex + 1].dew : table[lowerIndex + 1].bubble;",
            "  pressurePsig = (lowerPressure + (upperPressure - lowerPressure) * fraction) / 10.0f;",
            "  return true;",
            "}",
            "",
            "inline bool saturationTemperatureF(const String& refrigerant, float pressurePsig,",
            "                                   bool dewPoint, float& temperatureF) {",
            "  const SaturationPoint* table = tableFor(refrigerant);",
            "  if (table == nullptr) return false;",
            "",
            "  const float target = pressurePsig * 10.0f;",
            "  for (size_t index = 0; index < kPointCount - 1; ++index) {",
            "    const float lowerPressure = dewPoint ? table[index].dew : table[index].bubble;",
            "    const float upperPressure = dewPoint ? table[index + 1].dew : table[index + 1].bubble;",
            "    if (target >= lowerPressure && target <= upperPressure) {",
            "      const float fraction = (target - lowerPressure) / (upperPressure - lowerPressure);",
            "      temperatureF = kMinTemperatureF +",
            "                     (static_cast<float>(index) + fraction) * kTemperatureStepF;",
            "      return true;",
            "    }",
            "  }",
            "  return false;",
            "}",
            "",
            "inline bool meanSaturationPressurePsig(const String& refrigerant, float temperatureF,",
            "                                       float& pressurePsig) {",
            "  float bubblePressure = 0.0f;",
            "  float dewPressure = 0.0f;",
            "  if (!saturationPressurePsig(refrigerant, temperatureF, false, bubblePressure) ||",
            "      !saturationPressurePsig(refrigerant, temperatureF, true, dewPressure)) {",
            "    return false;",
            "  }",
            "  pressurePsig = (bubblePressure + dewPressure) * 0.5f;",
            "  return true;",
            "}",
            "",
            "}",
            "",
            "#endif",
            "",
        ]
    )
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--write",
        action="store_true",
        help="regenerate the firmware header from CoolProp 8.0.0",
    )
    args = parser.parse_args()

    if CoolProp.__version__ != EXPECTED_COOLPROP_VERSION:
        raise RuntimeError(
            f"Expected CoolProp {EXPECTED_COOLPROP_VERSION}, found {CoolProp.__version__}"
        )

    generated = render_header()
    if args.write:
        HEADER_PATH.write_text(generated, encoding="utf-8", newline="\n")

    source = HEADER_PATH.read_text(encoding="utf-8")
    if source != generated:
        raise AssertionError(
            "RefrigerantPressure.h differs from CoolProp output; "
            "run tools/validate-refrigerant-pressure.py --write"
        )

    worst_error = 0.0
    worst_interpolation_error = 0.0
    checked_points = 0
    interpolation_samples = 0
    samples_per_grid_interval = int(TEMPERATURE_STEP_F / INTERPOLATION_SAMPLE_STEP_F)

    for table_name, fluid in TABLES.items():
        points = parse_table(source, table_name)
        for index, (bubble_tenths, dew_tenths) in enumerate(points):
            temperature_f = MIN_TEMPERATURE_F + index * TEMPERATURE_STEP_F
            expected_bubble = saturation_pressure_psig(fluid, temperature_f, 0)
            expected_dew = saturation_pressure_psig(fluid, temperature_f, 1)
            bubble_error = abs(bubble_tenths / 10.0 - expected_bubble)
            dew_error = abs(dew_tenths / 10.0 - expected_dew)
            worst_error = max(worst_error, bubble_error, dew_error)
            checked_points += 1

            if max(bubble_error, dew_error) > MAX_ALLOWED_ERROR_PSI:
                raise AssertionError(
                    f"{fluid} at {temperature_f}F differs from CoolProp by "
                    f"{bubble_error:.3f}/{dew_error:.3f} psi"
                )

        sample_count = (POINT_COUNT - 1) * samples_per_grid_interval + 1
        for sample_index in range(sample_count):
            temperature_f = (
                MIN_TEMPERATURE_F + sample_index * INTERPOLATION_SAMPLE_STEP_F
            )
            position = sample_index / samples_per_grid_interval
            lower_index = min(int(position), POINT_COUNT - 2)
            fraction = position - lower_index

            for dew_point, quality in ((False, 0), (True, 1)):
                lower_tenths = points[lower_index][int(dew_point)]
                upper_tenths = points[lower_index + 1][int(dew_point)]
                interpolated_pressure = (
                    lower_tenths + (upper_tenths - lower_tenths) * fraction
                ) / 10.0
                expected_pressure = saturation_pressure_psig(
                    fluid, temperature_f, quality
                )
                interpolation_error = abs(interpolated_pressure - expected_pressure)
                worst_interpolation_error = max(
                    worst_interpolation_error, interpolation_error
                )
                interpolation_samples += 1

                if interpolation_error > MAX_ALLOWED_INTERPOLATION_ERROR_PSI:
                    raise AssertionError(
                        f"{fluid} interpolated {'dew' if dew_point else 'bubble'} "
                        f"pressure at {temperature_f}F differs from CoolProp by "
                        f"{interpolation_error:.3f} psi"
                    )

    print(
        f"Validated {checked_points} bubble/dew nodes (maximum node error "
        f"{worst_error:.3f} psi) and {interpolation_samples} interpolated "
        f"values (maximum error {worst_interpolation_error:.3f} psi)."
    )


if __name__ == "__main__":
    main()
