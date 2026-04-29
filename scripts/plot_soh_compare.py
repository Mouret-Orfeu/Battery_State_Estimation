#!/usr/bin/env python3
"""
plot_soh_compare.py — SoH Estimation Plot

Calls Soh_ComputeFromTimeSeries() from the C library (built with 'make lib')
to derive SoH estimates from a simulated_cell_behavior CSV file, then plots
the SoH update events against the current time series.

Usage:
    make lib
    python3 scripts/plot_soh_compare.py \
        --input docs/simulated_cell_behavior/csv/simulated_cell_behavior_<i>.csv

Output: docs/soh_estimation.png  (configurable with --output)

Author: Orfeu Mouret
"""

import argparse
import csv
import ctypes
import os
import pathlib
import subprocess

import matplotlib.pyplot as plt
import matplotlib.gridspec as gridspec

# ---- Paths ----
_REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
_LIB_PATH  = _REPO_ROOT / 'lib' / 'libbms.so'

# Maximum number of SoH update events the output arrays are sized for.
# One update requires at least two confirmed 2-hour rests with >= 80 % DeltaSoC,
# so even a very long simulation produces few updates.
_MAX_SOH_UPDATES = 500


# ---- Library loader ----

def _load_lib() -> ctypes.CDLL:
    """Build lib/libbms.so if missing, then load and annotate it."""
    if not _LIB_PATH.exists():
        print('[INFO] lib/libbms.so not found — running "make lib"...')
        subprocess.run(['make', 'lib'], cwd=str(_REPO_ROOT), check=True)

    lib = ctypes.CDLL(str(_LIB_PATH))

    lib.Soh_ComputeFromTimeSeries.argtypes = [
        ctypes.POINTER(ctypes.c_float),   # current_a
        ctypes.POINTER(ctypes.c_float),   # voltage_mv
        ctypes.c_uint32,                  # n_samples
        ctypes.c_float,                   # dt_s
        ctypes.POINTER(ctypes.c_float),   # out_times_s
        ctypes.POINTER(ctypes.c_float),   # out_soh_pct
        ctypes.c_uint32,                  # max_updates
    ]
    lib.Soh_ComputeFromTimeSeries.restype = ctypes.c_uint32

    return lib


# ---- I/O helpers ----

def load_csv(path: str) -> list:
    with open(path, newline='') as f:
        return [{
            'time_s':       float(row['time_s']),
            'current_a':    float(row['current_a']),
            'voltage_mv':   float(row['voltage_mv']),
            'true_soc_pct': float(row['true_soc_pct']),
        } for row in csv.DictReader(f)]


# ---- Main ----

def main():
    parser = argparse.ArgumentParser(description='SoH Estimation Plot')
    parser.add_argument(
        '--input', type=str,
        default='docs/simulated_cell_behavior/csv/simulated_cell_behavior_0.csv',
        help='Input CSV produced by simulate_cell.py',
    )
    parser.add_argument(
        '--output', type=str, default='docs/soh_estimation.png',
        help='Output plot file',
    )
    args = parser.parse_args()

    lib     = _load_lib()
    records = load_csv(args.input)
    n       = len(records)
    dt      = records[1]['time_s'] - records[0]['time_s'] if n > 1 else 0.1

    # Build C arrays from the CSV columns
    FloatArr    = ctypes.c_float * n
    current_arr = FloatArr(*[r['current_a'] for r in records])
    voltage_arr = FloatArr(*[r['voltage_mv'] for r in records])

    OutArr    = ctypes.c_float * _MAX_SOH_UPDATES
    out_times = OutArr()
    out_soh   = OutArr()

    n_updates = lib.Soh_ComputeFromTimeSeries(
        current_arr, voltage_arr,
        ctypes.c_uint32(n),
        ctypes.c_float(dt),
        out_times, out_soh,
        ctypes.c_uint32(_MAX_SOH_UPDATES),
    )

    print(f"[INFO] {n} samples processed  |  {n_updates} SoH update(s) found")
    for i in range(n_updates):
        print(f"  update {i+1}: t = {out_times[i]/3600:.2f} h  ->  SoH = {out_soh[i]:.2f} %")

    soh_times_h = [out_times[i] / 3600.0 for i in range(n_updates)]
    soh_values  = [out_soh[i]             for i in range(n_updates)]

    time_h  = [r['time_s'] / 3600.0 for r in records]
    current = [r['current_a']        for r in records]

    # ---- Plot ----
    fig = plt.figure(figsize=(12, 6))
    gs  = gridspec.GridSpec(2, 1, height_ratios=[1.5, 2], hspace=0.4)

    ax_cur = fig.add_subplot(gs[0])
    ax_soh = fig.add_subplot(gs[1], sharex=ax_cur)

    ax_cur.plot(time_h, current, color='dimgray', lw=0.6)
    ax_cur.axhline(0, color='k', lw=0.5, ls='--')
    ax_cur.set_ylabel('Current (A)')
    ax_cur.grid(True, alpha=0.3)
    plt.setp(ax_cur.get_xticklabels(), visible=False)

    if n_updates > 0:
        ax_soh.step(soh_times_h, soh_values, where='post',
                    color='tab:red', lw=1.5, label='SoH estimate')
        ax_soh.scatter(soh_times_h, soh_values, color='tab:red', zorder=5, s=40)
    else:
        ax_soh.text(
            0.5, 0.5,
            'No SoH updates — simulation too short\nor DeltaSoC < 80 % threshold',
            ha='center', va='center', transform=ax_soh.transAxes, color='gray',
        )

    ax_soh.set_ylabel('SoH (%)')
    ax_soh.set_xlabel('Time (h)')
    ax_soh.set_ylim(0, 110)
    ax_soh.grid(True, alpha=0.3)
    if n_updates > 0:
        ax_soh.legend(fontsize=9)

    fig.suptitle(
        f"SoH Estimation — {pathlib.Path(args.input).name}  "
        f"({n_updates} update{'s' if n_updates != 1 else ''})",
        fontsize=11,
    )

    out_dir = os.path.dirname(args.output)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    fig.savefig(args.output, dpi=150, bbox_inches='tight')
    plt.close(fig)
    print(f"[INFO] Plot saved → {args.output}")


if __name__ == '__main__':
    main()
