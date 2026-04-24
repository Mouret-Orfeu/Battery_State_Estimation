#!/usr/bin/env python3
"""
simulate_cell.py — Li-Ion Cell Simulator
Generates a synthetic current profile (drive cycle or recharge) and the
resulting true SoC trajectory via ECM simulation.

Usage:
    python3 scripts/simulate_cell.py --capacity 60 --duration 3600 --output test_vectors.csv
    python3 scripts/simulate_cell.py --profile cc_cv --initial-soc 20 --duration 7200 --output charge.csv
    python3 scripts/simulate_cell.py --profile constant --initial-soc 20 --duration 3600 --output charge.csv

Outputs a CSV with columns:
    time_s, current_a, voltage_mv, true_soc_pct

Author: Kamal Kadakara
"""

import argparse
import math
import csv
import random

# ---- ECM Parameters (NMC, 25°C) ----
R0 = 0.005   # Ohmic resistance [Ω]
R1 = 0.008   # RC resistance [Ω]
C1 = 1500.0  # RC capacitance [F]

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

def generate_drive_cycle(duration_s: int, dt: float = 0.1) -> list:
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
    max_slew = 20.0 * dt

    for _ in range(steps):
        state_timer += dt

        # State transitions
        if state_timer > random.uniform(5.0, 30.0):
            state = random.choices(
                ['cruise', 'accelerate', 'decelerate', 'regen', 'idle'],
                weights=[40, 20, 20, 10, 10]
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
    mode: str = 'cc_cv',
    I_cc: float = 30.0,
    I_cutoff: float = 3.0,
    soc_cv_pct: float = 80.0,
    initial_soc_pct: float = 20.0,
    capacity_ah: float = 60.0,
    slew_rate: float = 5.0,
    tau_cv: float = 600.0,
) -> list:
    """
    Generate a charging current profile (positive current = charge).

    mode='constant': constant current I_cc for the full duration.
    mode='cc_cv':    CC phase at I_cc until an internal SoC estimate reaches
                     soc_cv_pct, then CV phase where the current decays
                     exponentially (time constant tau_cv) from I_cc down to
                     I_cutoff, at which point it is held flat.

    Args:
        duration_s:      Total profile duration [s].
        dt:              Time step [s].
        mode:            'cc_cv' or 'constant'.
        I_cc:            Charge current during CC phase [A].
        I_cutoff:        Termination current for CV phase [A].
        soc_cv_pct:      SoC at which CC→CV transition occurs [%].
        initial_soc_pct: Starting SoC for internal CC→CV bookkeeping [%].
        capacity_ah:     Nominal cell capacity [Ah].
        slew_rate:       Maximum current ramp rate [A/s].
        tau_cv:          CV phase exponential decay time constant [s].
    """
    if mode not in ('cc_cv', 'constant'):
        raise ValueError(f"Unknown mode '{mode}'. Use 'cc_cv' or 'constant'.")

    steps    = int(duration_s / dt)
    profile  = []
    t        = 0.0
    current  = 0.0
    max_slew = slew_rate * dt

    # Internal SoC estimate — used only to trigger the CC→CV transition.
    # Simple coulomb counting with η=1 (charge efficiency is near 1 anyway).
    soc      = initial_soc_pct
    in_cv    = False
    t_cv     = 0.0

    for _ in range(steps):
        if mode == 'constant':
            target_current = I_cc

        else:  # cc_cv
            if not in_cv:
                if soc >= soc_cv_pct:
                    in_cv = True
                    t_cv  = t
                target_current = I_cc
            else:
                decay          = math.exp(-(t - t_cv) / tau_cv)
                target_current = max(I_cutoff, I_cutoff + (I_cc - I_cutoff) * decay)

        current = _apply_slew(current, target_current, max_slew)

        # Advance internal SoC estimate
        soc += (current * dt) / (3600.0 * capacity_ah) * 100.0
        soc  = max(0.0, min(100.0, soc))

        profile.append((round(t, 2), round(current, 2)))
        t += dt

    return profile


# ---- ECM simulation ----

def simulate(capacity_ah: float, duration_s: int, initial_soc: float = 90.0,
             dt: float = 0.1, noise_sigma_mv: float = 5.0,
             profile_mode: str = 'drive', **charge_kwargs):
    """Run full ECM simulation and return list of records."""
    if profile_mode == 'drive':
        profile = generate_drive_cycle(duration_s, dt)
    else:
        profile = generate_charge_cycle(
            duration_s, dt,
            mode=profile_mode,
            initial_soc_pct=initial_soc,
            capacity_ah=capacity_ah,
            **charge_kwargs
        )

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

        if profile_mode == 'drive' and soc <= 0.5:
            print(f"[INFO] Battery depleted at t={t_s:.1f}s — stopping simulation.")
            break
        if profile_mode != 'drive' and soc >= 99.5:
            print(f"[INFO] Battery fully charged at t={t_s:.1f}s — stopping simulation.")
            break

    return records


def main():
    parser = argparse.ArgumentParser(description='Li-Ion Cell Simulator')
    parser.add_argument('--capacity',    type=float, default=60.0,
                        help='Nominal capacity [Ah]')
    parser.add_argument('--duration',    type=int,   default=3600,
                        help='Simulation duration [s]')
    parser.add_argument('--initial-soc', type=float, default=90.0,
                        help='Initial SoC [%%]')
    parser.add_argument('--output',      type=str,   default='test_vectors.csv',
                        help='Output CSV file')
    parser.add_argument('--seed',        type=int,   default=42,
                        help='Random seed for reproducibility')
    parser.add_argument('--profile',     type=str,   default='drive',
                        choices=['drive', 'cc_cv', 'constant'],
                        help='Current profile: drive cycle or recharge mode')

    # Charge-specific arguments (used when --profile is cc_cv or constant)
    parser.add_argument('--charge-current', type=float, default=30.0,
                        help='CC phase charge current [A]')
    parser.add_argument('--cutoff-current', type=float, default=3.0,
                        help='CV phase termination current [A]')
    parser.add_argument('--soc-cv',         type=float, default=80.0,
                        help='SoC threshold for CC→CV transition [%%]')
    parser.add_argument('--cv-tau',         type=float, default=600.0,
                        help='CV phase exponential decay time constant [s]')
    parser.add_argument('--slew-rate',      type=float, default=5.0,
                        help='Charge current ramp rate [A/s]')

    args = parser.parse_args()
    random.seed(args.seed)

    print(f"[INFO] Profile={args.profile} | Duration={args.duration}s | "
          f"Capacity={args.capacity}Ah | InitialSoC={args.initial_soc}%")

    records = simulate(
        args.capacity, args.duration, args.initial_soc,
        profile_mode=args.profile,
        I_cc=args.charge_current,
        I_cutoff=args.cutoff_current,
        soc_cv_pct=args.soc_cv,
        tau_cv=args.cv_tau,
        slew_rate=args.slew_rate,
    )

    with open(args.output, 'w', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=['time_s', 'current_a', 'voltage_mv', 'true_soc_pct'])
        writer.writeheader()
        writer.writerows(records)

    print(f"[INFO] Written {len(records)} samples → {args.output}")
    final_soc = records[-1]['true_soc_pct']
    delta_soc = final_soc - args.initial_soc
    sign      = '+' if delta_soc >= 0 else ''
    print(f"[INFO] Final SoC: {final_soc:.2f}%  |  "
          f"ΔSoC: {sign}{delta_soc:.2f}%  |  "
          f"Energy: {abs(delta_soc) / 100.0 * args.capacity:.2f} Ah")


if __name__ == '__main__':
    main()
