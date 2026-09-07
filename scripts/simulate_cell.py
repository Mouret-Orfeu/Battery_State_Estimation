#!/usr/bin/env python3
"""
simulate_cell.py — Li-Ion Cell Simulator
Generates a synthetic current profile (charge, discharge, or mixed) and the
resulting voltage and true SoC trajectory via ECM simulation and coulomb counting.

Usage:
    python3 scripts/simulate_cell.py --n-cycles 5
    python3 scripts/simulate_cell.py --capacity 3.4 --n-cycles 2
    python3 scripts/simulate_cell.py --profile charge --initial-soc 20 --charge-duration-mean 3600

The mixed profile is sized by --n-cycles: its total duration is whatever the
requested sub-cycles and rests add up to, so no sub-cycle is ever cut short.
The single-phase profiles run for one sub-cycle, sized by --charge-duration-mean
or --discharge-duration-mean respectively.

The profile is generated, simulated and written as a stream: samples are produced
one at a time and appended to the CSV in chunks of --chunk-rows, so peak memory
stays bounded no matter how long the run is. Sample count scales as 1/--dt, so
runs of thousands of cycles reach tens of millions of rows.

Output is always saved to docs/simulated_cell_behavior/simulated_cell_behavior_i.csv,
where i follows the highest existing index in that folder.

Outputs a CSV with columns:
    time_s, current_a, voltage_mv, true_soc_pct

Author: Kamal Kadakara and Orfeu Mouret
"""

import argparse
import math
import csv
import random
import re
import pathlib
from collections.abc import Iterator
import matplotlib.pyplot as plt

_BASE_DIR  = pathlib.Path(__file__).resolve().parent.parent / "docs" / "simulated_cell_behavior"
CSV_DIR    = _BASE_DIR / "csv"
PLOTS_DIR  = _BASE_DIR / "plots" / "cycling_signal_plots"

def _next_output_paths() -> tuple[pathlib.Path, pathlib.Path]:
    CSV_DIR.mkdir(parents=True, exist_ok=True)
    PLOTS_DIR.mkdir(parents=True, exist_ok=True)
    pattern = re.compile(r'^simulated_cell_behavior_(\d+)\.csv$')
    max_idx = -1
    for f in CSV_DIR.iterdir():
        m = pattern.match(f.name)
        if m:
            max_idx = max(max_idx, int(m.group(1)))
    idx = max_idx + 1
    csv_path  = CSV_DIR   / f"simulated_cell_behavior_{idx}.csv"
    plot_path = PLOTS_DIR / f"simulated_cell_behavior_plot_{idx}.png"
    return csv_path, plot_path

# ---- Simulation parameters ----

# Sampling period, matching the integration step the C estimators run at
DEFAULT_DT_S = 0.4

# Records buffered before each flush to CSV. Only this many are ever resident, so
# it trades RAM against write syscalls rather than capping the total run length
DEFAULT_CHUNK_ROWS = 100_000

# Upper bound on samples retained for the plot. A multi-thousand-cycle run holds
# far more rows than a figure can resolve, so the curve is decimated on the fly
DEFAULT_PLOT_MAX_POINTS = 100_000

# Shortest sub-cycle generate_mixed_cycles emits; shorter draws are clamped up to it
MIN_SUB_DURATION_S = 10

CSV_FIELDNAMES = ['time_s', 'current_a', 'voltage_mv', 'true_soc_pct']

# Max per-step change in current during discharge and charge
DISCHARGE_MAX_SLEW = 5.0  
CHARGE_MAX_SLEW    = 5.0

# Discharge state (cruise, accelerate, decelerate, regen, idle) proportion percentages (sum to 100)
DISCHARGE_STATE_WEIGHTS = [40, 20, 20, 0, 20]

# Discharge state duration distribution in seconds
DEFAULT_DISCHARGE_STATE_DURATION_DISTRIB = lambda:random.uniform(30.0, 60.0*5.0)

