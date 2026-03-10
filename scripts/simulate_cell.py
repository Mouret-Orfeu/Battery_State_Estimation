#!/usr/bin/env python3
"""
simulate_cell.py — Li-Ion Cell Simulator
Generates a synthetic drive-cycle current profile and true SoC trajectory.

Usage:
    python3 simulate_cell.py --capacity 60 --duration 3600 --output test_vectors.csv

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


def generate_drive_cycle(duration_s: int, dt: float = 0.1) -> list:
    """
    Generate a synthetic WLTP-inspired current profile.
    Positive = charge (regen), Negative = discharge (driving).
    """
    steps = int(duration_s / dt)
    profile = []
    t = 0.0

    # State machine: idle, accelerate, cruise, decelerate, regen
    state = 'cruise'
    state_timer = 0.0
    current = -10.0  # Start with mild discharge

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

        # Slew rate limit: 20 A/s
        max_slew = 20.0 * dt
        if (target_current - current) > max_slew:
            current += max_slew
        elif (target_current - current) < -max_slew:
            current -= max_slew
        else:
            current = target_current

        profile.append((round(t, 2), round(current, 2)))
        t += dt

    return profile


def simulate(capacity_ah: float, duration_s: int, initial_soc: float = 90.0,
             dt: float = 0.1, noise_sigma_mv: float = 5.0):
    """Run full ECM simulation and return list of records."""
    profile = generate_drive_cycle(duration_s, dt)

    soc = initial_soc
    v_rc = 0.0  # RC voltage state
    tau = R1 * C1
    alpha = math.exp(-dt / tau)
    records = []

    for t_s, current_a in profile:
        # SoC update (Coulomb Counting — ground truth)
        eta = 0.999 if current_a >= 0 else 1.0
        delta_soc = (current_a * dt * eta) / (3600.0 * capacity_ah) * 100.0
        soc = max(0.0, min(100.0, soc + delta_soc))

        # RC voltage update
        v_rc = alpha * v_rc + R1 * (1.0 - alpha) * current_a

        # Terminal voltage (mV) with Gaussian noise
        ocv_mv = ocv_from_soc(soc)
        v_terminal_mv = ocv_mv + v_rc * 1000.0 + R0 * current_a * 1000.0
        v_terminal_mv += random.gauss(0.0, noise_sigma_mv)

        records.append({
            'time_s':       t_s,
            'current_a':    round(current_a, 3),
            'voltage_mv':   round(v_terminal_mv, 2),
            'true_soc_pct': round(soc, 4)
        })

        # Stop if battery depleted
        if soc <= 0.5:
            print(f"[INFO] Battery depleted at t={t_s:.1f}s — stopping simulation.")
            break

    return records


def main():
    parser = argparse.ArgumentParser(description='Li-Ion Cell Simulator')
    parser.add_argument('--capacity', type=float, default=60.0, help='Nominal capacity [Ah]')
    parser.add_argument('--duration', type=int,   default=3600, help='Simulation duration [s]')
    parser.add_argument('--initial-soc', type=float, default=90.0, help='Initial SoC [%%]')
    parser.add_argument('--output', type=str, default='test_vectors.csv', help='Output CSV file')
    parser.add_argument('--seed', type=int, default=42, help='Random seed for reproducibility')
    args = parser.parse_args()

    random.seed(args.seed)

    print(f"[INFO] Simulating {args.duration}s drive cycle | "
          f"Capacity={args.capacity}Ah | InitialSoC={args.initial_soc}%")

    records = simulate(args.capacity, args.duration, args.initial_soc)

    with open(args.output, 'w', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=['time_s', 'current_a', 'voltage_mv', 'true_soc_pct'])
        writer.writeheader()
        writer.writerows(records)

    print(f"[INFO] Written {len(records)} samples → {args.output}")
    final_soc = records[-1]['true_soc_pct']
    print(f"[INFO] Final SoC: {final_soc:.2f}%  |  "
          f"Energy consumed: {(args.initial_soc - final_soc)/100.0 * args.capacity:.2f} Ah")


if __name__ == '__main__':
    main()
