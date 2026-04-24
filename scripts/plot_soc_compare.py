#!/usr/bin/env python3
"""
plot_soc_compare.py — SoC Estimator Comparison Plot

Runs Coulomb Counting, OCV Lookup, and EKF against the ground-truth SoC
produced by simulate_cell.py and saves a three-panel comparison figure.

Usage:
    python3 scripts/plot_soc_compare.py --input docs/test_vectors.csv

Output: docs/soc_comparison.png  (configurable with --output)

Author: Orfeu Mouret
"""

import argparse
import csv
import math
import os

import matplotlib.pyplot as plt
import matplotlib.gridspec as gridspec

# ---- Cell & ECM parameters (must match bms_types.h / soc_ekf.c) ----
Q_NOM_AH = 60.0
ETA_CHARGE  = 0.999
ETA_DCHG = 1.0
R0       = 0.005    # Ohmic resistance [Ω]
R1_ECM   = 0.008    # RC resistance [Ω]
C1_ECM   = 1500.0   # RC capacitance [F]

# ---- EKF noise parameters (must match soc_ekf.c) ----
EKF_Q11 = 1e-6
EKF_Q22 = 1e-4
EKF_R   = 1e-2

# ---- OCV table (must match soc_ocv.c, 0–100% in 5% steps) ----
OCV_TABLE_MV = [
    3000, 3100, 3250, 3350, 3420, 3470, 3520, 3560,
    3600, 3630, 3660, 3690, 3720, 3760, 3800, 3850,
    3920, 4010, 4080, 4150, 4200,
]


# ---- OCV / SoC helpers ----

def ocv_from_soc(soc_pct: float) -> float:
    soc_pct = max(0.0, min(100.0, soc_pct))
    idx = int(soc_pct / 5.0)
    if idx >= 20:
        return float(OCV_TABLE_MV[20])
    frac = (soc_pct - idx * 5.0) / 5.0
    return OCV_TABLE_MV[idx] + frac * (OCV_TABLE_MV[idx + 1] - OCV_TABLE_MV[idx])


def soc_from_ocv(ocv_mv: float) -> float:
    if ocv_mv <= OCV_TABLE_MV[0]:
        return 0.0
    if ocv_mv >= OCV_TABLE_MV[-1]:
        return 100.0
    for i in range(len(OCV_TABLE_MV) - 1):
        if OCV_TABLE_MV[i] <= ocv_mv < OCV_TABLE_MV[i + 1]:
            frac = (ocv_mv - OCV_TABLE_MV[i]) / (OCV_TABLE_MV[i + 1] - OCV_TABLE_MV[i])
            return i * 5.0 + frac * 5.0
    return 100.0


# ---- Estimators ----

def run_coulomb(records: list, initial_soc: float, dt: float) -> list:
    """Coulomb counting: integrates current with coulombic efficiency."""
    soc = initial_soc
    result = []
    for rec in records:
        I   = rec['current_a']
        eta = ETA_CHARGE if I >= 0 else ETA_DCHG
        soc += (I * dt * eta) / (3600.0 * Q_NOM_AH) * 100.0
        soc  = max(0.0, min(100.0, soc))
        result.append(soc)
    return result


def run_ocv(records: list) -> list:
    """Instantaneous OCV lookup: voltage → SoC at every sample.
    Only physically valid at equilibrium; shown here to illustrate
    the transient bias during current flow."""
    return [soc_from_ocv(rec['voltage_mv']) for rec in records]


def run_ekf(records: list, initial_soc: float, dt: float) -> list:
    """1st-order ECM Extended Kalman Filter.
    State: x = [SoC (normalised 0–1), V_RC (V)]
    Matches the implementation in src/soc_ekf.c exactly."""
    x   = [initial_soc / 100.0, 0.0]
    P   = [[0.01, 0.0], [0.0, 0.01]]
    tau = R1_ECM * C1_ECM

    result = []
    for rec in records:
        I     = rec['current_a']
        v_meas = rec['voltage_mv']

        eta = ETA_CHARGE if I >= 0 else ETA_DCHG
        A22 = math.exp(-dt / tau)
        B1  = (dt * eta) / (3600.0 * Q_NOM_AH)
        B2  = R1_ECM * (1.0 - A22)

        # -- Prediction --
        xp = [
            min(1.0, max(0.0, x[0] + B1 * I)),
            A22 * x[1] + B2 * I,
        ]
        Pp = [
            [P[0][0] + EKF_Q11,          A22 * P[0][1]             ],
            [A22 * P[1][0],              A22 * A22 * P[1][1] + EKF_Q22],
        ]

        # -- Jacobian (numeric, dOCV_mV / d(x[0] normalised)) --
        dsoc = 0.001
        H0   = (ocv_from_soc((xp[0] + dsoc) * 100.0) -
                ocv_from_soc((xp[0] - dsoc) * 100.0)) / (2.0 * dsoc)
        H1   = 1000.0   # ∂V_terminal / ∂V_RC  [mV/V]

        # -- Innovation --
        y_pred = ocv_from_soc(xp[0] * 100.0) + xp[1] * 1000.0 + R0 * I * 1000.0
        innov  = v_meas - y_pred

        S  = (H0 * H0 * Pp[0][0]
            + H0 * H1 * (Pp[0][1] + Pp[1][0])
            + H1 * H1 * Pp[1][1]
            + EKF_R * 1e6)

        K0 = (H0 * Pp[0][0] + H1 * Pp[0][1]) / S
        K1 = (H0 * Pp[1][0] + H1 * Pp[1][1]) / S

        # -- Update --
        x[0] = min(1.0, max(0.0, xp[0] + K0 * innov))
        x[1] = xp[1] + K1 * innov

        P[0][0] = (1.0 - K0 * H0)   * Pp[0][0] - K0 * H1  * Pp[1][0]
        P[0][1] = (1.0 - K0 * H0)   * Pp[0][1] - K0 * H1  * Pp[1][1]
        P[1][0] = -K1 * H0 * Pp[0][0] + (1.0 - K1 * H1) * Pp[1][0]
        P[1][1] = -K1 * H0 * Pp[0][1] + (1.0 - K1 * H1) * Pp[1][1]

        result.append(x[0] * 100.0)

    return result


