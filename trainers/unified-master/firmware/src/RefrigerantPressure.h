#ifndef REFRIGERANT_PRESSURE_H
#define REFRIGERANT_PRESSURE_H

#include <Arduino.h>
#include <stdint.h>

namespace refrigerant_pressure {

// CoolProp 8.0.0 bubble/dew saturation pressure, stored in tenths of psig.
struct SaturationPoint {
  int16_t bubble;
  int16_t dew;
};

constexpr float kMinTemperatureF = -20.0f;
constexpr float kTemperatureStepF = 5.0f;
constexpr size_t kPointCount = 31;

static constexpr SaturationPoint kR410A[] = {
  {263, 262}, {312, 310}, {365, 363}, {422, 420}, {484, 482},
  {552, 549}, {624, 622}, {703, 700}, {787, 784}, {877, 874},
  {974, 970}, {1077, 1073}, {1188, 1184}, {1306, 1301}, {1431, 1426},
  {1565, 1560}, {1707, 1701}, {1858, 1851}, {2017, 2011}, {2186, 2179},
  {2365, 2357}, {2554, 2546}, {2753, 2745}, {2964, 2954}, {3185, 3176},
  {3419, 3408}, {3665, 3654}, {3923, 3912}, {4195, 4183}, {4480, 4468},
  {4780, 4768},
};

static constexpr SaturationPoint kR22[] = {
  {102, 102}, {132, 132}, {165, 165}, {201, 201}, {240, 240},
  {283, 283}, {328, 328}, {378, 378}, {431, 431}, {488, 488},
  {550, 550}, {615, 615}, {686, 686}, {761, 761}, {841, 841},
  {926, 926}, {1016, 1016}, {1112, 1112}, {1214, 1214}, {1322, 1322},
  {1436, 1436}, {1557, 1557}, {1684, 1684}, {1818, 1818}, {1959, 1959},
  {2108, 2108}, {2264, 2264}, {2428, 2428}, {2600, 2600}, {2780, 2780},
  {2969, 2969},
};

static constexpr SaturationPoint kR32[] = {
  {268, 268}, {317, 317}, {371, 371}, {429, 429}, {493, 493},
  {561, 561}, {635, 635}, {714, 714}, {800, 800}, {892, 892},
  {991, 991}, {1097, 1097}, {1210, 1210}, {1330, 1330}, {1458, 1458},
  {1595, 1595}, {1740, 1740}, {1895, 1895}, {2058, 2058}, {2232, 2232},
  {2415, 2415}, {2609, 2609}, {2813, 2813}, {3029, 3029}, {3257, 3257},
  {3496, 3496}, {3749, 3749}, {4014, 4014}, {4293, 4293}, {4587, 4587},
  {4895, 4895},
};

static constexpr SaturationPoint kR454B[] = {
  {247, 227}, {294, 271}, {345, 320}, {400, 372}, {459, 429},
  {524, 491}, {593, 557}, {668, 629}, {748, 706}, {835, 789},
  {927, 878}, {1026, 973}, {1132, 1075}, {1245, 1183}, {1364, 1299},
  {1492, 1422}, {1627, 1553}, {1771, 1692}, {1923, 1839}, {2084, 1995},
  {2254, 2160}, {2434, 2335}, {2623, 2519}, {2823, 2714}, {3033, 2919},
  {3254, 3135}, {3486, 3363}, {3730, 3603}, {3986, 3855}, {4255, 4121},
  {4536, 4400},
};

static constexpr SaturationPoint kR134A[] = {
  {-18, -18}, {0, 0}, {19, 19}, {41, 41}, {65, 65},
  {91, 91}, {119, 119}, {150, 150}, {184, 184}, {221, 221},
  {261, 261}, {304, 304}, {350, 350}, {401, 401}, {454, 454},
  {512, 512}, {574, 574}, {640, 640}, {711, 711}, {787, 787},
  {867, 867}, {952, 952}, {1043, 1043}, {1139, 1139}, {1242, 1242},
  {1350, 1350}, {1464, 1464}, {1584, 1584}, {1712, 1712}, {1846, 1846},
  {1987, 1987},
};

static constexpr SaturationPoint kR404A[] = {
  {168, 160}, {205, 197}, {246, 236}, {289, 279}, {337, 326},
  {388, 377}, {443, 431}, {502, 490}, {566, 553}, {634, 621},
  {707, 693}, {786, 771}, {869, 854}, {958, 942}, {1053, 1036},
  {1153, 1136}, {1260, 1242}, {1373, 1355}, {1493, 1474}, {1620, 1601},
  {1754, 1734}, {1895, 1875}, {2045, 2024}, {2202, 2181}, {2368, 2347},
  {2542, 2521}, {2726, 2704}, {2918, 2897}, {3121, 3099}, {3334, 3312},
  {3557, 3536},
};

static constexpr SaturationPoint kR407C[] = {
  {137, 65}, {172, 93}, {209, 123}, {250, 157}, {295, 194},
  {343, 235}, {395, 279}, {452, 327}, {512, 379}, {577, 435},
  {647, 496}, {722, 561}, {802, 632}, {888, 707}, {979, 788},
  {1076, 875}, {1179, 968}, {1289, 1067}, {1405, 1173}, {1528, 1285},
  {1658, 1405}, {1796, 1532}, {1941, 1667}, {2094, 1810}, {2255, 1961},
  {2424, 2121}, {2602, 2290}, {2789, 2469}, {2986, 2658}, {3192, 2856},
  {3407, 3066},
};

inline const SaturationPoint* tableFor(const String& refrigerant) {
  if (refrigerant.equalsIgnoreCase("R410A")) return kR410A;
  if (refrigerant.equalsIgnoreCase("R22")) return kR22;
  if (refrigerant.equalsIgnoreCase("R32")) return kR32;
  if (refrigerant.equalsIgnoreCase("R454B")) return kR454B;
  if (refrigerant.equalsIgnoreCase("R134a")) return kR134A;
  if (refrigerant.equalsIgnoreCase("R404A")) return kR404A;
  if (refrigerant.equalsIgnoreCase("R407C")) return kR407C;
  return nullptr;
}

inline bool saturationPressurePsig(const String& refrigerant, float temperatureF,
                                   bool dewPoint, float& pressurePsig) {
  const SaturationPoint* table = tableFor(refrigerant);
  if (table == nullptr || temperatureF < kMinTemperatureF ||
      temperatureF > 130.0f) {
    return false;
  }

  const float position = (temperatureF - kMinTemperatureF) / kTemperatureStepF;
  size_t lowerIndex = static_cast<size_t>(position);
  float fraction = position - lowerIndex;
  if (lowerIndex >= kPointCount - 1) {
    lowerIndex = kPointCount - 2;
    fraction = 1.0f;
  }
  const float lowerPressure = dewPoint ? table[lowerIndex].dew : table[lowerIndex].bubble;
  const float upperPressure = dewPoint ? table[lowerIndex + 1].dew : table[lowerIndex + 1].bubble;
  pressurePsig = (lowerPressure + (upperPressure - lowerPressure) * fraction) / 10.0f;
  return true;
}

inline bool saturationTemperatureF(const String& refrigerant, float pressurePsig,
                                   bool dewPoint, float& temperatureF) {
  const SaturationPoint* table = tableFor(refrigerant);
  if (table == nullptr) return false;

  const float target = pressurePsig * 10.0f;
  for (size_t index = 0; index < kPointCount - 1; ++index) {
    const float lowerPressure = dewPoint ? table[index].dew : table[index].bubble;
    const float upperPressure = dewPoint ? table[index + 1].dew : table[index + 1].bubble;
    if (target >= lowerPressure && target <= upperPressure) {
      const float fraction = (target - lowerPressure) / (upperPressure - lowerPressure);
      temperatureF = kMinTemperatureF +
                     (static_cast<float>(index) + fraction) * kTemperatureStepF;
      return true;
    }
  }
  return false;
}

inline bool meanSaturationPressurePsig(const String& refrigerant, float temperatureF,
                                       float& pressurePsig) {
  float bubblePressure = 0.0f;
  float dewPressure = 0.0f;
  if (!saturationPressurePsig(refrigerant, temperatureF, false, bubblePressure) ||
      !saturationPressurePsig(refrigerant, temperatureF, true, dewPressure)) {
    return false;
  }
  pressurePsig = (bubblePressure + dewPressure) * 0.5f;
  return true;
}

}

#endif
