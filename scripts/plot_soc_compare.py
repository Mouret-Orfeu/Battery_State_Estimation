#!/usr/bin/env python3
"""
plot_soc_compare.py — SoC Estimator Comparison Plot

Calls the actual C implementations (src/soc_coulomb.c, src/soc_ocv.c,
src/soc_ekf.c) via the shared library built with 'make lib'.

Usage:
    make lib
    python3 scripts/plot_soc_compare.py \
        --input <path_to_cell_behavior_format_csv>

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

try:
    from plotly.subplots import make_subplots
    import plotly.graph_objects as go
    _PLOTLY_AVAILABLE = True
except ImportError:
    _PLOTLY_AVAILABLE = False

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

    lib.SocOcv_LoadTableFromCsv.argtypes = [ctypes.c_char_p]
    lib.SocOcv_LoadTableFromCsv.restype  = ctypes.c_uint

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
                initial_soc_pct: float, dts: list) -> list:
    state = BmsSocState()
    lib.SocCoulomb_Init(ctypes.byref(state), ctypes.c_float(initial_soc_pct))
    result = []
    for rec, dt in zip(records, dts):
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
            initial_soc_pct: float, dts: list) -> list:
    ecm   = BmsEcmParams(R0=0.005, R1=0.008, C1=1500.0)
    state = BmsSocState()
    ekf   = BmsEkfState()
    lib.SocEkf_Init(ctypes.byref(ekf), ctypes.byref(state),
                    ctypes.byref(ecm), ctypes.c_float(initial_soc_pct))
    result = []
    for rec, dt in zip(records, dts):
        lib.SocEkf_Update(ctypes.byref(ekf), ctypes.byref(state),
                          ctypes.c_float(rec['current_a']),
                          ctypes.c_float(rec['voltage_mv']),
                          ctypes.c_float(dt))
        result.append(state.soc_pct)
    return result


# ---- I/O helpers ----

def load_csv(path: str) -> list:
    with open(path, newline='') as f:
        reader = csv.DictReader(f)
        has_true_soc = 'true_soc_pct' in reader.fieldnames
        records = []
        for row in reader:
            r = {
                'time_s':     float(row['time_s']),
                'current_a':  float(row['current_a']),
                'voltage_mv': float(row['voltage_mv']),
            }
            if has_true_soc:
                r['true_soc_pct'] = float(row['true_soc_pct'])
            records.append(r)
    return records


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
        '--output', type=str, default=None,
        help='Output plot file (auto-detected from input path if omitted)',
    )
    parser.add_argument(
        '--interactive', action='store_true',
        help='Save an interactive HTML plot (Plotly) alongside the static PNG.',
    )
    parser.add_argument(
        '--soc0', type=float, default=None, metavar='PCT',
        help='Initial SoC in %% (0–100). If omitted, initialised via OCV lookup on the first sample.',
    )
    _default_ocv_table = str(
        _REPO_ROOT / 'data' / 'OCV_SoC' / 'OCV_SOC_NCA_1_folder' / 'OCV_SOC_NCA_1_processed.csv'
    )
    parser.add_argument(
        '--ocv-table', type=str, default=_default_ocv_table, metavar='CSV',
        help='OCV–SoC lookup table CSV (soc,ocv_mv columns). '
             f'Defaults to {_default_ocv_table}',
    )
    args = parser.parse_args()

    if args.soc0 is not None and not (0.0 <= args.soc0 <= 100.0):
        parser.error(f'--soc0 must be between 0 and 100, got {args.soc0}')

    if args.output is None:
        input_path = pathlib.Path(args.input).resolve()
        stem = input_path.stem
        if 'simulated_cell_behavior' in str(input_path):
            out_dir = _REPO_ROOT / 'docs' / 'simulated_cell_behavior' / 'plots' / 'soc_plots'
        elif 'real_cell_behavior' in str(input_path):
            out_dir = _REPO_ROOT / 'docs' / 'real_cell_behavior' / 'plots' / 'soc_plots'
        else:
            out_dir = _REPO_ROOT / 'docs'
        args.output = str(out_dir / f'soc_comparison_{stem}.png')

    lib = _load_lib()

    ret = lib.SocOcv_LoadTableFromCsv(args.ocv_table.encode())
    if ret != 0:
        raise RuntimeError(
            f'SocOcv_LoadTableFromCsv failed (err={ret}) for "{args.ocv_table}". '
            'Check that the file exists and has the expected soc,ocv_mv columns '
            'with the right number of entries and the right step size'
        )
    print(f'[INFO] OCV table loaded from {args.ocv_table}')

    records = load_csv(args.input)
    times   = [r['time_s'] for r in records]
    dts     = [times[i+1] - times[i] for i in range(len(times) - 1)]
    dts.append(dts[-1] if dts else 1.0)  # repeat last dt for the final sample

    has_true_soc = 'true_soc_pct' in records[0]

    t       = [r['time_s'] / 60.0 for r in records]
    current = [r['current_a']     for r in records]
    if has_true_soc:
        true_soc = [r['true_soc_pct'] for r in records]

    # Determine initial SoC
    if args.soc0 is not None:
        soc0 = args.soc0
        info = f"[INFO] Initial SoC from --soc0 argument: {soc0:.2f}%"
    else:
        soc0_c = ctypes.c_float(0.0)
        lib.SocOcv_LookupSoc(ctypes.c_float(records[0]['voltage_mv']),
                             ctypes.byref(soc0_c))
        soc0 = soc0_c.value
        info = f"[INFO] Initial SoC from OCV lookup: {soc0:.2f}%"
    if has_true_soc:
        info += f"  |  True initial SoC: {true_soc[0]:.2f}%"
    print(info)

    soc_cc  = run_coulomb(lib, records, soc0, dts)
    soc_ocv = run_ocv(lib, records)
    soc_ekf = run_ekf(lib, records, soc0, dts)

    if has_true_soc:
        err_cc  = [e - t_ for e, t_ in zip(soc_cc,  true_soc)]
        err_ocv = [e - t_ for e, t_ in zip(soc_ocv, true_soc)]
        err_ekf = [e - t_ for e, t_ in zip(soc_ekf, true_soc)]

    # ---- Plot ----
    if has_true_soc:
        fig = plt.figure(figsize=(12, 8))
        gs  = gridspec.GridSpec(3, 1, height_ratios=[3, 2, 1.5], hspace=0.45)
        ax_soc = fig.add_subplot(gs[0])
        ax_err = fig.add_subplot(gs[1], sharex=ax_soc)
        ax_cur = fig.add_subplot(gs[2], sharex=ax_soc)
    else:
        fig = plt.figure(figsize=(12, 6))
        gs  = gridspec.GridSpec(2, 1, height_ratios=[3, 1.5], hspace=0.35)
        ax_soc = fig.add_subplot(gs[0])
        ax_cur = fig.add_subplot(gs[1], sharex=ax_soc)
        ax_err = None

    # Marker stride: one marker every ~2 % of the time axis, staggered per curve
    _n = len(t)
    _ms = max(1, _n // 50)
    _me_ocv = (0,          _ms)
    _me_cc  = (_ms // 3,   _ms)
    _me_ekf = (_ms * 2 // 3, _ms)

    if has_true_soc:
        ax_soc.plot(t, true_soc, color='black',       lw=2.5, ls='-',
                    label='True SoC', zorder=4)
    ax_soc.plot(t, soc_ocv,  color='#2196F3',  lw=2.0, ls='-',
                marker='o', markersize=4, markevery=_me_ocv,
                label='OCV Lookup',       zorder=2)
    ax_soc.plot(t, soc_cc,   color='#4CAF50',  lw=2.0, ls='--',
                marker='s', markersize=4, markevery=_me_cc,
                label='Coulomb Counting', zorder=3)
    ax_soc.plot(t, soc_ekf,  color='#E91E63',  lw=2.0, ls='-.',
                marker='^', markersize=4, markevery=_me_ekf,
                label='EKF',             zorder=5)
    ax_soc.set_ylabel('State of Charge (%)')
    ax_soc.set_title('SoC Estimation Comparison — Li-Ion NMC 60 Ah')
    ax_soc.legend(loc='upper right', fontsize=9)
    ax_soc.grid(True, alpha=0.3)
    ax_soc.set_ylim(0, 105)

    if has_true_soc:
        ax_err.axhline(0, color='k', lw=0.8)
        ax_err.plot(t, err_ocv,  color='#2196F3', lw=1.4, ls='-',
                    marker='o', markersize=3, markevery=_me_ocv, label='OCV Lookup')
        ax_err.plot(t, err_cc,   color='#4CAF50', lw=1.4, ls='--',
                    marker='s', markersize=3, markevery=_me_cc,  label='Coulomb Counting')
        ax_err.plot(t, err_ekf,  color='#E91E63', lw=1.4, ls='-.',
                    marker='^', markersize=3, markevery=_me_ekf, label='EKF')
        ax_err.set_ylabel('SoC Error (%)')
        ax_err.legend(loc='upper right', fontsize=9)
        ax_err.grid(True, alpha=0.3)
        plt.setp(ax_err.get_xticklabels(), visible=False)

    ax_cur.plot(t, current, color='dimgray', lw=0.6)
    ax_cur.axhline(0, color='k', lw=0.5, ls='--')
    ax_cur.set_ylabel('Current (A)')
    ax_cur.set_xlabel('Time (min)')
    ax_cur.grid(True, alpha=0.3)

    plt.setp(ax_soc.get_xticklabels(), visible=False)

    out_dir = os.path.dirname(args.output)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    fig.savefig(args.output, dpi=150, bbox_inches='tight')
    plt.close(fig)

    print(f"[INFO] Plot saved → {args.output}")
    if has_true_soc:
        print(f"[INFO] RMSE  Coulomb: {rmse(err_cc):.3f}%  |  "
              f"OCV: {rmse(err_ocv):.3f}%  |  "
              f"EKF: {rmse(err_ekf):.3f}%")
    else:
        print("[INFO] No true_soc_pct column — RMSE not computed")

    if args.interactive:
        if not _PLOTLY_AVAILABLE:
            print("[WARN] plotly not installed — skipping interactive plot. "
                  "Run: pip install plotly")
        else:
            _save_interactive(
                args, t, soc_ocv, soc_cc, soc_ekf,
                true_soc if has_true_soc else None,
                err_ocv if has_true_soc else None,
                err_cc  if has_true_soc else None,
                err_ekf if has_true_soc else None,
                current,
            )


def _save_interactive(args, t, soc_ocv, soc_cc, soc_ekf,
                      true_soc, err_ocv, err_cc, err_ekf, current):
    has_true_soc = true_soc is not None
    n_rows = 3 if has_true_soc else 2
    row_heights = [0.55, 0.25, 0.20] if has_true_soc else [0.65, 0.35]
    subplot_titles = ['SoC Estimation', 'SoC Error (%)' if has_true_soc else None, 'Current (A)']
    subplot_titles = [s for s in subplot_titles if s]

    fig = make_subplots(
        rows=n_rows, cols=1,
        shared_xaxes=True,
        vertical_spacing=0.06,
        row_heights=row_heights,
        subplot_titles=subplot_titles,
    )

    soc_row = 1
    err_row = 2 if has_true_soc else None
    cur_row = 3 if has_true_soc else 2

    common = dict(mode='lines', line_width=2)

    if has_true_soc:
        fig.add_trace(go.Scattergl(
            x=t, y=true_soc, name='True SoC',
            line=dict(color='black', width=2.5), **{k: v for k, v in common.items() if k != 'line_width'},
        ), row=soc_row, col=1)

    fig.add_trace(go.Scattergl(
        x=t, y=soc_ocv, name='OCV Lookup',
        line=dict(color='#2196F3', width=2, dash='solid'), mode='lines',
    ), row=soc_row, col=1)
    fig.add_trace(go.Scattergl(
        x=t, y=soc_cc, name='Coulomb Counting',
        line=dict(color='#4CAF50', width=2, dash='dash'), mode='lines',
    ), row=soc_row, col=1)
    fig.add_trace(go.Scattergl(
        x=t, y=soc_ekf, name='EKF',
        line=dict(color='#E91E63', width=2, dash='dashdot'), mode='lines',
    ), row=soc_row, col=1)

    if has_true_soc:
        fig.add_hline(y=0, line_color='black', line_width=0.8, row=err_row, col=1)
        fig.add_trace(go.Scattergl(
            x=t, y=err_ocv, name='Error OCV',
            line=dict(color='#2196F3', width=1.4, dash='solid'), mode='lines',
            showlegend=False,
        ), row=err_row, col=1)
        fig.add_trace(go.Scattergl(
            x=t, y=err_cc, name='Error CC',
            line=dict(color='#4CAF50', width=1.4, dash='dash'), mode='lines',
            showlegend=False,
        ), row=err_row, col=1)
        fig.add_trace(go.Scattergl(
            x=t, y=err_ekf, name='Error EKF',
            line=dict(color='#E91E63', width=1.4, dash='dashdot'), mode='lines',
            showlegend=False,
        ), row=err_row, col=1)

    fig.add_hline(y=0, line_color='black', line_width=0.6,
                  line_dash='dash', row=cur_row, col=1)
    fig.add_trace(go.Scattergl(
        x=t, y=current, name='Current',
        line=dict(color='dimgray', width=1), mode='lines',
        showlegend=False,
    ), row=cur_row, col=1)

    fig.update_yaxes(title_text='SoC (%)', range=[0, 105], row=soc_row, col=1)
    if has_true_soc:
        fig.update_yaxes(title_text='Error (%)', row=err_row, col=1)
    fig.update_yaxes(title_text='Current (A)', row=cur_row, col=1)
    fig.update_xaxes(title_text='Time (min)', row=cur_row, col=1)

    fig.update_layout(
        title='SoC Estimation Comparison — Li-Ion NMC 60 Ah',
        hovermode='x unified',
        template='simple_white',
        height=700,
        legend=dict(orientation='h', yanchor='bottom', y=1.02, xanchor='right', x=1),
    )

    html_path = pathlib.Path(args.output).with_suffix('.html')
    fig.write_html(str(html_path), include_plotlyjs='cdn')
    print(f"[INFO] Interactive plot saved → {html_path}")


if __name__ == '__main__':
    main()