# ---- I/O ----

def load_csv(path: str) -> list:
    with open(path, newline='') as f:
        return [{
            'time_s':       float(row['time_s']),
            'current_a':    float(row['current_a']),
            'voltage_mv':   float(row['voltage_mv']),
            'true_soc_pct': float(row['true_soc_pct']),
        } for row in csv.DictReader(f)]


def rmse(errors: list) -> float:
    return math.sqrt(sum(e ** 2 for e in errors) / len(errors))


# ---- Main ----

def main():
    parser = argparse.ArgumentParser(description='SoC Estimator Comparison Plot')
    parser.add_argument('--input',  type=str, default='docs/test_vectors.csv',
                        help='Input CSV produced by simulate_cell.py')
    parser.add_argument('--output', type=str, default='docs/soc_comparison.png',
                        help='Output plot file')
    args = parser.parse_args()

    records  = load_csv(args.input)
    dt       = records[1]['time_s'] - records[0]['time_s'] if len(records) > 1 else 0.1
    t        = [r['time_s']       for r in records]
    true_soc = [r['true_soc_pct'] for r in records]
    current  = [r['current_a']    for r in records]

    # All estimators initialise from OCV lookup on the first voltage sample
    # (realistic: ignition-ON initialisation via OCV at near-rest conditions)
    soc0 = soc_from_ocv(records[0]['voltage_mv'])
    print(f"[INFO] Initial SoC from OCV: {soc0:.2f}%  |  True initial SoC: {true_soc[0]:.2f}%")

    soc_cc  = run_coulomb(records, soc0, dt)
    soc_ocv = run_ocv(records)
    soc_ekf = run_ekf(records, soc0, dt)

    err_cc  = [e - tr for e, tr in zip(soc_cc,  true_soc)]
    err_ocv = [e - tr for e, tr in zip(soc_ocv, true_soc)]
    err_ekf = [e - tr for e, tr in zip(soc_ekf, true_soc)]

    # ---- Plot ----
    fig = plt.figure(figsize=(12, 8))
    gs  = gridspec.GridSpec(3, 1, height_ratios=[3, 2, 1.5], hspace=0.45)

    ax_soc = fig.add_subplot(gs[0])
    ax_err = fig.add_subplot(gs[1], sharex=ax_soc)
    ax_cur = fig.add_subplot(gs[2], sharex=ax_soc)

    # SoC panel
    ax_soc.plot(t, true_soc, 'k-',  lw=1.8, label='True SoC',        zorder=4)
    ax_soc.plot(t, soc_ekf,  'r-',  lw=1.2, label='EKF',             zorder=5)
    ax_soc.plot(t, soc_cc,   'b--', lw=1.0, label='Coulomb Counting', zorder=3)
    ax_soc.plot(t, soc_ocv,  'g:',  lw=1.0, label='OCV Lookup',       zorder=2)
    ax_soc.set_ylabel('State of Charge (%)')
    ax_soc.set_title('SoC Estimation Comparison — Li-Ion NMC 60 Ah')
    ax_soc.legend(loc='upper right', fontsize=9)
    ax_soc.grid(True, alpha=0.3)
    ax_soc.set_ylim(0, 105)

    # Error panel
    ax_err.axhline(0, color='k', lw=0.8)
    ax_err.plot(t, err_ekf,  'r-',  lw=0.9, label='EKF')
    ax_err.plot(t, err_cc,   'b--', lw=0.9, label='Coulomb Counting')
    ax_err.plot(t, err_ocv,  'g:',  lw=0.9, label='OCV Lookup')
    ax_err.set_ylabel('SoC Error (%)')
    ax_err.legend(loc='upper right', fontsize=9)
    ax_err.grid(True, alpha=0.3)

    # Current panel
    ax_cur.plot(t, current, color='dimgray', lw=0.6)
    ax_cur.axhline(0, color='k', lw=0.5, ls='--')
    ax_cur.set_ylabel('Current (A)')
    ax_cur.set_xlabel('Time (s)')
    ax_cur.grid(True, alpha=0.3)

    plt.setp(ax_soc.get_xticklabels(), visible=False)
    plt.setp(ax_err.get_xticklabels(), visible=False)

    out_dir = os.path.dirname(args.output)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    fig.savefig(args.output, dpi=150, bbox_inches='tight')
    plt.close(fig)

    print(f"[INFO] Plot saved → {args.output}")
    print(f"[INFO] RMSE  Coulomb: {rmse(err_cc):.3f}%  |  "
          f"OCV: {rmse(err_ocv):.3f}%  |  "
          f"EKF: {rmse(err_ekf):.3f}%")


if __name__ == '__main__':
    main()