# ---- Mixed profile default parameters ----
DEFAULT_MIXED_DISCHARGE_DURATION_MEAN_S = 60.0 * 30.0  # 30 min
DEFAULT_MIXED_DISCHARGE_DURATION_STD_S  = 60.0 * 10.0  # 10 min
DEFAULT_MIXED_CHARGE_DURATION_MEAN_S    = 60.0 * 30.0  # 30 min
DEFAULT_MIXED_CHARGE_DURATION_STD_S     = 60.0 * 10.0  # 10 min
DEFAULT_MIXED_REST_DURATION_MEAN_S      = 60.0 * 5.0   # 5 min
DEFAULT_MIXED_REST_DURATION_STD_S       = 60.0 * 1.0   # 1 min
DEFAULT_MIXED_FIRST_PHASE               = 'discharge'
DEFAULT_MIXED_CYCLES                    = 5     # charge/discharge pairs per run

# ---- ECM Parameters (NMC, 25°C) ----
R0 = 0.005   # Ohmic resistance [Ω]
R1 = 0.008   # RC resistance [Ω]
C1 = 1500.0  # RC capacitance [F]

# ---- Cell Parameters (must match bms_types.h / soc_ekf.c) ----
# BMS_CELL_CAPACITY_INI_AH = 60.0
# 3.4 Ah for the UL-PUR dataset. Keep this equal to BMS_CELL_CAPACITY_INI_AH in
# bms_types.h: the SoH estimator measures Qmax from the simulated current and
# judges it against the C-side nominal, so a mismatch here reads as capacity
# fade (or as an impossibly large cell) rather than as the healthy cell simulated.
BMS_CELL_CAPACITY_INI_AH = 3.4

# ---- OCV Table (0–100%, 5% steps) ----
OCV_TABLE_MV = [
    3000, 3100, 3250, 3350, 3420, 3470, 3520, 3560,
    3600, 3630, 3660, 3690, 3720, 3760, 3800, 3850,
    3920, 4010, 4080, 4150, 4200
]


def ocv_from_soc(soc_pct: float) -> float:
    """Linear interpolation of OCV from SoC table."""
    soc_pct = max(0.0, min(100.0, soc_pct))
    idx = int(soc_pct / 5.0)
    if idx >= 20:
        return OCV_TABLE_MV[20]
    frac = (soc_pct - idx * 5.0) / 5.0
    return OCV_TABLE_MV[idx] + frac * (OCV_TABLE_MV[idx + 1] - OCV_TABLE_MV[idx])


# ---- Shared profile helper ----

def _apply_slew(current: float, target: float, max_delta: float) -> float:
    """Clamp the per-step change in current to ±max_delta (slew rate limit)."""
    delta = target - current
    if delta > max_delta:
        return current + max_delta
    if delta < -max_delta:
        return current - max_delta
    return target


# ---- Current profile generators ----

# This function was designed for EV-like behavior, thus the states "accelerate, regen" etc
# As I don't want regen behavior in my case, I put regen weight to 0 here
def generate_discharge_cycle(
    duration_s: int,
    dt: float = DEFAULT_DT_S,
    discharge_slew_rate: float = DISCHARGE_MAX_SLEW,
    state_duration_min_s: float = 30.0,
    state_duration_max_s: float = 300.0,
    discharge_state_weights: list = None,
    idle_current: float = -2.0,
    accelerate_current_min: float = -80.0,
    accelerate_current_max: float = -40.0,
    cruise_current_min: float = -30.0,
    cruise_current_max: float = -15.0,
    decelerate_current_min: float = -15.0,
    decelerate_current_max: float = -5.0,
    regen_current_min: float = 10.0,
    regen_current_max: float = 40.0,
) -> Iterator[tuple[float, float]]:
    """
    Generate a synthetic WLTP-inspired current profile.
    Positive = charge (regen), Negative = discharge (driving).

    Yields (time_s, current_a) one sample at a time so a long profile never has to
    exist in memory all at once.
    """
    if discharge_state_weights is None:
        discharge_state_weights = DISCHARGE_STATE_WEIGHTS

    steps = int(duration_s / dt)
    t = 0.0

    state = 'cruise'
    state_timer = 0.0
    current = (cruise_current_min + cruise_current_max) / 2.0
    max_slew = discharge_slew_rate * dt

    for _ in range(steps):
        state_timer += dt

        # State transitions
        if state_timer > random.uniform(state_duration_min_s, state_duration_max_s):
            state = random.choices(
                ['cruise', 'accelerate', 'decelerate', 'regen', 'idle'],
                weights=discharge_state_weights,
            )[0]
            state_timer = 0.0

        # Current target per state
        if state == 'idle':
            target_current = idle_current
        elif state == 'accelerate':
            target_current = random.uniform(accelerate_current_min, accelerate_current_max)
        elif state == 'cruise':
            target_current = random.uniform(cruise_current_min, cruise_current_max)
        elif state == 'decelerate':
            target_current = random.uniform(decelerate_current_min, decelerate_current_max)
        elif state == 'regen':
            target_current = random.uniform(regen_current_min, regen_current_max)
        else:
            target_current = idle_current

        current = _apply_slew(current, target_current, max_slew)
        yield (round(t, 2), round(current, 2))
        t += dt


