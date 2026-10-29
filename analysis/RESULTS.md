# Claim A results: does stock RFC 6298 RTO misbehave on LTE?

30 seeds × 300 s per model (LTE + blockage: 29, seed 28 crashed in ns-3's PDCP code and is excluded).
Spurious = the retransmitted segment had already reached the receiver (receiver-side oracle, TSval-matched).

| | Wired control (n=30) | LTE radio-only (n=30) | LTE + blockage (n=29) |
|---|---|---|---|
| Spurious RTO expiries / hour | 0 | 363 [329, 397] | 582 [541, 628] |
| Share of expiries that are spurious | 0% | 85% | 89% |
| Of those, "late ACK" | n/a | 99.8% | 99.8% |
| Time in loss recovery (CA_LOSS) | 0.2% | 3.9% (2.5% without seed 8) | 9.1% |
| Goodput (kbit/s) | 2548 | 1108 | 1034 |
| p99 RTT / min RTT | 3.4 | 8.4 | 8.4 |

Brackets are 95% bootstrap CIs over runs. Every LTE run (59/59) exceeded every wired run on spurious expiries per hour.
Blockage adds 219/h [164, 274] on top of the radio-only baseline.

## Pre-registered criterion
M1 (spurious/hour: LTE >= 2x control, CIs disjoint): met in both scenarios.
M2 (RTO slack at genuine loss: LTE >= 1.5x control, CIs disjoint): not met (blockage 0.83 vs 1.34; radio-only 1.85 [0.64, 4.19], inconclusive).
Claim A is supported through M1 (RTO too short). It is not supported through M2 (RTO too loose).

## The "stale RTO after a fade" hypothesis: not supported
352 outage episodes in 29 runs. RTO/pre-fade-RTO at recovery: median 1.04 vs control 1.00 (p90 2.07 vs 2.01);
median RTO recovery time 0 s. Only 14 genuine losses fell within 3 s after a recovery. A small upper-tail excess
appears 3-6 s after recovery (p90 2.35-2.81 vs control 2.13-2.18); it is not a stale-timer effect at recovery.

## Caveats
1. Absolute rates depend on uncalibrated simulator settings (RLC AM buffer 32 KB, bottleneck 5 Mbps / 30 packets). Do not quote them as real-network rates; a sensitivity run is still needed.
2. The control has no outages (Arm 1 only). The result is "LTE delay variability causes spurious timeouts", not "outages cause them". Arm 2 (outage replay on wired) is not run.
3. Goodput difference mixes radio capacity with the timeout effect.
4. LTE radio-only seed 8 is anomalous (43% of run in loss recovery, 14 of 20 expiries genuine).
5. Definition amendments made during the study: outage attribution uses the logged blockage schedule (the CQI <= 3 rule missed a 3.6 s outage); M1 split into late-ACK and lost-ACK parts; verdict code fixed to treat a zero-control ratio as infinite.
6. RLF thresholds are raised (documented earlier), UEs are stationary, and cross-EPC handover is not modelled.

Figures: fig1_headline.png, fig2_example_trace.png, fig3_rto_around_recovery.png.
Data: analysis/*_per_run.csv, *_compare.csv, *_verdict.json; raw runs in runs/<model>/seed_N/.
Code: scratch/{lte_ue_to_ue.cc, wired_control.cc, run_seeds.sh, analyze_claim_a.py, plot_claim_a.py}.

---

# Sensitivity sweeps (MinRto and RLC buffer)

Wired and LTE+blockage: seeds 1-6. LTE radio-only: seeds 1-5 at every point (seed 6 crashes in ns-3's PDCP
code, `m_dcBit == DATA_PDU`, in every non-baseline radio-only configuration, so it is excluded everywhere).
Brackets in the text are 95% bootstrap CIs. Figures: fig4_minrto_sweep.png, fig5_rlc_sweep.png.

## MinRto (200 ms = Linux default and main study; 1000 ms = RFC 6298's recommended floor)
| MinRto | Wired spurious/h | Radio-only spurious/h | Blockage spurious/h | Radio-only mean wait at genuine loss |
|---|---|---|---|---|
| 200 ms | 0 | 361 [293, 437] | 643 [574, 702] | 359 ms |
| 400 ms | 0 | 49 [22, 73] | 352 [275, 425] | 494 ms |
| 600 ms | 0 | 15 [5, 27] | 214 [179, 254] | 615 ms |
| 1000 ms | 0 | 0 [0, 0] | 128 [98, 157] | 1001 ms |

- Radio-only: a 1 s floor removes every spurious timeout, but at that point the RTO is pinned at the floor
  (mean wait 1001 ms), so the estimator is effectively switched off. Goodput is unchanged (1101 -> 1120 kbit/s)
  and time in loss recovery falls (2.5% -> 0.4%); the cost is latency on the ~80 genuine RTO-recovered losses/hour.
- With blockage: still 128 [98, 157] spurious/h at 1 s, because outages last 1-8 s. No floor below the outage
  length fixes that.
- Wired: zero at every floor; the wait at a genuine loss equals the floor exactly.

## RLC transmit buffer (LTE radio-only; study value 32 KB, ns-3 default 10 KB)
| Buffer | Spurious/h | Genuine RTO/h | Goodput kbit/s | p99 RTT ms |
|---|---|---|---|---|
| 5 KB | 1429 [1353, 1499] | 545 | 421 | 134 |
| 10 KB | 637 [586, 693] | 330 | 709 | 188 |
| 32 KB | 361 [293, 437] | 63 | 1101 | 421 |
| 64 KB | 244 [173, 293] | 71 | 1182 | 712 |
| 128 KB | 144 [107, 173] | 46 | 1211 | 1148 |

The pre-run expectation (spurious timeouts rise with buffer size) was WRONG: they fall as the buffer grows.
Delay does grow with the buffer, as expected: median RTT 77 / 89 / 156 / 263 / 442 ms for 5 / 10 / 32 / 64 / 128 KB.
But the RTO tracks it: median RTO 200 / 208 / 308 / 430 / 618 ms, and the share of time the RTO sits at its 200 ms
floor falls from 63% (5 KB) to 5% (128 KB). Checked and ruled out: (a) backoff chains inflating the count - every
spurious episode contains exactly one spurious expiry at every size; (b) a larger RTO-minus-RTT margin - it is only
120-176 ms at all sizes. Working hypothesis (NOT yet tested): a spurious timeout needs a delay hiccup larger than the
*current absolute RTO*; small buffers keep the RTO at its floor so ordinary radio hiccups trip it, large buffers
raise the RTO above most hiccups. Consistent with the MinRto sweep (raising the floor also removes them). Decisive
test not yet run: 5 KB buffer with MinRto 1000 ms (hypothesis predicts ~0 spurious). Shallow buffers are also worse
on every other axis (more real losses, much lower goodput); deep buffers cost tail RTT (1.1 s p99).
The spurious rate is well above the wired zero at every size, including ns-3's default, so the main result is
not an artefact of the 32 KB setting.

## Caveats
Only 5-6 seeds per point (enough for these large effects, loose for small ones). RLC sweep is radio-only.
The MinRto/RLC settings interact (not crossed here). Mechanism statements above are hypotheses, not tests.

## How realistic are the RLC buffer sizes? (added after a literature check)
No source found reports RLC buffer sizes in bytes. What exists is measured *queueing delay*: Jiang et al.
(NCSU, 3G/4G, ~2012) saw RTTs up to 10 s on AT&T HSPA+ with a true BDP of only ~58 KB; a 2013 e2e-list post by
K. Winstein (Verizon LTE downlink, one TCP download) measured RTT growing to 1-2 s with zero lost pings, and a reader
estimated an implied TCP window of ~2.3 MB at ~15 Mbit/s and 0.8 s RTT (an estimate, informal, 2013). Our 5-128 KB
buffers span ~0.1 s to ~1 s of queueing delay at our ~1 Mbit/s radio rate (p99 RTT 134 ms to 1.15 s), so in DELAY terms
the top of the sweep resembles reported LTE bufferbloat; in BYTES it is far smaller than real eNB buffers because
our radio is slower. Modern operators may use smaller buffers or AQM; no current data was found. Open: extend the sweep
to 256 KB-1 MB (multi-second delays) to cover the reported range.
