# Pending experiments (claim-A study)

Living list of simulations still to run, why, what we expect, and what each would decide.
Status: TODO / RUNNING / DONE. Update as work proceeds. Results so far: `RESULTS.md`.

Cost rule of thumb (4 cores): one 300 s LTE run takes about 15-17 min of wall time but four run in parallel,
so N LTE runs cost about 4.3 min x N. One wired run is about 2.5 min. Radio-only LTE uses seeds 1-5 in all
sweeps (seed 6 crashes inside ns-3's PDCP code in every non-baseline radio-only configuration).

Runner: `TAG=<name> EXTRA="<flags>" scratch/run_seeds.sh <lte|lte_blk|wired> <first> <last> [simTime] [jobs]`
writes to `runs/<model>_<TAG>/seed_N/`. Existing simulator flags: `--minRto`, `--rlcBufferKB`, `--rlcMode`,
`--blockage`, `--tcpVariant`, `--bottleneckQueue`, `--bottleneckBw`, `--bgRate`, `--outageSchedule` (wired only).

---

## Priority 1: cheap and high value

### E1. Mechanism test: small RLC buffer with a high RTO floor  [TODO]
- **Question:** why does the spurious count fall as the RLC buffer grows? Hypothesis: spurious timeouts need a delay
  hiccup larger than the *current absolute RTO*; small buffers leave the RTO at its 200 ms floor 63% of the time.
- **Run:** LTE radio-only, 5 KB buffer + MinRto 1000 ms, seeds 1-5.
  `TAG=rlc5_mr1000 EXTRA="--rlcBufferKB=5 --minRto=1000ms" scratch/run_seeds.sh lte 1 5`
- **Prediction (written before running):** spurious per hour falls to about 0.
- **Reading it:** about 0 supports "absolute timer level vs ordinary radio hiccups". Still high means another
  mechanism (loss/retransmission dynamics with tiny windows) and the RESULTS.md explanation must change.
- **Cost:** 5 runs, about 35 min.

### E2. Wired outage replay (control "Arm 2")  [TODO]
- **Question:** is the spurious-timeout effect LTE-specific, or does *any* path with the same outages show it?
- **Run:** `wired_control` with `--outageSchedule` built from each seed's `runs/lte_blk/seed_N/blockage_lte.csv`
  (convert `start,end,depth` to `start,duration`, e.g. `awk -F, 'NR>1{print $1","$2-$1}'`). Needs a small
  per-seed driver (run_seeds.sh takes one EXTRA for all seeds), seeds 1-30, MinRto 200 ms.
- **Prediction:** wired + outages shows some spurious timeouts (ACK-loss kind, since the outage kills ACKs both
  ways) but fewer *late-ACK* ones than LTE+blockage, because there is no RLC retransmission holding data.
- **Reading it:** separates "LTE adds delay jitter" from "outages cause timeouts on any link". Also supplies the
  wired arm needed for E5/Test 2.
- **Cost:** about 20 min.

### E3. Wired bottleneck-queue sweep (fairness of the control)  [TODO]
- **Question:** is the wired control's zero spurious timeouts robust, or tuned by the 30-packet queue?
- **Run:** `TAG=q15 EXTRA="--bottleneckQueue=15"`, `q100`, `q200` with `scratch/run_seeds.sh wired 1 6` (30 = existing).
- **Prediction:** stays 0 spurious; genuine losses and RTT change.
- **Cost:** 18 wired runs, about 12 min.

---

## Priority 2: strengthens or extends the main claims

### E4. Extended RLC buffer sweep to real-world delay range  [TODO]
- **Why:** measured real LTE queueing delay is 1-2 s (Winstein 2013) and up to 10 s on HSPA+ (Jiang et al.), above
  our 128 KB point (p99 RTT 1.15 s).
- **Run:** radio-only, 256, 512 and 1024 KB, seeds 1-5.
  `for kb in 256 512 1024; do TAG=rlc$kb EXTRA="--rlcBufferKB=$kb" scratch/run_seeds.sh lte 1 5; done`
- **Prediction:** spurious/h keeps falling (about 100 or below) but p99 RTT reaches several seconds; the
  blockage scenario's outage-driven expiries should NOT fall (not buffer-dependent). Test that with `lte_blk`.
- **Cost:** 15 radio-only runs, about 1.2 h (+ about 1.2 h if repeated with `lte_blk`).

### E5. Test 2: fault-injection after a fade  [TODO, needs new code]
- **Question:** is the RTO too high for a real loss right after a fade? (The stale-RTO-after-a-fade hypothesis; existing data say no.)
- **Design:** inject a 200 ms 100%-loss burst on the SGi link (burst so fast retransmit cannot rescue it) at
  tau = 0.25, 0.5, 1, 2, 4, 8 s after each blockage recovery; paired control injections at random times at
  least 12 s from any blockage; plus a single-segment-drop condition to separate "RTO inflated but masked by fast
  retransmit" (hypothesis A) from "RTO never inflated" (B).
- **Arms:** LTE after fade, LTE no-fade, wired no-fade, wired outage replay (E2).
- **Criterion (fixed in advance):** slack and detection delay for tau <= 3 s at least 1.5x the no-fade baseline,
  non-overlapping 95% CIs over the runs.
- **Open question:** run at MinRto 200 ms, 1000 ms, or both (slack scales with the floor by construction).
- **Prediction:** fails (existing evidence: at fast-retransmit moments after a fade, RTO / pre-fade RTO = 0.97 vs
  1.26 control; median RTO at recovery 1.04 vs 1.00).
- **New code:** scheduled-burst hook in `lte_ue_to_ue.cc` (RateErrorModel on the SGi devices keyed to the blockage
  schedule), `inject_lte.csv` log, detection-delay metric in `analyze_claim_a.py`.
- **Cost:** 29 LTE runs about 2.3 h + wired about 20 min, plus development.

### E6. RLC mode: Acknowledged vs Unacknowledged  [TODO]
- **Question:** are late ACKs mainly caused by RLC AM link-layer retransmission?
- **Run:** `TAG=um EXTRA="--rlcMode=RlcUmAlways"`, radio-only, seeds 1-5 (note: earlier work saw AM window stalls
  with heavy UDP; watch for them).
- **Prediction:** fewer late-ACK spurious timeouts, more genuine ones (delay becomes loss).
- **Cost:** 5 runs, about 35 min.

### E7. Finer MinRto grid  [TODO, optional]
- 250, 300, 500, 800 ms (radio-only and blockage) to draw a smoother trade-off curve, the bar claim B must beat.
- Cost: 4 values x 5-6 seeds x 2 scenarios, about 3 h.

---

## Priority 3: scope and generality

### E8. Other TCP variants  [TODO]
- Does the result depend on NewReno? Run `--tcpVariant=TcpCubic` and `TcpBbr` on LTE radio-only and wired, 10 seeds.
- Cost: LTE 20 runs about 1.5 h, wired about 15 min.

### E9. Mobility  [TODO, needs new code]
- The monitored UEs are stationary; the project goal is mobile-to-mobile. Add a moving UE-A / UE-B (waypoint or
  constant velocity inside coverage); cross-EPC handover remains unmodelled (needs a shared MME).
- Compare spurious rate and the RTO/RTT behaviour against the stationary case.

### E10. Understand ns-3 vs Linux RTT sampling after an outage  [TODO, investigation]
- Gurtov & Ludwig (2003) Fig 6(b): with timestamps the RTO inflates and takes about 10 s to settle. ns-3 (timestamps
  on) showed no inflation after recovery. Read `TcpSocketBase::EstimateRtt` / receiver echo logic and run a
  single-flow forced-outage test to find out why; decides whether "no stale RTO" is ns-3-specific.

### E11. Statistical tightening and crash handling  [TODO, optional]
- Raise sweep seeds from 5-6 to 15-30 for any point whose CI is within a factor of 2 of a comparison.
- Find the cause of the ns-3 PDCP assertion (`m_dcBit == DATA_PDU`, lte_blk seed 28; radio-only seed 6 in sweeps)
  or document it as a known ns-3 limitation.


### E12. Realism of the cross-layer radio feed (claim B prerequisite)  [TODO, design]
- **Why:** on stock Android the radio metrics reach apps at best every 3 s and only after a 2 dB change
  (AOSP `SignalStrengthController`: `REPORTING_HYSTERESIS_MILLIS = 3000`, `REPORTING_HYSTERESIS_DB = 2`; verified at
  source), after a ~200 ms L1 measurement period (3GPP drafts / patent, not the spec). Radio hiccups last 0.1-0.5 s and
  our blockages 1-8 s, so the public-API feed is slower than the events the RTO must react to.
- **Plan:** in the RL environment, expose radio features as sampled + delayed + quantised: period in {10 ms, 200 ms,
  1 s, 3 s}, 2 dB change threshold, optional extra latency. Compare Tier A (TCP-only) vs Tier B at each period.
- **Result wanted:** the longest feed period at which radio information still beats TCP-only.
- **Unknowns to settle:** framework-to-app callback latency; what an OEM/kernel-level feed could achieve; CQI report
  periodicity (network-configured, values not verified here).

---

## Validation (not simulations)
- Compare simulated RTT/jitter distributions with a real LTE trace; the buffer sizes and radio rate are uncalibrated.
- Read the unread leads: Li et al. ToN 2017 and Jang et al. 2009 (cited second-hand for "high spurious RTO rate"),
  Huang et al. SIGCOMM 2013, Alfredsson et al. WoWMoM 2013.

## Claim B simulations (later phase)
- Learned RTO as a custom `RttEstimator` (hook: `TcpSocketBase::SetRtt`), in-loop runs on held-out seeds
  (about 10 per model, LTE and control, baseline vs candidate = about 20 runs), after replay-based development.
- The MinRto trade-off curves in `fig4_minrto_sweep.png` are the baseline frontier it must beat.

## Suggested order
E1 -> E2 -> E3 (about 70 min total, mostly parallelisable) -> decide E5's MinRto choice -> E5 -> E4 -> E6 -> rest.
