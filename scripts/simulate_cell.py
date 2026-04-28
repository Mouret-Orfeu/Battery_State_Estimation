#!/usr/bin/env python3
"""
simulate_cell.py — Li-Ion Cell Simulator
Generates a synthetic current profile (charge, discharge, or mixed) and the
resulting voltage and true SoC trajectory via ECM simulation and coulomb counting.

Usage:
    python3 scripts/simulate_cell.py --duration 7200
    python3 scripts/simulate_cell.py --capacity 60 --duration 3600
    python3 scripts/simulate_cell.py --profile charge --initial-soc 20 --duration 3600

Output is always saved to docs/simulated_cell_behavior/simulated_cell_behavior_i.csv,
where i follows the highest existing index in that folder.

Outputs a CSV with columns:
    time_s, current_a, voltage_mv, true_soc_pct

Author: Kamal Kadakara
"""

import argparse
import math
import csv
import random
import re
import pathlib
import matplotlib.pyplot as plt

_BASE_DIR  = pathlib.Path(__file__).resolve().parent.parent / "docs" / "simulated_cell_behavior"
CSV_DIR    = _BASE_DIR / "csv"
PLOTS_DIR  = _BASE_DIR / "plots"

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

# Default duration for the main simulation
DEFAULT_SIMULATION_DURATION_S = 60*60*2  # 2 hours

# Max per-step change in current during discharge and cahrge
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

# ---- ECM Parameters (NMC, 25°C) ----
R0 = 0.005   # Ohmic resistance [Ω]
R1 = 0.008   # RC resistance [Ω]
C1 = 1500.0  # RC capacitance [F]

# ---- Cell Parameters (must match bms_types.h / soc_ekf.c) ----
BMS_CELL_CAPACITY_AH = 60.0

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
def generate_discharge_cycle(duration_s: int, dt: float = 0.1) -> list:
    """
    Generate a synthetic WLTP-inspired current profile.
    Positive = charge (regen), Negative = discharge (driving).
    """
    steps = int(duration_s / dt)
    profile = []
    t = 0.0

    state = 'cruise'
    state_timer = 0.0
    current = -10.0     # Start with mild discharge
    max_slew = DISCHARGE_MAX_SLEW * dt

    for _ in range(steps):
        state_timer += dt

        # State transitions
        if state_timer > DEFAULT_DISCHARGE_STATE_DURATION_DISTRIB():
            state = random.choices(
                ['cruise', 'accelerate', 'decelerate', 'regen', 'idle'],
                weights= DISCHARGE_STATE_WEIGHTS
            )[0]
            state_timer = 0.0

        # Current target per state
        if state == 'idle':
            target_current = -2.0
        elif state == 'accelerate':
            target_current = random.uniform(-80.0, -40.0)
        elif state == 'cruise':
            target_current = random.uniform(-30.0, -15.0)
        elif state == 'decelerate':
            target_current = random.uniform(-15.0, -5.0)
        elif state == 'regen':
            target_current = random.uniform(10.0, 40.0)
        else:
            target_current = -10.0

        current = _apply_slew(current, target_current, max_slew)
        profile.append((round(t, 2), round(current, 2)))
        t += dt

    return profile


def generate_charge_cycle(
    duration_s: int,
    dt: float = 0.1,
    I_cc: float = 30.0,
    slew_rate: float = 5.0,
) -> list:
    """Generate a constant-current charging profile (positive current = charge)."""
    steps    = int(duration_s / dt)
    profile  = []
    t        = 0.0
    current  = 0.0
    max_slew = slew_rate * dt

    for _ in range(steps):
        current = _apply_slew(current, I_cc, max_slew)
        profile.append((round(t, 2), round(current, 2)))
        t += dt

    return profile


