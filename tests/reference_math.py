"""Independent offline oracle, not production DSP. Python stdlib only.

Raw energy lists, Decimal gating and nearest-order quantiles deliberately do not use the
production histogram, streaming core, host state or any third-party meter.
Sources and conventions: ITU-R BS.1770-5 and EBU Tech 3341/3342.
"""
from decimal import Decimal, localcontext
import math

OFFSET = -0.691
SHELF_B = (1.53512485958697, -2.69169618940638, 1.19839281085285)
SHELF_A = (1.0, -1.69065929318241, 0.73248077421585)
RLB_B = (1.0, -2.0, 1.0)
RLB_A = (1.0, -1.99004745483398, 0.99007225036621)
RATES = (44100, 48000, 88200, 96000, 176400, 192000)


def to_power(lufs):
    return 10.0 ** ((lufs - OFFSET) / 10.0)


def to_lufs(power):
    return OFFSET + 10.0 * math.log10(power) if power > 0 else -math.inf


def integrated(energies):
    """Two-stage exact-list oracle; Decimal avoids a histogram's boundary decisions."""
    with localcontext() as context:
        context.prec = 50
        threshold = Decimal(10) ** ((Decimal(-70) - Decimal('-0.691')) / 10)
        absolute = [Decimal(str(e)) for e in energies if math.isfinite(e) and e > 0]
        absolute = [e for e in absolute if e > threshold]
        if not absolute:
            return -math.inf
        relative = sum(absolute) / len(absolute) / 10
        selected = [e for e in absolute if e > relative]
        mean = sum(selected) / len(selected)
        return float(Decimal('-0.691') + 10 * mean.log10())


def order_statistic(values, probability):
    if not values:
        return math.nan
    ordered = sorted(values)
    index = math.floor((len(ordered) - 1) * probability + 0.5)
    return ordered[index]


def loudness_range(energies):
    absolute = [e for e in energies if math.isfinite(e) and e >= to_power(-70)]
    if not absolute:
        return math.nan
    relative = math.fsum(absolute) / len(absolute) / 100
    selected = [to_lufs(e) for e in absolute if e >= relative]
    return order_statistic(selected, 0.95) - order_statistic(selected, 0.10)


def section_short_term(levels, section_seconds=20):
    """Analytical 3-s box energy for level sections at 100-ms grid; no tail padding.

    Levels specify ideal post-K summed-power LUFS. A physical two-channel sine fixture
    must separately account for the per-channel peak amplitude/K response and transients.
    """
    powers = [to_power(level) for level in levels]
    result = []
    for end_tick in range(30, section_seconds * len(levels) * 10 + 1):
        begin, end = end_tick - 30, end_tick
        energy = math.fsum(
            max(0, min(end, (i + 1) * section_seconds * 10)
                - max(begin, i * section_seconds * 10)) * power
            for i, power in enumerate(powers)
        ) / 30
        result.append(energy)
    return result


def filter_tone_gain_48k(frequency=1000.0):
    """Time-domain direct recurrence, independent of C++ spectral evaluation."""
    states = [[0.0, 0.0, 0.0, 0.0], [0.0, 0.0, 0.0, 0.0]]
    total = 0.0
    for sample in range(96000):
        value = math.sin(2 * math.pi * frequency * sample / 48000)
        for state, b, a in zip(states, (SHELF_B, RLB_B), (SHELF_A, RLB_A)):
            x1, x2, y1, y2 = state
            filtered = b[0] * value + b[1] * x1 + b[2] * x2 - a[1] * y1 - a[2] * y2
            state[:] = value, x1, filtered, y1
            value = filtered
        if sample >= 48000:
            total += value * value
    return 10 * math.log10((total / 48000) / 0.5)


def anchors():
    gain = filter_tone_gain_48k()
    return {
        'k_gain_1k_db': gain,
        'mono_peak1_1k_lufs': OFFSET + gain - 10 * math.log10(2),
        'stereo_peak01_1k_lufs': OFFSET + gain - 20,
        'integrated_1_001': integrated([1, 0.01]),
        'lra_20_30': loudness_range(section_short_term([-20, -30])),
        'tp19_analytical_dbtp': 20 * math.log10(1.41),
    }