def generate_charge_cycle(
    duration_s: int,
    dt: float = DEFAULT_DT_S,
    I_cc: float = 30.0,
    slew_rate: float = 5.0,
) -> Iterator[tuple[float, float]]:
    """
    Generate a constant-current charging profile (positive current = charge).

    Yields (time_s, current_a) one sample at a time.
    """
    steps    = int(duration_s / dt)
    t        = 0.0
    current  = 0.0
    max_slew = slew_rate * dt

    for _ in range(steps):
        current = _apply_slew(current, I_cc, max_slew)
        yield (round(t, 2), round(current, 2))
        t += dt


def generate_mixed_cycles(
    n_cycles: int,
    dt: float = DEFAULT_DT_S,
    # Charge sub-cycle parameters (shared across all charge cycles)
    I_cc: float = 30.0,
    slew_rate: float = 5.0,
    # Duration distributions for sub-cycles
    discharge_duration_mean_s: float = DEFAULT_MIXED_DISCHARGE_DURATION_MEAN_S,
    discharge_duration_std_s: float = DEFAULT_MIXED_DISCHARGE_DURATION_STD_S,
    charge_duration_mean_s: float = DEFAULT_MIXED_CHARGE_DURATION_MEAN_S,
    charge_duration_std_s: float = DEFAULT_MIXED_CHARGE_DURATION_STD_S,
    # Rest period distribution "a" between sub-cycles
    rest_duration_mean_s: float = DEFAULT_MIXED_REST_DURATION_MEAN_S,
    rest_duration_std_s: float = DEFAULT_MIXED_REST_DURATION_STD_S,
    # Rest period distribution "b" — selected per-occurrence via rest_duration_pattern
    rest_duration_mean_s_b: float = DEFAULT_MIXED_REST_DURATION_MEAN_S,
    rest_duration_std_s_b: float = DEFAULT_MIXED_REST_DURATION_STD_S,
    # Sequence of 'a'/'b' picking which rest distribution to draw from, one entry
    # per rest actually emitted, cycling (index % len(pattern)) once exhausted
    rest_duration_pattern: list = None,
    # Which sub-cycle phases get a trailing rest period
    rest_after_charge: bool = True,
    rest_after_discharge: bool = True,
    # Which phase starts the sequence
    first_phase: str = DEFAULT_MIXED_FIRST_PHASE,
    # Discharge cycle distribution parameters (forwarded to generate_discharge_cycle)
    **discharge_kwargs,
) -> Iterator[tuple[float, float]]:
    """
    Concatenate n_cycles charge/discharge pairs separated by rest periods.

    Yields (time_s, current_a) one sample at a time: sub-cycles are consumed
    lazily and shifted onto the running time offset as they are produced, so a
    5000-cycle profile costs the same memory as a 5-cycle one.

    Every sub-cycle runs to its full drawn length: the profile is built from a
    cycle count rather than a time budget, so no sub-cycle is ever truncated and
    the total duration is an outcome of the parameters rather than an input.

    Sub-cycle durations are drawn from N(mean, std), floored at MIN_SUB_DURATION_S.
    Rest durations are drawn from distribution 'a' (rest_duration_mean_s/std_s) or
    'b' (rest_duration_mean_s_b/std_s_b), floored at 0, selected per rest
    occurrence by cycling through rest_duration_pattern (default ['a']).
    All charge sub-cycles share the same parameters except their duration.
    A rest is only inserted after a sub-cycle whose phase has its
    rest_after_charge / rest_after_discharge flag set to True.
    """
    rest_dists = {
        'a': (rest_duration_mean_s, rest_duration_std_s),
        'b': (rest_duration_mean_s_b, rest_duration_std_s_b),
    }
    pattern = rest_duration_pattern or ['a']
    t_offset = 0.0
    phase = first_phase
    rest_count = 0

    # One cycle is a charge sub-cycle plus a discharge sub-cycle, ordered by first_phase
    for _ in range(2 * n_cycles):

        # ---- Sub cycle generation ----

        if phase == 'discharge':
            raw_duration = random.gauss(discharge_duration_mean_s, discharge_duration_std_s)
            duration_s   = int(max(MIN_SUB_DURATION_S, raw_duration))
            sub_cycle    = generate_discharge_cycle(duration_s, dt, **discharge_kwargs)
        else:
            raw_duration = random.gauss(charge_duration_mean_s, charge_duration_std_s)
            duration_s   = int(max(MIN_SUB_DURATION_S, raw_duration))
            sub_cycle    = generate_charge_cycle(
                duration_s, dt,
                I_cc=I_cc,
                slew_rate=slew_rate,
            )

        for t_s, i_a in sub_cycle:
            yield (round(t_offset + t_s, 2), i_a)

        t_offset += duration_s
        completed_phase = phase
        phase = 'charge' if phase == 'discharge' else 'discharge'

        # ---- Rest period generation ----

        want_rest = (completed_phase == 'charge' and rest_after_charge) or \
                    (completed_phase == 'discharge' and rest_after_discharge)

        if want_rest:
            # Rest period (zero current) — distribution cycles through rest_duration_pattern
            mean_s, std_s     = rest_dists[pattern[rest_count % len(pattern)]]
            raw_rest_duration = random.gauss(mean_s, std_s)
            rest_s            = max(0.0, raw_rest_duration)
            rest_steps        = int(rest_s / dt)
            rest_count       += 1

            for i in range(rest_steps):
                yield (round(t_offset + i * dt, 2), 0.0)
            t_offset += rest_steps * dt