def generate_mixed_cycles(
    total_duration_s: int,
    dt: float = 0.1,
    # Charge sub-cycle parameters (shared across all charge cycles)
    I_cc: float = 30.0,
    slew_rate: float = 5.0,
    # Duration distributions for sub-cycles
    discharge_duration_mean_s: float = DEFAULT_MIXED_DISCHARGE_DURATION_MEAN_S,
    discharge_duration_std_s: float = DEFAULT_MIXED_DISCHARGE_DURATION_STD_S,
    charge_duration_mean_s: float = DEFAULT_MIXED_CHARGE_DURATION_MEAN_S,
    charge_duration_std_s: float = DEFAULT_MIXED_CHARGE_DURATION_STD_S,
    # Rest period distribution between sub-cycles
    rest_duration_mean_s: float = DEFAULT_MIXED_REST_DURATION_MEAN_S,
    rest_duration_std_s: float = DEFAULT_MIXED_REST_DURATION_STD_S,
    # Which phase starts the sequence
    first_phase: str = DEFAULT_MIXED_FIRST_PHASE,
) -> list:
    """
    Concatenate alternating discharge and charge sub-cycles separated by rest
    periods, until total_duration_s is reached.

    Sub-cycle durations are drawn from N(mean, std), clamped to [10 s, remaining].
    Rest durations are drawn from N(mean, std), clamped to [0, remaining].
    All charge sub-cycles share the same parameters except their duration.
    """
    MIN_SUB_DURATION_S = 10
    profile = []
    t_offset = 0.0
    phase = first_phase

    while t_offset < total_duration_s:
        remaining = total_duration_s - t_offset

        # ---- Sub cycle generation ----

        if phase == 'discharge':
            raw_duration = random.gauss(discharge_duration_mean_s, discharge_duration_std_s)
            duration_s   = int(max(MIN_SUB_DURATION_S, min(remaining, raw_duration)))
            sub_cycle     = generate_discharge_cycle(duration_s, dt)
        else:
            raw_duration = random.gauss(charge_duration_mean_s, charge_duration_std_s)
            duration_s   = int(max(MIN_SUB_DURATION_S, min(remaining, raw_duration)))
            sub_cycle     = generate_charge_cycle(
                duration_s, dt,
                I_cc=I_cc,
                slew_rate=slew_rate,
            )

        # ---- End of simulation detection ----    

        for t_s, i_a in sub_cycle:
            t_abs = round(t_offset + t_s, 2)
            if t_abs >= total_duration_s:
                break
            profile.append((t_abs, i_a))

        t_offset += duration_s
        phase = 'charge' if phase == 'discharge' else 'discharge'

        if t_offset >= total_duration_s:
            break

        # ---- Rest period generation ----   

        # Rest period (zero current)
        raw_rest_duration  = random.gauss(rest_duration_mean_s, rest_duration_std_s)
        rest_s    = max(0.0, min(total_duration_s - t_offset, raw_rest_duration))
        rest_steps = int(rest_s / dt)

        # ---- End of simulation detection ----   

        for i in range(rest_steps):
            t_abs = round(t_offset + i * dt, 2)
            if t_abs >= total_duration_s:
                break
            profile.append((t_abs, 0.0))
        t_offset += rest_steps * dt

    return profile


# ---- ECM simulation to get current corresponding voltage and SoC ----

def simulate(capacity_ah: float, duration_s: int, initial_soc: float = 90.0,
             dt: float = 0.1, noise_sigma_mv: float = 5.0,
             profile_mode: str = 'discharge', **charge_kwargs):
    """Run full ECM simulation and return list of records."""
    _MIXED_KEYS = {
        'discharge_duration_mean_s', 'discharge_duration_std_s',
        'charge_duration_mean_s', 'charge_duration_std_s',
        'rest_duration_mean_s', 'rest_duration_std_s', 'first_phase',
    }
    mixed_kwargs  = {k: v for k, v in charge_kwargs.items() if k in _MIXED_KEYS}
    charge_kwargs = {k: v for k, v in charge_kwargs.items() if k not in _MIXED_KEYS}

    if profile_mode == 'discharge':
        profile = generate_discharge_cycle(duration_s, dt)
    elif profile_mode == 'mixed':
        profile = generate_mixed_cycles(duration_s, dt, **mixed_kwargs, **charge_kwargs)
    else:  # CC charge
        profile = generate_charge_cycle(duration_s, dt, **charge_kwargs)

    soc   = initial_soc
    v_rc  = 0.0
    tau   = R1 * C1
    alpha = math.exp(-dt / tau)
    records = []

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
        v_terminal_mv += random.gauss(0.0, noise_sigma_mv)

        records.append({
            'time_s':       t_s,
            'current_a':    round(current_a, 3),
            'voltage_mv':   round(v_terminal_mv, 2),
            'true_soc_pct': round(soc, 4)
        })

        if profile_mode == 'discharge' and soc <= 0.5:
            print(f"[INFO] Battery depleted at t={t_s:.1f}s — stopping simulation.")
            break
        elif profile_mode == 'charge' and soc >= 99.5:
            print(f"[INFO] Battery fully charged at t={t_s:.1f}s — stopping simulation.")
            break

    return records


