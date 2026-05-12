# This script processes the raw OCV-SoC data for NCA_1, creating a uniformly spaced grid of SoC values using linear interpolation.
import numpy as np
import pandas as pd
from pathlib import Path

HERE = Path(__file__).parent

raw = pd.read_csv(HERE / 'OCV_SOC_NCA_1.csv', header=None, names=['soc', 'ocv'])
raw = raw.sort_values('soc').reset_index(drop=True)

soc_grid = np.arange(0, 101) / 100               # 101 points: 0.00, 0.01, …, 1.00 (exact)
ocv_grid = np.interp(soc_grid, raw['soc'], raw['ocv']) * 1000  # V → mV

out = pd.DataFrame({'soc': soc_grid, 'ocv_mv': np.round(ocv_grid, 0)})

out_path = HERE / 'OCV_SOC_NCA_1_processed.csv'
out.to_csv(out_path, index=False)
print(f'Saved {len(out)} rows → {out_path}')


