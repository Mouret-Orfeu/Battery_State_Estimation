#!/usr/bin/env python3
"""
plot_soc_compare.py — SoC Estimator Comparison Plot

Calls the actual C implementations (src/soc_coulomb.c, src/soc_ocv.c,
src/soc_ekf.c) via the shared library built with 'make lib'.

Usage:
    make lib
    python3 scripts/plot_soc_compare.py \
        --input docs/simulated_cell_behavior/csv/simulated_cell_behavior_9.csv

Output: docs/soc_comparison.png  (configurable with --output)

Author: Orfeu Mouret
"""

import argparse
import csv
import ctypes
import math
import os
import pathlib
import subprocess

import matplotlib.pyplot as plt
import matplotlib.gridspec as gridspec

# ---- Paths ----
_REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
_LIB_PATH  = _REPO_ROOT / 'lib' / 'libbms.so'


# ---- ctypes struct definitions (must match bms_types.h exactly) ----

class BmsEcmParams(ctypes.Structure):
    _fields_ = [
        ('R0', ctypes.c_float),
        ('R1', ctypes.c_float),
        ('C1', ctypes.c_float),
    ]

class BmsSocState(ctypes.Structure):
    _fields_ = [
        ('soc_pct',          ctypes.c_float),
        ('soc_prev_pct',     ctypes.c_float),
        ('v_terminal_mv',    ctypes.c_float),
        ('current_a',        ctypes.c_float),
        ('temperature_degc', ctypes.c_float),
        ('is_initialised',   ctypes.c_bool),
    ]

class BmsEkfState(ctypes.Structure):
    _fields_ = [
        ('x', ctypes.c_float * 2),
        ('P', (ctypes.c_float * 2) * 2),
        ('Q', (ctypes.c_float * 2) * 2),
        ('R', ctypes.c_float),
    ]


# ---- Library loader ----

# It loads the shared library, and tells Python the C function signatures
def _load_lib() -> ctypes.CDLL:
    """Build lib/libbms.so if missing, then load and annotate it."""
    if not _LIB_PATH.exists():
        print('[INFO] lib/libbms.so not found — running "make lib"...')
        subprocess.run(['make', 'lib'], cwd=str(_REPO_ROOT), check=True)

    lib = ctypes.CDLL(str(_LIB_PATH))

    lib.SocCoulomb_Init.argtypes  = [ctypes.POINTER(BmsSocState), ctypes.c_float]
    lib.SocCoulomb_Init.restype   = None

    lib.SocCoulomb_Update.argtypes = [ctypes.POINTER(BmsSocState),
                                      ctypes.c_float, ctypes.c_float]
    lib.SocCoulomb_Update.restype  = ctypes.c_uint

    lib.SocOcv_LookupSoc.argtypes = [ctypes.c_float,
                                      ctypes.POINTER(ctypes.c_float)]
    lib.SocOcv_LookupSoc.restype  = ctypes.c_uint

    lib.SocOcv_GetOcv.argtypes = [ctypes.c_float]
    lib.SocOcv_GetOcv.restype  = ctypes.c_float

    lib.SocEkf_Init.argtypes = [ctypes.POINTER(BmsEkfState),
                                 ctypes.POINTER(BmsSocState),
                                 ctypes.POINTER(BmsEcmParams),
                                 ctypes.c_float]
    lib.SocEkf_Init.restype  = None

    lib.SocEkf_Update.argtypes = [ctypes.POINTER(BmsEkfState),
                                   ctypes.POINTER(BmsSocState),
                                   ctypes.c_float, ctypes.c_float, ctypes.c_float]
    lib.SocEkf_Update.restype  = ctypes.c_uint

    return lib


# ---- Estimator wrappers — call the actual C implementations ----

def run_coulomb(lib: ctypes.CDLL, records: list,
                initial_soc_pct: float, dt: float) -> list:
    state = BmsSocState()
    lib.SocCoulomb_Init(ctypes.byref(state), ctypes.c_float(initial_soc_pct))
    result = []
    for rec in records:
        lib.SocCoulomb_Update(ctypes.byref(state),
                              ctypes.c_float(rec['current_a']),
                              ctypes.c_float(dt))
        result.append(state.soc_pct)
    return result


def run_ocv(lib: ctypes.CDLL, records: list) -> list:
    soc_out = ctypes.c_float(0.0)
    result  = []
    for rec in records:
        lib.SocOcv_LookupSoc(ctypes.c_float(rec['voltage_mv']),
                              ctypes.byref(soc_out))
        result.append(soc_out.value)
    return result