def main():
    parser = argparse.ArgumentParser(description='Li-Ion Cell Simulator')
    parser.add_argument('--capacity',    type=float, default=BMS_CELL_CAPACITY_AH,
                        help='Nominal capacity [Ah]')
    parser.add_argument('--duration',    type=int,   default=DEFAULT_SIMULATION_DURATION_S,
                        help='Simulation duration [s]')
    parser.add_argument('--initial-soc', type=float, default=90.0,
                        help='Initial SoC [%%]')
    parser.add_argument('--seed',        type=int,   default=42,
                        help='Random seed for reproducibility')
    parser.add_argument('--profile',     type=str,   default='mixed',
                        choices=['discharge', 'charge', 'mixed'],
                        help='Current profile: discharge, CC charge, or mixed')

    # Charge-specific arguments (used when --profile is charge or mixed)
    parser.add_argument('--charge-current', type=float, default=30.0,
                        help='Charge current [A]')
    parser.add_argument('--slew-rate',      type=float, default=CHARGE_MAX_SLEW,
                        help='Charge current ramp rate [A/s]')

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
                        help='Std dev of rest period duration [s]')
    parser.add_argument('--first-phase',             type=str,
                        default=DEFAULT_MIXED_FIRST_PHASE,
                        choices=['discharge', 'charge'],
                        help='First sub-cycle phase in mixed profile')

    args = parser.parse_args()
    random.seed(args.seed)

    output_path, plot_path = _next_output_paths()

    print(f"[INFO] Profile={args.profile} | Duration={args.duration}s | "
          f"Capacity={args.capacity}Ah | InitialSoC={args.initial_soc}%")

    # ---- Simulation (cell behaviour curve generation) ----

    records = simulate(
        args.capacity, args.duration, args.initial_soc,
        profile_mode=args.profile,
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
        first_phase=args.first_phase,
    )

    # ---- Output CSV generation ----

    with open(output_path, 'w', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=['time_s', 'current_a', 'voltage_mv', 'true_soc_pct'])
        writer.writeheader()
        writer.writerows(records)

    print(f"[INFO] Written {len(records)} samples → {output_path}")
    final_soc = records[-1]['true_soc_pct']
    delta_soc = final_soc - args.initial_soc
    sign      = '+' if delta_soc >= 0 else ''
    print(f"[INFO] Final SoC: {final_soc:.2f}%  |  "
          f"ΔSoC: {sign}{delta_soc:.2f}%  |  "
          f"Energy: {abs(delta_soc) / 100.0 * args.capacity:.2f} Ah")

    # ---- Plot ----

    time     = [r['time_s']       for r in records]
    current  = [r['current_a']    for r in records]
    voltage  = [r['voltage_mv']   for r in records]
    soc_hist = [r['true_soc_pct'] for r in records]

    fig, axes = plt.subplots(3, 1, figsize=(12, 8), sharex=True)
    fig.suptitle(f"Simulated cell behaviour — {output_path.stem}", fontsize=11)

    axes[0].plot(time, current, linewidth=0.8)
    axes[0].set_ylabel("Current (A)")
    axes[0].axhline(0, color='k', linewidth=0.4, linestyle='--')
    axes[0].grid(True, linewidth=0.3)

    axes[1].plot(time, voltage, color='tab:orange', linewidth=0.8)
    axes[1].set_ylabel("Voltage (mV)")
    axes[1].grid(True, linewidth=0.3)

    axes[2].plot(time, soc_hist, color='tab:green', linewidth=0.8)
    axes[2].set_ylabel("True SoC (%)")
    axes[2].set_xlabel("Time (s)")
    axes[2].grid(True, linewidth=0.3)

    plt.tight_layout()
    plt.savefig(plot_path, dpi=150)
    print(f"[INFO] Plot saved → {plot_path}")
    plt.show()


if __name__ == '__main__':
    main()