# ---- ECM simulation to get current corresponding voltage and SoC ----

def simulate(capacity_ah: float, duration_s: int, initial_soc: float = 90.0,
             dt: float = DEFAULT_DT_S, noise_sigma_mv: float = 5.0,
             profile_mode: str = 'discharge', n_cycles: int = DEFAULT_MIXED_CYCLES,
             seed: int = 42, **charge_kwargs) -> Iterator[dict]:
    """
    Run full ECM simulation, yielding one record per sample.

    Consumes the current profile lazily and yields each record as it is computed,
    so the caller can write results away and never hold the whole run in memory.

    Measurement noise is drawn from a private generator rather than the global one
    the profile generators use. Lazy generation interleaves profile draws with
    noise draws, and a single shared stream would make the current profile depend
    on how many noise samples had been consumed so far, so a given seed would no
    longer reproduce the sub-cycle durations it used to.

    duration_s sizes the single-phase profiles ('discharge' and 'charge'), while
    the mixed profile is sized by n_cycles instead and runs for however long its
    sub-cycles and rests take.
    """
    noise_rng = random.Random(f"{seed}-measurement-noise")
    _MIXED_KEYS = {
        'discharge_duration_mean_s', 'discharge_duration_std_s',
        'charge_duration_mean_s', 'charge_duration_std_s',
        'rest_duration_mean_s', 'rest_duration_std_s',
        'rest_duration_mean_s_b', 'rest_duration_std_s_b', 'rest_duration_pattern',
        'first_phase', 'rest_after_charge', 'rest_after_discharge',
    }
    _DISCHARGE_KEYS = {
        'discharge_slew_rate', 'state_duration_min_s', 'state_duration_max_s',
        'discharge_state_weights', 'idle_current',
        'accelerate_current_min', 'accelerate_current_max',
        'cruise_current_min', 'cruise_current_max',
        'decelerate_current_min', 'decelerate_current_max',
        'regen_current_min', 'regen_current_max',
    }
    mixed_kwargs     = {k: v for k, v in charge_kwargs.items() if k in _MIXED_KEYS}
    discharge_kwargs = {k: v for k, v in charge_kwargs.items() if k in _DISCHARGE_KEYS}
    charge_kwargs    = {k: v for k, v in charge_kwargs.items() if k not in _MIXED_KEYS and k not in _DISCHARGE_KEYS}

    if profile_mode == 'discharge':
        profile = generate_discharge_cycle(duration_s, dt, **discharge_kwargs)
    elif profile_mode == 'mixed':
        profile = generate_mixed_cycles(n_cycles, dt, **mixed_kwargs, **charge_kwargs, **discharge_kwargs)
    else:  # CC charge
        profile = generate_charge_cycle(duration_s, dt, **charge_kwargs)

    soc   = initial_soc
    v_rc  = 0.0
    tau   = R1 * C1
    alpha = math.exp(-dt / tau)

    for t_s, current_a in profile:
        # SoC update (Coulomb Counting — ground truth)
        eta       = 0.999 if current_a >= 0 else 1.0
        delta_soc = (current_a * dt * eta) / (3600.0 * capacity_ah) * 100.0
        soc       = max(0.0, min(100.0, soc + delta_soc))

        # RC voltage update
        v_rc = alpha * v_rc + R1 * (1.0 - alpha) * current_a

        # Terminal voltage (mV) with Gaussian noise
        ocv_mv       = ocv_from_soc(soc)
        v_terminal_mv = ocv_mv + v_rc * 1000.0 + R0 * current_a * 1000.0
        v_terminal_mv += noise_rng.gauss(0.0, noise_sigma_mv)

        yield {
            'time_s':       t_s,
            'current_a':    round(current_a, 3),
            'voltage_mv':   round(v_terminal_mv, 2),
            'true_soc_pct': round(soc, 4)
        }

        if profile_mode == 'discharge' and soc <= 0.5:
            print(f"[INFO] Battery depleted at t={t_s:.1f}s — stopping simulation.")
            break
        elif profile_mode == 'charge' and soc >= 99.5:
            print(f"[INFO] Battery fully charged at t={t_s:.1f}s — stopping simulation.")
            break