def run_ekf(lib: ctypes.CDLL, records: list,
            initial_soc_pct: float, dt: float) -> list:
    ecm   = BmsEcmParams(R0=0.005, R1=0.008, C1=1500.0)
    state = BmsSocState()
    ekf   = BmsEkfState()
    lib.SocEkf_Init(ctypes.byref(ekf), ctypes.byref(state),
                    ctypes.byref(ecm), ctypes.c_float(initial_soc_pct))
    result = []
    for rec in records:
        lib.SocEkf_Update(ctypes.byref(ekf), ctypes.byref(state),
                          ctypes.c_float(rec['current_a']),
                          ctypes.c_float(rec['voltage_mv']),
                          ctypes.c_float(dt))
        result.append(state.soc_pct)
    return result


# ---- I/O helpers ----

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
    parser.add_argument(
        '--input', type=str,
        default='docs/simulated_cell_behavior/csv/simulated_cell_behavior_0.csv',
        help='Input CSV produced by simulate_cell.py',
    )
    parser.add_argument(
        '--output', type=str, default='docs/soc_comparison.png',
        help='Output plot file',
    )
    args = parser.parse_args()

    lib = _load_lib()

    records  = load_csv(args.input)
    # In case of very short data, assume 0.1s timestep to avoid errors
    dt       = records[1]['time_s'] - records[0]['time_s'] if len(records) > 1 else 0.1

    t        = [r['time_s'] / 60.0 for r in records] # Convert time to minutes for plotting
    true_soc = [r['true_soc_pct'] for r in records]
    current  = [r['current_a']    for r in records]

    # Initialise all estimators from OCV lookup on the first voltage sample
    soc0_c = ctypes.c_float(0.0)
    lib.SocOcv_LookupSoc(ctypes.c_float(records[0]['voltage_mv']),
                         ctypes.byref(soc0_c))
    soc0 = soc0_c.value
    print(f"[INFO] Initial SoC from OCV: {soc0:.2f}%  |  "
          f"True initial SoC: {true_soc[0]:.2f}%")

    soc_cc  = run_coulomb(lib, records, soc0, dt)
    soc_ocv = run_ocv(lib, records)
    soc_ekf = run_ekf(lib, records, soc0, dt)

    err_cc  = [estimate - truth for estimate, truth in zip(soc_cc,  true_soc)]
    err_ocv = [estimate - truth for estimate, truth in zip(soc_ocv, true_soc)]
    err_ekf = [estimate - truth for estimate, truth in zip(soc_ekf, true_soc)]

    # ---- Plot ----
    fig = plt.figure(figsize=(12, 8))
    gs  = gridspec.GridSpec(3, 1, height_ratios=[3, 2, 1.5], hspace=0.45)

    ax_soc = fig.add_subplot(gs[0])
    ax_err = fig.add_subplot(gs[1], sharex=ax_soc)
    ax_cur = fig.add_subplot(gs[2], sharex=ax_soc)

    ax_soc.plot(t, true_soc, 'k-',  lw=1.8, label='True SoC',        zorder=4)
    ax_soc.plot(t, soc_ekf,  'r-',  lw=1.2, label='EKF',             zorder=5)
    ax_soc.plot(t, soc_cc,   'b--', lw=1.0, label='Coulomb Counting', zorder=3)
    ax_soc.plot(t, soc_ocv,  'g:',  lw=1.0, label='OCV Lookup',       zorder=2)
    ax_soc.set_ylabel('State of Charge (%)')
    ax_soc.set_title('SoC Estimation Comparison — Li-Ion NMC 60 Ah')
    ax_soc.legend(loc='upper right', fontsize=9)
    ax_soc.grid(True, alpha=0.3)
    ax_soc.set_ylim(0, 105)

    ax_err.axhline(0, color='k', lw=0.8)
    ax_err.plot(t, err_ekf,  'r-',  lw=0.9, label='EKF')
    ax_err.plot(t, err_cc,   'b--', lw=0.9, label='Coulomb Counting')
    ax_err.plot(t, err_ocv,  'g:',  lw=0.9, label='OCV Lookup')
    ax_err.set_ylabel('SoC Error (%)')
    ax_err.legend(loc='upper right', fontsize=9)
    ax_err.grid(True, alpha=0.3)

    ax_cur.plot(t, current, color='dimgray', lw=0.6)
    ax_cur.axhline(0, color='k', lw=0.5, ls='--')
    ax_cur.set_ylabel('Current (A)')
    ax_cur.set_xlabel('Time (min)')
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
