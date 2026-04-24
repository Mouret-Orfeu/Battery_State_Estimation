# BMS State-of-Charge (SoC) Estimation — Embedded C + Python

A modular **Battery Management System (BMS)** SoC estimation library targeting automotive-grade high-voltage battery packs. Implements three SoC algorithms commonly used in production EV/HEV systems, validated against a simulated Li-Ion cell model.

> Developed with reference to ASIL C/D functional safety requirements (ISO 26262) for high-voltage battery management — reflecting experience from HVBMS project work on electric drive platforms.

---

## Motivation

Accurate SoC estimation is one of the most safety-critical functions in an EV Battery Management System. Underestimation leads to unexpected shutdowns; overestimation risks overcharge, thermal runaway, and cell degradation. This project implements and compares three established estimation methods:

| Algorithm         | Method                    | Pros                                | Cons                        |
|-------------------|---------------------------|-------------------------------------|-----------------------------|
| Coulomb Counting  | Integrate current × time  | Simple, low CPU                     | Drifts with sensor error    |
| OCV Lookup        | Voltage → SoC table       | No drift, accurate at rest          | Only valid at equilibrium   |
| Extended Kalman Filter (EKF) | State estimation | Best accuracy, handles noise | Higher complexity          |

---

## File Structure

```
Battery_State_Estimation/
├── include/
│   ├── bms_types.h          # Cell parameters, SoC state struct, error codes
|   ├── test_helpers.h       # Some common macros for the test scripts
│   ├── soc_coulomb.h        # Coulomb Counting API
│   ├── soc_ocv.h            # OCV Lookup Table API
│   └── soc_ekf.h            # Extended Kalman Filter API
├── src/
│   ├── soc_coulomb.c        # Coulomb Counting implementation
│   ├── soc_ocv.c            # OCV table interpolation
│   └── soc_ekf.c            # EKF state estimator (1st order ECM model)
├── test/
│   ├── test_coulomb.c       # Unit tests: current integration accuracy
│   ├── test_ocv.c           # Unit tests: OCV table boundary & interpolation
│   └── test_ekf.c           # Unit tests: EKF convergence from initial error
├── scripts/
│   ├── simulate_cell.py     # Li-Ion cell simulator (generates test vectors)
│   └── plot_soc_compare.py  # Plots true vs. estimated SoC across methods  
├── Makefile
└── README.md
```

---

## Cell Model (Equivalent Circuit Model — ECM)

The EKF uses a first-order **Randles circuit** (1-RC) model:

```
    R0        R1
+--/\/\/---+--/\/\/--+
|          |         |
Voc(SoC)  === C1   V_terminal
|          |         |
+----------+---------+
```

- **Voc(SoC):** OCV as a function of SoC (from lookup table)
- **R0:** Ohmic (internal) resistance
- **R1, C1:** Diffusion RC pair (polarization dynamics)
- **State vector:** `x = [SoC, V_RC]ᵀ`

---

## Algorithms

### 1. Coulomb Counting (`soc_coulomb.c`)

```
SoC(t) = SoC(t-1) - (I × Δt) / (3600 × Q_nom × η)
```

- Integrates measured current at each sampling step
- Applies Coulombic efficiency `η` during charge/discharge
- Requires accurate initial SoC (uses OCV lookup at startup)
- (Δt is in second and is divided by 3600 so the numerator is in A.h like Q_nom)

### 2. OCV Lookup (`soc_ocv.c`)

- Uses a 21-point OCV–SoC table (0–100% in 5% steps) for NMC chemistry
- Linear interpolation between table entries
- Valid only after ≥ 2 hours of rest (equilibrium condition)
- Used for SoC initialisation at ignition ON

### 3. Extended Kalman Filter (`soc_ekf.c`)

```
State prediction:   x̂(k|k-1) = A·x̂(k-1|k-1) + B·u(k)
Covariance predict: P(k|k-1) = A·P(k-1)·Aᵀ + Q
Kalman gain:        K = P(k|k-1)·Hᵀ·(H·P(k|k-1)·Hᵀ + R)⁻¹
State update:       x̂(k|k) = x̂(k|k-1) + K·(y(k) − ŷ(k))
Covariance update:  P(k|k) = (I − K·H)·P(k|k-1)
```

---

## Simulation & Visualisation

```bash
# Generate simulated drive cycle current profile + true SoC
python3 scripts/simulate_cell.py --capacity 60 --duration 3600 --output test_vectors.csv

# Compare all three estimators against true SoC
python3 scripts/plot_soc_compare.py --input test_vectors.csv
```

Output: comparison plot saved to `docs/soc_comparison.png`

---

## Building

```bash
# Build all C modules
make all

# Run unit tests
make test

# Clean
make clean
```

---

## ISO 26262 Relevance

This project reflects design patterns applied in ASIL C/D BMS development:

- **Plausibility checks:** Current sensor saturation, voltage out-of-range detection
- **Redundant estimation:** Dual-channel SoC (Coulomb + EKF), voter logic
- **Safe state:** SoC clamped to `[SOC_MIN, SOC_MAX]` on estimation divergence
- **Error propagation:** `bms_error_t` codes fed to diagnostic manager (maps to UDS DTC storage)

---

## Tools & Standards

- Language: **C99**, **Python 3.10+**
- Standards: ISO 26262, IEC 62133, SAE J1772
- Cell chemistry: NMC (Nickel Manganese Cobalt) — 3.0 V – 4.2 V
- Nominal capacity: 60 Ah (configurable via `bms_types.h`)
- Sampling rate: 100 ms (10 Hz)

---

## Author

**Kamal Kadakara**  
M.Sc. Embedded Systems — TU Chemnitz  
ISO 26262 Functional Safety Professional (TÜV SÜD, Level 1)  
[LinkedIn](https://linkedin.com/in/kamal-kadakara)
