# TCP retransmission timeout (RTO) over LTE: simulation study

ns-3.46 testbed and analysis code for one question: **does the standard TCP RTO estimator (RFC 6298)
behave worse over LTE than over an otherwise identical wired path?** This is "claim A" of a project whose
long-term goal is a reinforcement-learning RTO that runs on the phone using only signals the phone can see
("claim B", not started).

Start with [`KNOWLEDGE_TRANSFER.md`](KNOWLEDGE_TRANSFER.md) if you are taking over the project.

## Headline result (30 seeds x 300 s per scenario)

| | Wired control | LTE radio-only | LTE + blockage |
|---|---|---|---|
| Spurious RTO expiries per hour | **0** | **363** [329, 397] | **582** [541, 628] |
| Share of expiries that are spurious | 0% | 85% | 89% |
| Time in loss recovery | 0.2% | 3.9% | 9.1% |

* A timeout is *spurious* when the data had already reached the receiver (ground truth from the simulator).
  99.8% of the LTE spurious timeouts were caused by a **late ACK**, not a lost one.
* The effect is controlled mainly by the **RTO floor** (`MinRto`): 361 -> 0 per hour at 1 s without outages, but
  128 per hour remain with blockage outages. It holds for RLC buffers from 5 to 128 KB.
* A hypothesised "stale RTO after a fade" mechanism was **not** supported.
* Brackets are 95% bootstrap CIs over runs. Details, caveats and corrections: [`analysis/RESULTS.md`](analysis/RESULTS.md).

## What is in this repository

```
scratch/                      copy these into <ns-3.46>/scratch/
  lte_ue_to_ue.cc             LTE testbed: two independent EPCs, lossy radio, optional blockage
  wired_control.cc            matched wired twin (no radio); scheduled-outage hook reserved
  run_seeds.sh                run one model for a range of seeds, N in parallel
  run_sweeps.sh               MinRto and RLC-buffer sensitivity sweeps (one shared job queue)
  analyze_claim_a.py          oracle, metrics M1-M5, LTE-vs-wired comparison and criterion
  analyze_sweep.py            sweep tables and figures
  plot_claim_a.py             main figures
analysis/                     results, figures and the pending-experiments list
  RESULTS.md                  written summary: tables, caveats, sensitivity sweeps
  PENDING_EXPERIMENTS.md      simulations still to run (E1-E12) with commands and predictions
  fig1..fig5_*.png            result figures
  *_compare.csv, *_per_run.csv, *_verdict.json   numbers behind the comparisons
report/
  make_topology_fig.py        topology diagram generator; figures/fig_topology.png
```

Raw simulation output (`runs/`, about 12 MB per run) is **not** stored here. Regenerate it with the scripts below.

## Requirements

* **ns-3.46** built from source (<https://www.nsnam.org>), with the LTE, internet, applications, point-to-point,
  CSMA, mobility, propagation and traffic-control modules (all default modules).
* Python 3 with `numpy`, `pandas`, `matplotlib` (`pip install -r requirements.txt`, ideally in a virtual
  environment; many systems block system-wide `pip`).
* A multi-core machine: one 300 s LTE run takes about 15-17 minutes of wall time, about 3 s per simulated
  second; four runs in parallel is a good fit for 4 cores. A wired run takes about 2.5 minutes.

## Quick start

All commands are run from the **ns-3.46 root directory**.

```bash
# 1. install: copy the simulators, scripts and analysis code into your ns-3.46 tree
cp scratch/* /path/to/ns-3.46/scratch/
cp -r analysis /path/to/ns-3.46/          # optional: the published results and figures
cd /path/to/ns-3.46

# 2. build
./ns3 build scratch/lte_ue_to_ue scratch/wired_control

# 3. smoke test (30 s of simulated time; CSVs appear in the current directory)
./ns3 run "scratch/lte_ue_to_ue --simTime=30"
./ns3 run "scratch/wired_control --simTime=30"

# 4. main batches: 30 seeds each, results in runs/<model>/seed_N/
scratch/run_seeds.sh wired   1 30
scratch/run_seeds.sh lte     1 30      # radio-only
scratch/run_seeds.sh lte_blk 1 30      # radio + blockage

# 5. compare LTE with the wired control (writes analysis/*_compare.csv, *_per_run.csv, *_verdict.json)
python3 scratch/analyze_claim_a.py --lte 'runs/lte_blk/seed_*' --wired 'runs/wired/seed_*' --out analysis/blk_vs_wired
python3 scratch/analyze_claim_a.py --lte 'runs/lte/seed_*'     --wired 'runs/wired/seed_*' --out analysis/radio_vs_wired

# 6. figures
python3 scratch/plot_claim_a.py

# 7. sensitivity sweeps (MinRto and RLC buffer), then their tables and figures
scratch/run_sweeps.sh
python3 scratch/analyze_sweep.py
```

A single configuration, for example a 1 s RTO floor with a 5 KB RLC buffer:

```bash
TAG=rlc5_mr1000 EXTRA="--rlcBufferKB=5 --minRto=1000ms" scratch/run_seeds.sh lte 1 5
```

Useful flags: `--minRto`, `--rlcBufferKB`, `--rlcMode`, `--blockage`, `--tcpVariant`, `--bottleneckQueue`,
`--bottleneckBw`, `--bgRate`, `--RngRun` (seed), and for the wired control `--outageSchedule <file>` (lines of
`start_s,duration_s`).

## Output of one run

`runs/<model>/seed_N/` holds `tx_*.csv`, `rx_*.csv` and `retx_*.csv` (every segment, with TCP timestamp),
`timeout_*.csv` (one row per RTO expiry), `ack_rx_*.csv` (every ACK with timestamp echo), `rtt_*.csv`,
`rto_*.csv`, `cwnd_*.csv`, `queue_*.csv`, `throughput_*.csv`, `cqi_lte.csv` and `blockage_lte.csv` (LTE),
`cmd.txt` (the exact command) and `stdout.txt`. A `DONE` file means the simulator exited cleanly; analysis
skips runs without it.

## Status and caveats

* Completed: testbed, oracle, 30-seed comparison, `MinRto` and RLC-buffer sweeps.
* Pending: see [`analysis/PENDING_EXPERIMENTS.md`](analysis/PENDING_EXPERIMENTS.md) (mechanism test, wired outage
  replay, fault-injection experiment, mobility, other TCP variants, realism of the cross-layer radio feed).
* Simulation only and **uncalibrated**: read the numbers as relative comparisons, not real-network rates.
* The monitored UEs are stationary; handover between the two EPCs is not modelled.
* ns-3 occasionally aborts with `NS_ASSERT failed, cond="m_dcBit == DATA_PDU"` (PDCP). Those runs are excluded.

## Licence

Released under the **GNU General Public License, version 2 only (GPL-2.0-only)**; see [`LICENSE`](LICENSE).
This matches ns-3, which is distributed under GPL-2.0-only and which the simulators here are built against.

SPDX-License-Identifier: GPL-2.0-only

Copyright (C) 2026 [Chetan Varma]

## Authors

[Chetan Varma dchetanv2003@gmail.com]
