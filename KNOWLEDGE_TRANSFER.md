# Knowledge transfer guide

For someone taking over this project. Read this, then [`README.md`](README.md) (how to run things), then
[`analysis/RESULTS.md`](analysis/RESULTS.md) (what was found), then
[`analysis/PENDING_EXPERIMENTS.md`](analysis/PENDING_EXPERIMENTS.md) (what to do next).

## 1. The idea in five sentences

TCP waits for an acknowledgement for a time called the RTO; if it expires the sender retransmits and slows
down. RFC 6298 sets the RTO from recent round-trip times, which assumes delay changes slowly. On LTE, link-layer
retransmission, scheduling, queueing and blocked channels add sudden delay without loss, so the timer can fire
although nothing was lost (a *spurious timeout*). We compare an LTE testbed with a wired twin that differs only in
having no radio, and count spurious timeouts with simulator-exact ground truth. Once that is established, the plan
is a learned RTO that runs on the phone (claim B).

## 2. Mental model of the two programs

* `scratch/lte_ue_to_ue.cc`: TCP flow UE-A -> UE-B across **two independent LTE networks** (EPC-A and EPC-B, UE
  pools 7.0.0.0/8 and 9.0.0.0/8), joined by a 5 Mbps / 30-packet link that is the monitored bottleneck.
  Each side has 4 background UEs. Radio: PHY error model + HARQ, RLC acknowledged mode (32 KB buffer), Rayleigh
  trace fading, and an optional **blockage model** (`--blockage=true`).
* `scratch/wired_control.cc`: the same network with each radio replaced by a 100 Mbps LAN. TCP settings, traffic,
  bottleneck and wired delays are identical.
* Both write CSVs in the same schema so one analyser serves both.

## 3. How a timeout is classified (the core measurement)

* **Expiry detection.** ns-3 has no timeout event. The traced RTO value doubles *exactly* only in the
  exponential-backoff code, so `RTO == 2 x previous RTO` marks an expiry (`TraceRto`).
* **Oracle.** For an expiry retransmitting segment `s`, look at earlier copies of `s` (sender log, matched by TCP
  timestamp value TSval) and ask whether any reached the receiver (receiver log, tapped at
  `Ipv4L3Protocol/LocalDeliver`). Not delivered -> **genuine**. Delivered -> **spurious**, split into **late ACK**
  (an ACK echoing that copy arrived after the timer fired, via TSecr in `ack_rx_*.csv`) and **lost ACK**.
* The oracle is for *scoring only*. A deployed learner cannot use it.

## 4. Metrics and the pre-registered criterion

M1 spurious expiries per hour; M2 RTO slack at genuine losses `(RTO - RTT) / RTT`; M3 share of time in loss
recovery; M4 goodput; M5 RTO recovery time after an outage. Criterion (fixed before comparing): claim A holds if
LTE is worse than the control on M1 (>= 2x) or M2 (>= 1.5x) with non-overlapping 95% bootstrap CIs. The **run** is
the unit of analysis (30 seeds). The 2x / 1.5x thresholds are judgements, not derived.

## 5. Decisions made, and why