def _validate_args(parser, args):
    """
    Reject parameter values the generators cannot honour.

    argparse already covers types and choices; these are the constraints that
    would otherwise be silently clamped inside the generators and quietly produce
    a profile different from the one requested.
    """
    if args.capacity <= 0.0:
        parser.error(f'--capacity must be > 0, got {args.capacity}')

    if not 0.0 <= args.initial_soc <= 100.0:
        parser.error(f'--initial-soc must lie within [0, 100], got {args.initial_soc}')

    if args.profile == 'mixed' and args.n_cycles < 1:
        parser.error(f'--n-cycles must be >= 1, got {args.n_cycles}')

    if args.dt <= 0.0:
        parser.error(f'--dt must be > 0, got {args.dt}')

    # A sub-cycle shorter than one sampling period would emit no samples at all
    if args.dt > MIN_SUB_DURATION_S:
        parser.error(f'--dt must be <= {MIN_SUB_DURATION_S} s, the shortest sub-cycle '
                     f'the generator emits, got {args.dt}')

    if args.chunk_rows < 1:
        parser.error(f'--chunk-rows must be >= 1, got {args.chunk_rows}')

    if args.plot_max_points < 1:
        parser.error(f'--plot-max-points must be >= 1, got {args.plot_max_points}')

    # Sub-cycle means below the floor would be raised to it without notice
    for name, value in (('--charge-duration-mean',    args.charge_duration_mean),
                        ('--discharge-duration-mean', args.discharge_duration_mean)):
        if value < MIN_SUB_DURATION_S:
            parser.error(f'{name} must be >= {MIN_SUB_DURATION_S} s, the shortest '
                         f'sub-cycle the generator emits, got {value}')

    # Negative rest means would be floored to zero, dropping the rest entirely
    for name, value in (('--rest-duration-mean',   args.rest_duration_mean),
                        ('--rest-duration-mean-b', args.rest_duration_mean_b)):
        if value < 0.0:
            parser.error(f'{name} must be >= 0, got {value}')

    for name, value in (('--charge-duration-std',    args.charge_duration_std),
                        ('--discharge-duration-std', args.discharge_duration_std),
                        ('--rest-duration-std',      args.rest_duration_std),
                        ('--rest-duration-std-b',    args.rest_duration_std_b)):
        if value < 0.0:
            parser.error(f'{name} must be >= 0, got {value}')

    # A rest pattern is meaningless when neither phase is allowed a trailing rest
    if (args.profile == 'mixed'
            and not (args.rest_after_charge or args.rest_after_discharge)
            and args.rest_duration_pattern != ['a']):
        parser.error('--rest-duration-pattern conflicts with --no-rest-after-charge '
                     'combined with --no-rest-after-discharge, which suppress every rest')