| Decision | Reason |
|---|---|
| Two EPCs instead of one | One shared gateway makes UE-to-UE traffic "hairpin" inside it and never cross a network boundary |
| Wired twin matched in everything but the radio | Any difference is then attributable to the radio |
| Honest wired delays (no padding to LTE's RTT) | Padding would hide the higher LTE RTT floor; compare rates and normalised quantities |
| `MinRto = 200 ms` | Linux default; RFC 6298 says 1 s. The study shows this choice matters a lot (see sweep) |
| RLC acknowledged mode, error model on | Real LTE data bearers; a benign radio could not produce spurious timeouts at all |
| Custom blockage model | Rayleigh fading never produced a real fade (wideband SINR stayed >= 14 dB) |
| Outage = ACK stall >= 200 ms overlapping a blockage episode | The first rule (CQI <= 3) missed a 3.6 s outage; amended on a pilot run before the main batches |
| Run = unit of analysis, 30 seeds | Samples within a run are strongly correlated |
| Radio-only sweeps use seeds 1-5 | Seed 6 crashes in ns-3's PDCP code in every non-baseline radio-only configuration; excluded everywhere |

## 6. ns-3 pitfalls we hit (save yourself the time)

1. **Two EPCs in one simulation.** `NoBackhaulEpcHelper` hard-codes the UE pool to 7.0.0.0/8 with no setter.
   Rebase the second network's addresses (to 9.0.0.0/8) **before** `Attach()`, because
   `ActivateEpsBearer()` reads the UE's address synchronously inside `Attach()`.
2. `Ipv4AddressGenerator::Reset()` is needed between the two EPC helpers (and before the second network's S1-U
   install and UE address assignment), or ns-3 aborts on "address collision".
3. **Use one `LteHelper`** and switch its EPC with `SetEpcHelper()` before each `InstallEnbDevice`,
   `InstallUeDevice` and `Attach` group. Two `LteHelper`s both give cell ID 1 and crash attach (RRC "unexpected
   state").
4. `Ipv4AddressHelper::Assign()` silently installs an **FqCoDel queue disc** on devices without one. We remove it
   on the bottleneck (`TrafficControlHelper::Uninstall`) so the 30-packet DropTail is the only buffer.
5. `EpsBearerToRlcMapping` valid values: `RlcSmAlways | RlcUmAlways | RlcAmAlways | PacketErrorRateBased`
   (there is no `PerBearer`).
6. The `RTO` trace fires only on **changes**; with a high `MinRto` the trace can show a single entry.
7. `TraceFadingLossModel` loops a 10 s trace, so `appStart` must not be a multiple of 10. Radio-link-failure
   thresholds are relaxed (N310 = 20, T310 = 2 s, N311 = 1, T300 = 2 s, Qout disabled) so deep outages do not
   trigger re-establishment; this is a known limitation.
8. Crashes: `NS_ASSERT failed, cond="m_dcBit == DATA_PDU"` (`lte-pdcp-header.cc`) hits some seeds (blockage seed 28;
   radio-only seed 6 under several non-baseline settings). `run_seeds.sh` only writes `DONE` on a clean exit, and the
   analysers skip incomplete runs. Report them rather than hiding them.
9. Do not use `pkill -f <pattern>` where the pattern also appears in your own command line; it kills your shell.
10. System Python on many Linux distributions blocks `pip install`; use a virtual environment.

## 7. Where the evidence for each claim lives

| Statement | Evidence |
|---|---|
| LTE has hundreds of spurious timeouts per hour, wired has none | `analysis/blk_vs_wired_*`, `analysis/radio_vs_wired_*`, `fig1_headline.png` |
| They come from late ACKs | `n_spurious_late` vs `n_spurious_ackloss` in `*_per_run.csv` |
| The RTO is not stale after a fade | `fig3_rto_around_recovery.png`; masking check in `RESULTS.md` section on Test 2 |
| The RTO floor controls the effect | `fig4_minrto_sweep.png`, `sweep_per_run.csv` |
| Buffer size: more buffer, fewer spurious timeouts | `fig5_rlc_sweep.png`; the pre-run prediction was the opposite and was wrong |

## 8. Things that were wrong at some point (so you do not repeat them)

* A benign first radio gave RTT that never exceeded the RTO floor, so spurious timeouts were impossible.
* The first verdict code divided by the control's zero rate (NaN), wrongly reporting the claim unsupported.
  A zero control rate is now treated as infinitely smaller.
* The first version of the example-episode figure merged two blockage episodes (an unshaded second one).
* The hypothesis that bigger RLC buffers cause more spurious timeouts was wrong; the working explanation (the RTO
  tracks higher delay, while a floor-pinned RTO loses to ordinary hiccups) is **still untested** (experiment E1).

## 9. Adding a new experiment

1. Add a command-line flag in the simulator(s) if needed (`CommandLine cmd`), and echo it in the `[SEED]` log
   line so every run records its configuration.
2. Run with `TAG=<name> EXTRA="<flags>" scratch/run_seeds.sh <model> <first> <last>`; results go to
   `runs/<model>_<name>/`.
3. Write predictions **before** running (see how `PENDING_EXPERIMENTS.md` does it), then analyse with the same
   scripts and report the outcome either way.
4. Update `PENDING_EXPERIMENTS.md` and `RESULTS.md`.

## 10. Open questions for the next stage (claim B)

* Where does the agent act and what does it control: the RTO per ACK (kernel/OS change) or the per-route minimum
  RTO (`ip route ... rto_min`, settable from user space on Linux)?
* What can a phone really observe? Downlink RSRP/RSRQ/SINR/CQI directly; uplink channel and cell load only through
  proxies. On stock Android the signal-strength feed to apps is rate-limited (AOSP defaults: at least 3 s between
  unsolicited reports and a 2 dB change threshold), far slower than the events the RTO must handle, so the
  simulated feed must be degraded accordingly (experiment E12).
* Reward from the simulator's oracle (spurious penalty, waiting penalty at genuine losses, goodput); safety bounds on
  the RTO; train and test on different seeds and scenarios. Baselines: RFC 6298 at 200 ms, and the fixed-floor
  trade-off curve in `fig4_minrto_sweep.png`.

## 11. Contact

[Add the previous owner's name and email, and the supervisor's name]