def main():
    parser = argparse.ArgumentParser(description='Li-Ion Cell Simulator')
    parser.add_argument('--capacity',    type=float, default=BMS_CELL_CAPACITY_INI_AH,
                        help='Nominal capacity [Ah]')
    parser.add_argument('--n-cycles',    type=int,   default=DEFAULT_MIXED_CYCLES,
                        help='Charge/discharge pairs to generate (mixed profile). The total '
                             'duration follows from this and the sub-cycle/rest durations')
    parser.add_argument('--initial-soc', type=float, default=0.0,
                        help='Initial SoC [%%]')
    parser.add_argument('--dt',          type=float, default=DEFAULT_DT_S,
                        help='Sampling period [s]; sample count scales as 1/dt')
    parser.add_argument('--chunk-rows',  type=int,   default=DEFAULT_CHUNK_ROWS,
                        help='Records buffered in RAM before each flush to the CSV')
    parser.add_argument('--plot-max-points', type=int, default=DEFAULT_PLOT_MAX_POINTS,
                        help='Max samples kept for the plot; the curve is decimated '
                             'on the fly past this, while the CSV keeps every sample')
    parser.add_argument('--seed',        type=int,   default=42,
                        help='Random seed for reproducibility')
    parser.add_argument('--profile',     type=str,   default='mixed',
                        choices=['discharge', 'charge', 'mixed'],
                        help='Current profile: discharge, CC charge, or mixed')

    # Charge-specific arguments (used when --profile is charge or mixed)
    parser.add_argument('--charge-current', type=float, default=4.2,
                        help='Charge current [A]')
    parser.add_argument('--slew-rate',      type=float, default=CHARGE_MAX_SLEW,
                        help='Charge current ramp rate [A/s]')

    # Discharge-specific arguments (used when --profile is discharge or mixed)
    parser.add_argument('--discharge-slew-rate',          type=float, default=DISCHARGE_MAX_SLEW,
                        help='Discharge current ramp rate [A/s]')
    parser.add_argument('--discharge-state-duration-min', type=float, default=30.0,
                        help='Min duration of each discharge state [s]')
    parser.add_argument('--discharge-state-duration-max', type=float, default=300.0,
                        help='Max duration of each discharge state [s]')
    parser.add_argument('--discharge-state-weights',      type=float, nargs=5,
                        default=DISCHARGE_STATE_WEIGHTS,
                        metavar=('CRUISE', 'ACCEL', 'DECEL', 'REGEN', 'IDLE'),
                        help='Relative weights for discharge states [cruise accel decel regen idle]')
    parser.add_argument('--discharge-idle-current',       type=float, default=-2.0,
                        help='Target current in idle state [A]')
    parser.add_argument('--discharge-accelerate-min',     type=float, default=-80.0,
                        help='Min current in accelerate state [A]')
    parser.add_argument('--discharge-accelerate-max',     type=float, default=-40.0,
                        help='Max current in accelerate state [A]')
    parser.add_argument('--discharge-cruise-min',         type=float, default=-30.0,
                        help='Min current in cruise state [A]')
    parser.add_argument('--discharge-cruise-max',         type=float, default=-15.0,
                        help='Max current in cruise state [A]')
    parser.add_argument('--discharge-decelerate-min',     type=float, default=-15.0,
                        help='Min current in decelerate state [A]')
    parser.add_argument('--discharge-decelerate-max',     type=float, default=-5.0,
                        help='Max current in decelerate state [A]')
    parser.add_argument('--discharge-regen-min',          type=float, default=10.0,
                        help='Min current in regen state [A]')
    parser.add_argument('--discharge-regen-max',          type=float, default=40.0,
                        help='Max current in regen state [A]')

    # Mixed-profile arguments (used when --profile is mixed)
    parser.add_argument('--discharge-duration-mean', type=float,
                        default=DEFAULT_MIXED_DISCHARGE_DURATION_MEAN_S,
                        help='Mean discharge sub-cycle duration [s]')
    parser.add_argument('--discharge-duration-std',  type=float,
                        default=DEFAULT_MIXED_DISCHARGE_DURATION_STD_S,
                        help='Std dev of discharge sub-cycle duration [s]')
    parser.add_argument('--charge-duration-mean',    type=float,
                        default=DEFAULT_MIXED_CHARGE_DURATION_MEAN_S,
                        help='Mean charge sub-cycle duration [s]')
    parser.add_argument('--charge-duration-std',     type=float,
                        default=DEFAULT_MIXED_CHARGE_DURATION_STD_S,
                        help='Std dev of charge sub-cycle duration [s]')
    parser.add_argument('--rest-duration-mean',      type=float,
                        default=DEFAULT_MIXED_REST_DURATION_MEAN_S,
                        help='Mean rest period duration between sub-cycles [s]')
    parser.add_argument('--rest-duration-std',       type=float,
                        default=DEFAULT_MIXED_REST_DURATION_STD_S,
                        help='Std dev of rest period duration [s] (distribution "a")')
    parser.add_argument('--rest-duration-mean-b',    type=float,
                        default=DEFAULT_MIXED_REST_DURATION_MEAN_S,
                        help='Mean rest period duration [s] (distribution "b")')
    parser.add_argument('--rest-duration-std-b',     type=float,
                        default=DEFAULT_MIXED_REST_DURATION_STD_S,
                        help='Std dev of rest period duration [s] (distribution "b")')
    parser.add_argument('--rest-duration-pattern',   type=str, nargs='+',
                        default=['a'], choices=['a', 'b'], metavar='{a,b}',
                        help='Sequence of "a"/"b" picking which rest distribution to draw '
                             'from, one entry per rest actually emitted, cycling once '
                             'exhausted (default: always "a")')
    parser.add_argument('--rest-after-charge',       action=argparse.BooleanOptionalAction,
                        default=True,
                        help='Insert a rest period after each charge sub-cycle')
    parser.add_argument('--rest-after-discharge',    action=argparse.BooleanOptionalAction,
                        default=True,
                        help='Insert a rest period after each discharge sub-cycle')
    parser.add_argument('--first-phase',             type=str,
                        default=DEFAULT_MIXED_FIRST_PHASE,
                        choices=['discharge', 'charge'],
                        help='First sub-cycle phase in mixed profile')

    args = parser.parse_args()
    _validate_args(parser, args)
    random.seed(args.seed)

    output_path, plot_path = _next_output_paths()

    # The mixed profile is sized by its cycle count, so a duration only has to be
    # supplied for the single-phase profiles, which run for one sub-cycle each
    duration_s = int(args.charge_duration_mean if args.profile == 'charge'
                     else args.discharge_duration_mean)

    sizing = (f"Cycles={args.n_cycles}" if args.profile == 'mixed'
              else f"Duration={duration_s}s")
    print(f"[INFO] Profile={args.profile} | {sizing} | "
          f"Capacity={args.capacity}Ah | InitialSoC={args.initial_soc}%")

    # ---- Simulation (cell behaviour curve generation) ----

    records = simulate(
        args.capacity, duration_s, args.initial_soc,
        dt=args.dt,
        profile_mode=args.profile,
        n_cycles=args.n_cycles,
        seed=args.seed,
        # charge params (charge and mixed)
        I_cc=args.charge_current,
        slew_rate=args.slew_rate,
        # mixed-specific params (ignored by other profile modes)
        discharge_duration_mean_s=args.discharge_duration_mean,
        discharge_duration_std_s=args.discharge_duration_std,
        charge_duration_mean_s=args.charge_duration_mean,
        charge_duration_std_s=args.charge_duration_std,
        rest_duration_mean_s=args.rest_duration_mean,
        rest_duration_std_s=args.rest_duration_std,
        rest_duration_mean_s_b=args.rest_duration_mean_b,
        rest_duration_std_s_b=args.rest_duration_std_b,
        rest_duration_pattern=args.rest_duration_pattern,
        rest_after_charge=args.rest_after_charge,
        rest_after_discharge=args.rest_after_discharge,
        first_phase=args.first_phase,
        # discharge distribution params (discharge and mixed)
        discharge_slew_rate=args.discharge_slew_rate,
        state_duration_min_s=args.discharge_state_duration_min,
        state_duration_max_s=args.discharge_state_duration_max,
        discharge_state_weights=args.discharge_state_weights,
        idle_current=args.discharge_idle_current,
        accelerate_current_min=args.discharge_accelerate_min,
        accelerate_current_max=args.discharge_accelerate_max,
        cruise_current_min=args.discharge_cruise_min,
        cruise_current_max=args.discharge_cruise_max,
        decelerate_current_min=args.discharge_decelerate_min,
        decelerate_current_max=args.discharge_decelerate_max,
        regen_current_min=args.discharge_regen_min,
        regen_current_max=args.discharge_regen_max,
    )

    # ---- Output CSV generation ----

    # The records generator is consumed once, here: each record is buffered, flushed
    # to disk in chunks and then dropped, so only chunk_rows records plus the
    # decimated plot arrays stay resident. Run length is bounded by disk, not RAM.
    chunk       = []
    total_rows  = 0
    last_record = None

    # Plot decimation state — see the adaptive halving below
    plot_stride = 1
    plot_time, plot_current, plot_voltage, plot_soc = [], [], [], []

    with open(output_path, 'w', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=CSV_FIELDNAMES)
        writer.writeheader()

        for sample_index, record in enumerate(records):
            chunk.append(record)
            if len(chunk) >= args.chunk_rows:
                writer.writerows(chunk)
                total_rows += len(chunk)
                chunk.clear()

            # Keep every plot_stride-th sample; once the kept set outgrows twice the
            # budget, drop every other one and double the stride. Bounds the plot
            # arrays without needing the total sample count up front, which the
            # generator cannot supply, and keeps the retained samples evenly spaced.
            if sample_index % plot_stride == 0:
                plot_time.append(record['time_s'] / 60.0)  # minutes for plotting
                plot_current.append(record['current_a'])
                plot_voltage.append(record['voltage_mv'])
                plot_soc.append(record['true_soc_pct'])

                if len(plot_time) >= 2 * args.plot_max_points:
                    del plot_time[1::2]
                    del plot_current[1::2]
                    del plot_voltage[1::2]
                    del plot_soc[1::2]
                    plot_stride *= 2

            last_record = record

        # Trailing partial chunk
        if chunk:
            writer.writerows(chunk)
            total_rows += len(chunk)
            chunk.clear()

    print(f"[INFO] Written {total_rows} samples → {output_path}")
    if plot_stride > 1:
        print(f"[INFO] Plot decimated to {len(plot_time)} points "
              f"(every {plot_stride}th sample); the CSV keeps every sample.")

    final_soc = last_record['true_soc_pct']
    delta_soc = final_soc - args.initial_soc
    sign      = '+' if delta_soc >= 0 else ''
    print(f"[INFO] Final SoC: {final_soc:.2f}%  |  "
          f"ΔSoC: {sign}{delta_soc:.2f}%  |  "
          f"Energy: {abs(delta_soc) / 100.0 * args.capacity:.2f} Ah")

    # ---- Plot ----

    # Series were accumulated during the write loop above, already decimated

    fig, axes = plt.subplots(3, 1, figsize=(12, 8), sharex=True)
    fig.suptitle(f"Simulated cell behaviour — {output_path.stem}", fontsize=11)

    axes[0].plot(plot_time, plot_current, linewidth=0.8)
    axes[0].set_ylabel("Current (A)")
    axes[0].axhline(0, color='k', linewidth=0.4, linestyle='--')
    axes[0].grid(True, linewidth=0.3)

    axes[1].plot(plot_time, plot_voltage, color='tab:orange', linewidth=0.8)
    axes[1].set_ylabel("Voltage (mV)")
    axes[1].grid(True, linewidth=0.3)

    axes[2].plot(plot_time, plot_soc, color='tab:green', linewidth=0.8)
    axes[2].set_ylabel("True SoC (%)")
    axes[2].set_xlabel("Time (min)")
    axes[2].grid(True, linewidth=0.3)

    plt.tight_layout()
    plt.savefig(plot_path, dpi=150)
    print(f"[INFO] Plot saved → {plot_path}")
    plt.show()


if __name__ == '__main__':
    main()
