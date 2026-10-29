#!/usr/bin/env python3
"""
analyze_claim_a.py — Claim A: does stock RFC 6298 RTO fail on LTE after a fade?

Consumes the CSVs written by scratch/lte_ue_to_ue.cc (tag "lte") and
scratch/wired_control.cc (tag "wired"), one directory per (model, seed).

Per run it computes (see the approved plan for the definitions):
  M1  spurious timeouts per simulated hour   (receiver-side ORACLE)
  M2  RTO slack at genuine losses            (stratified by time since recovery)
  M3  stall fraction                         (share of time in CA_LOSS)
  M4  goodput
  M5  RTO recovery time after an outage

Spurious-timeout oracle
  A timeout is SPURIOUS iff some copy of the retransmitted segment that was sent
  BEFORE the timeout reached the receiver (data was late, not lost). Copies are
  matched with the TCP timestamp option: the sender logs (seq, TSval) for every
  transmission, the receiver logs (seq, TSval) for every delivery. The oracle is
  used for SCORING only and is never a model input.

Outage definition (c)
  Stall   : sender cumulative-ACK gap >= STALL_T seconds.
  Outage  : a stall during which the channel was poor (CQI <= POOR_CQI for at
            least POOR_FRAC of the CQI reports inside the stall). Stalls without
            a poor channel are congestion stalls and are analysed separately.
  Recovery: first ACK advance after the stall. Outage stalls closer together than
            MERGE_S are merged into one episode.
  For the wired arm there is no channel; outages come only from an optional
  --outage-file (the same "start,duration" file given to wired_control).

Usage
  analyze_claim_a.py --single DIR --tag lte|wired [--outage-file F] [--verbose]
  analyze_claim_a.py --lte 'runs/lte/seed_*' --wired 'runs/wired/seed_*' [--out summary.csv]
"""
import argparse
import glob
import json
import os
import sys

import numpy as np
import pandas as pd

# ── Parameters (all recorded in the output; change here, re-run post-processing) ──
STALL_T = 0.200      # s   : ACK-progress gap that counts as a stall (= MinRto floor)
POOR_CQI = 3         #     : channel "poor" means CQI <= this (SINR < 0.2 dB)
POOR_FRAC = 0.5      #     : fraction of CQI reports in a stall that must be poor
MERGE_S = 0.5        # s   : merge outage stalls closer than this into one episode
RTO_TOL = 1.25       #     : RTO "recovered" when <= RTO_TOL * pre-outage RTO
RTO_CENSOR_S = 30.0  # s   : give up looking for RTO recovery after this long
APP_START = 5.0      # s   : applications start (matches both simulators)
BINS = [(0.0, 1.0), (1.0, 3.0), (3.0, 10.0), (10.0, float("inf"))]
BIN_NAMES = ["0-1s", "1-3s", "3-10s", ">10s/none"]


def _read(d, name, tag):
    p = os.path.join(d, f"{name}_{tag}.csv")
    return pd.read_csv(p) if os.path.exists(p) else None


# ───────────────────────────── oracle ─────────────────────────────────────────
TO_COLS = ["t", "expired_rto_us", "last_rtt_us", "srtt_us", "rttvar_us", "seq", "cls"]


def classify_timeouts(tx, retx, rx, to, ackrx=None):
    """One row per RTO expiry, classified by the receiver-side oracle:

      genuine         no copy of the segment sent before the timeout reached the
                      receiver (the data really was lost)
      spurious_late   the data was delivered AND an ACK echoing one of those
                      earlier copies reached the sender AFTER the timeout: the
                      ACK was merely delayed past the RTO (RTO too short)
      spurious_ackloss  the data was delivered but no ACK echoing an earlier copy
                      ever arrived (ACK lost): a longer RTO would not have helped
      spurious        data delivered; late-vs-lost undeterminable (no ack_rx log)
      unknown         the retransmitted segment could not be identified
    """
    if to is None or len(to) == 0:
        return pd.DataFrame(columns=TO_COLS)
    tx_by_seq = {s: g[["timestamp_s", "tsval_ms"]].to_numpy() for s, g in tx.groupby("seq")}
    rx_set = set(zip(rx["seq"].to_numpy(), rx["tsval_ms"].to_numpy()))
    ack_by_echo = None
    if ackrx is not None and len(ackrx):
        ack_by_echo = {e: np.sort(g["timestamp_s"].to_numpy())
                       for e, g in ackrx.groupby("tsecr_ms")}
    retx_t = retx["timestamp_s"].to_numpy()
    retx_seq = retx["seq"].to_numpy()
    rows = []
    for r in to.itertuples(index=False):
        te = r.timestamp_s
        i = np.searchsorted(retx_t, te - 2e-6)
        seq, cls = -1, "unknown"
        if i < len(retx_t) and abs(retx_t[i] - te) <= 2e-6:
            seq = int(retx_seq[i])
            copies = tx_by_seq.get(seq)
            if copies is not None:
                earlier = copies[copies[:, 0] < te - 1e-7]
                if len(earlier):
                    delivered = any((seq, int(ts)) in rx_set for ts in earlier[:, 1])
                    if not delivered:
                        cls = "genuine"
                    elif ack_by_echo is None:
                        cls = "spurious"
                    else:
                        late = False
                        for ts in earlier[:, 1]:
                            arr = ack_by_echo.get(int(ts))
                            if arr is not None and len(arr) and arr[-1] > te:
                                late = True
                                break
                        cls = "spurious_late" if late else "spurious_ackloss"
        rows.append((te, r.expired_rto_us, r.last_rtt_us, r.srtt_us, r.rttvar_us, seq, cls))
    return pd.DataFrame(rows, columns=TO_COLS)


def selftest():
    """Deterministic check of the classifier on hand-built traces with known truth."""
    tx = pd.DataFrame({
        "timestamp_s": [1.0, 2.0, 3.0, 5.0, 5.4, 6.0, 7.0],
        "seq":         [100, 200, 300, 100, 200, 300, 400],
        "size":        [1448] * 7,
        "tsval_ms":    [1000, 2000, 3000, 5000, 5400, 6000, 7000]})
    # seq 100 lost (never received), seq 200 delivered with its ACK lost,
    # seq 300 delivered with its ACK delayed past the timeout
    rx = pd.DataFrame({"timestamp_s": [2.05, 3.05, 5.5, 5.55],
                       "seq": [200, 300, 100, 200], "size": [1448] * 4,
                       "tsval_ms": [2000, 3000, 5000, 5400]})
    retx = pd.DataFrame({"timestamp_s": [5.0, 5.4, 6.0], "seq": [100, 200, 300],
                         "size": [1448] * 3, "tsval_ms": [5000, 5400, 6000]})
    to = pd.DataFrame({"timestamp_s": [5.0, 5.4, 6.0], "node_id": [0] * 3,
                       "expired_rto_us": [4000000, 3400000, 3000000],
                       "last_rtt_us": [50000] * 3, "srtt_us": [50000] * 3, "rttvar_us": [1000] * 3})
    ackrx = pd.DataFrame({"timestamp_s": [6.3], "ack": [400], "tsecr_ms": [3000]})  # echo of seq300's original
    got = classify_timeouts(tx, retx, rx, to, ackrx)["cls"].tolist()
    want = ["genuine", "spurious_ackloss", "spurious_late"]
    assert got == want, f"classifier self-test FAILED: got {got}, want {want}"
    # without ack_rx the delivered cases degrade to plain 'spurious'
    got2 = classify_timeouts(tx, retx, rx, to, None)["cls"].tolist()
    assert got2 == ["genuine", "spurious", "spurious"], got2
    # zero timeouts must not crash the metrics path
    assert len(classify_timeouts(tx, retx, rx, to.iloc[0:0], None)) == 0
    print("selftest OK:", got)


# ───────────────────────────── outages ────────────────────────────────────────
def find_outages(ack, cqi, outage_windows=None, block_windows=None):
    """Return (episodes, congestion_stalls) as lists of (start, end).

    Attribution of a stall to a fade, in order of preference:
      1. block_windows (logged blockage schedule, exact ground truth): the stall
         overlaps an episode for >= POOR_FRAC of its length.  AMENDMENT to the
         agreed definition (c): the CQI<=3 rule missed a textbook 3.6 s outage
         because wideband SINR hovered near 1 dB (CQI 3-5).
      2. cqi (natural fading, no schedule): CQI <= POOR_CQI for >= POOR_FRAC of
         the CQI reports inside the stall (definition (c) as originally agreed).
      3. outage_windows (wired Arm 2 schedule).
    """
    if ack is None or len(ack) == 0:
        return [], []
    t_now = ack["timestamp_s"].to_numpy()
    iat = ack["ack_iat_us"].to_numpy() / 1e6
    big = iat >= STALL_T
    stalls = [(t_now[k] - iat[k], t_now[k]) for k in np.nonzero(big)[0]]
    cq_t = cq_c = None
    if cqi is not None and len(cqi):
        cq_t = cqi["timestamp_s"].to_numpy()
        cq_c = cqi["cqi_index"].to_numpy()
    out, cong = [], []
    for s, e in stalls:
        is_outage = False
        if block_windows is not None:
            ov = sum(max(0.0, min(e, we) - max(s, ws)) for ws, we in block_windows)
            is_outage = ov >= POOR_FRAC * (e - s)
        elif cq_t is not None:
            lo, hi = np.searchsorted(cq_t, [s, e])
            if hi > lo:
                is_outage = (cq_c[lo:hi] <= POOR_CQI).mean() >= POOR_FRAC
        elif outage_windows:
            ov = sum(max(0.0, min(e, we) - max(s, ws)) for ws, we in outage_windows)
            is_outage = ov >= POOR_FRAC * (e - s)
        (out if is_outage else cong).append((s, e))
    merged = []
    for s, e in out:
        if merged and s - merged[-1][1] < MERGE_S:
            merged[-1] = (merged[-1][0], e)
        else:
            merged.append((s, e))
    return merged, cong


def rto_recovery(episodes, rto, end_t):
    """Seconds from each recovery until RTO <= RTO_TOL * pre-outage RTO (censored flag)."""
    if rto is None or len(rto) == 0:
        return []
    rt = rto["timestamp_s"].to_numpy()
    rv = rto["rto_us"].to_numpy() / 1e6
    res = []
    for k, (s, e) in enumerate(episodes):
        i0 = np.searchsorted(rt, s, side="right") - 1
        base = rv[i0] if i0 >= 0 else 1.0
        limit = min(episodes[k + 1][0] if k + 1 < len(episodes) else end_t, e + RTO_CENSOR_S)
        j = np.searchsorted(rt, e, side="right") - 1
        cur = rv[j] if j >= 0 else base
        if cur <= RTO_TOL * base:
            res.append((0.0, False))
            continue
        found = None
        for m in range(max(j + 1, 0), len(rt)):
            if rt[m] > limit:
                break
            if rv[m] <= RTO_TOL * base:
                found = rt[m] - e
                break
        res.append((found, False) if found is not None else (limit - e, True))
    return res


# ───────────────────────────── metrics per run ────────────────────────────────
def analyze_run(d, tag, outage_file=None):
    tx, retx, rx = (_read(d, n, tag) for n in ("tx", "retx", "rx"))
    to, rto = _read(d, "timeout", tag), _read(d, "rto", tag)
    rtt, ack = _read(d, "rtt", tag), _read(d, "ack_iat", tag)
    cong, thr = _read(d, "cong_state", tag), _read(d, "throughput", tag)
    cqi = _read(d, "cqi", tag)
    ackrx = _read(d, "ack_rx", tag)
    blk = _read(d, "blockage", tag)
    block_windows = None
    if blk is not None:
        block_windows = [(r.start_s, r.end_s) for r in blk.itertuples(index=False)]
    if any(x is None for x in (tx, retx, rx, rtt, thr)):
        raise FileNotFoundError(f"missing CSVs in {d} for tag {tag}")

    end_t = float(max(thr["timestamp_s"].max(), rtt["timestamp_s"].max()))
    dur_h = max(end_t - APP_START, 1e-9) / 3600.0

    windows = None
    if outage_file:
        windows = []
        for line in open(outage_file):
            line = line.strip()
            if line and not line.startswith("#"):
                a, b = line.split(",")[:2]
                windows.append((float(a), float(a) + float(b)))

    tos = classify_timeouts(tx, retx, rx, to, ackrx)
    episodes, cong_stalls = find_outages(ack, cqi, windows, block_windows)
    rec_ends = np.array([e for _, e in episodes])
    ep_start = np.array([s for s, _ in episodes])

    # per-timeout context: during an outage, or time since last recovery
    since, in_out = [], []
    for t in tos["t"]:
        io = bool(len(ep_start) and np.any((ep_start <= t) & (t <= rec_ends)))
        prev = rec_ends[rec_ends <= t]
        since.append(float(t - prev.max()) if len(prev) else float("inf"))
        in_out.append(io)
    tos["in_outage"] = pd.Series(in_out, index=tos.index, dtype=bool)
    tos["since_recovery_s"] = pd.Series(since, index=tos.index, dtype=float)
    gen = tos[tos["cls"] == "genuine"].copy()
    gen["slack"] = ((gen["expired_rto_us"] - gen["last_rtt_us"])
                    / gen["last_rtt_us"].replace(0, np.nan)).astype(float)

    m = {
        "dir": d, "tag": tag, "end_t": end_t, "sim_hours": dur_h,
        "n_timeouts": len(tos),
        "n_spurious": int(tos["cls"].str.startswith("spurious").sum()),
        "n_spurious_late": int((tos["cls"] == "spurious_late").sum()),
        "n_spurious_ackloss": int((tos["cls"] == "spurious_ackloss").sum()),
        "n_genuine": int((tos["cls"] == "genuine").sum()),
        "n_unknown": int((tos["cls"] == "unknown").sum()),
        "M1_spurious_per_hour": float(tos["cls"].str.startswith("spurious").sum() / dur_h),
        "M1a_late_ack_per_hour": float((tos["cls"] == "spurious_late").sum() / dur_h),
        "M1b_ack_lost_per_hour": float((tos["cls"] == "spurious_ackloss").sum() / dur_h),
        "timeouts_per_hour": float(len(tos) / dur_h),
        "M2_slack_mean": float(gen["slack"].mean()) if len(gen) else float("nan"),
        "M2_slack_median": float(gen["slack"].median()) if len(gen) else float("nan"),
    }
    # cost side of the MinRto trade-off: how long the sender actually waited at real losses
    m["genuine_per_hour"] = float(len(gen) / dur_h)
    m["genuine_wait_ms_mean"] = (float(gen["expired_rto_us"].mean() / 1e3) if len(gen)
                                 else float("nan"))
    post = gen[~gen["in_outage"]]
    for (lo, hi), nm in zip(BINS, BIN_NAMES):
        sel = post[(post["since_recovery_s"] >= lo) & (post["since_recovery_s"] < hi)]
        m[f"M2_slack_mean[{nm}]"] = float(sel["slack"].mean()) if len(sel) else float("nan")
        m[f"M2_n[{nm}]"] = int(len(sel))
    ino = gen[gen["in_outage"]]
    m["M2_slack_mean[in-outage]"] = float(ino["slack"].mean()) if len(ino) else float("nan")
    m["M2_n[in-outage]"] = int(len(ino))

    # M3: time in CA_LOSS
    loss_t = 0.0
    if cong is not None and len(cong):
        t_enter = None
        for r in cong.itertuples(index=False):
            if r.new_state == "CA_LOSS" and t_enter is None:
                t_enter = max(r.timestamp_s, APP_START)
            elif r.old_state == "CA_LOSS" and t_enter is not None:
                loss_t += max(r.timestamp_s - t_enter, 0.0)
                t_enter = None
        if t_enter is not None:
            loss_t += max(end_t - t_enter, 0.0)
    m["M3_stall_fraction"] = loss_t / max(end_t - APP_START, 1e-9)

    # M4: goodput over [APP_START, end]
    th = thr[thr["timestamp_s"] >= APP_START]
    m["M4_goodput_kbps"] = float(th["throughput_kbps"].mean()) if len(th) else float("nan")

    # M5: RTO recovery time
    rec = rto_recovery(episodes, rto, end_t)
    vals = [v for v, _ in rec]
    m["n_outages"] = len(episodes)
    m["outage_time_frac"] = float(sum(e - s for s, e in episodes) / max(end_t - APP_START, 1e-9))
    m["n_congestion_stalls"] = len(cong_stalls)
    m["n_blockage_episodes"] = len(block_windows) if block_windows is not None else 0
    m["attribution"] = ("blockage-schedule" if block_windows is not None
                        else ("cqi" if cqi is not None else ("outage-file" if windows else "none")))
    m["M5_rto_recovery_median_s"] = float(np.median(vals)) if vals else float("nan")
    m["M5_n_censored"] = int(sum(c for _, c in rec))

    r = rtt[(rtt["rtt_us"] > 0) & (rtt["timestamp_s"] >= APP_START)]["rtt_us"] / 1000.0
    m["rtt_min_ms"] = float(r.min()) if len(r) else float("nan")
    m["rtt_p50_ms"] = float(r.median()) if len(r) else float("nan")
    m["rtt_p99_ms"] = float(r.quantile(0.99)) if len(r) else float("nan")
    m["rtt_max_ms"] = float(r.max()) if len(r) else float("nan")
    m["rtt_p99_over_min"] = m["rtt_p99_ms"] / m["rtt_min_ms"] if len(r) else float("nan")
    m["n_rtt_above_200ms"] = int((r > 200.0).sum())
    return m, tos, episodes


# ───────────────────────────── comparison ─────────────────────────────────────
def boot_ci(x, n=10000, seed=0):
    x = np.asarray([v for v in x if np.isfinite(v)])
    if len(x) == 0:
        return float("nan"), float("nan"), float("nan")
    rng = np.random.default_rng(seed)
    means = rng.choice(x, size=(n, len(x)), replace=True).mean(axis=1)
    return float(x.mean()), float(np.percentile(means, 2.5)), float(np.percentile(means, 97.5))


def compare(lte_runs, wired_runs):
    keys = ["M1_spurious_per_hour", "M1a_late_ack_per_hour", "M1b_ack_lost_per_hour", "timeouts_per_hour", "M2_slack_mean", "M3_stall_fraction",
            "M4_goodput_kbps", "M5_rto_recovery_median_s", "outage_time_frac",
            "rtt_p99_over_min", "M2_slack_mean[0-1s]", "M2_slack_mean[1-3s]",
            "M2_slack_mean[3-10s]", "M2_slack_mean[in-outage]"]
    rows = []
    for k in keys:
        a, b = [r.get(k, np.nan) for r in lte_runs], [r.get(k, np.nan) for r in wired_runs]
        am, alo, ahi = boot_ci(a)
        bm, blo, bhi = boot_ci(b, seed=1)
        # Against a zero control any positive LTE value is infinitely larger (the
        # pre-registered ">= 2x" test is satisfied); only 0/0 or missing data is NaN.
        if not (np.isfinite(am) and np.isfinite(bm)):
            ratio = float("nan")
        elif bm == 0.0:
            ratio = float("inf") if am > 0.0 else float("nan")
        else:
            ratio = am / bm
        rows.append({"metric": k, "lte_mean": am, "lte_lo": alo, "lte_hi": ahi,
                     "wired_mean": bm, "wired_lo": blo, "wired_hi": bhi, "ratio": ratio,
                     "ci_disjoint_lte_worse": bool(np.isfinite(alo) and np.isfinite(bhi) and alo > bhi)})
    df = pd.DataFrame(rows).set_index("metric")
    m1, m2 = df.loc["M1_spurious_per_hour"], df.loc["M2_slack_mean"]
    c1 = bool(m1.ci_disjoint_lte_worse and m1.ratio >= 2.0)
    c2 = bool(m2.ci_disjoint_lte_worse and m2.ratio >= 1.5)
    verdict = {
        "M1_spurious_rate: LTE>=2x control, CIs disjoint": c1,
        "M2_slack: LTE>=1.5x control, CIs disjoint": c2,
        "CLAIM_A_SUPPORTED": c1 or c2,
        "n_lte_runs": len(lte_runs), "n_wired_runs": len(wired_runs),
    }
    return df, verdict


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("--single")
    ap.add_argument("--tag", choices=["lte", "wired"])
    ap.add_argument("--outage-file")
    ap.add_argument("--lte")
    ap.add_argument("--wired")
    ap.add_argument("--out", default="claim_a_summary")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        selftest()
        return
    pd.set_option("display.width", 200, "display.max_columns", 50, "display.float_format", "{:.3f}".format)

    if a.single:
        m, tos, eps = analyze_run(a.single, a.tag, a.outage_file)
        print(json.dumps({k: (None if isinstance(v, float) and not np.isfinite(v) else v)
                          for k, v in m.items()}, indent=1))
        if a.verbose:
            print("\nTimeouts:\n", tos.to_string())
            print("\nOutage episodes (start,end):", [(round(s, 2), round(e, 2)) for s, e in eps])
        return

    def gather(pattern, tag):
        out = []
        for d in sorted(glob.glob(pattern)):
            # run_seeds.sh writes DONE only when the simulator exited cleanly; a
            # crashed run leaves partial/empty CSVs and must not enter the analysis.
            if os.path.isdir(d) and not os.path.exists(os.path.join(d, "DONE")):
                print(f"EXCLUDED (simulator crashed / incomplete): {d}", file=sys.stderr)
                continue
            try:
                out.append(analyze_run(d, tag)[0])
            except (FileNotFoundError, pd.errors.EmptyDataError) as e:
                print("EXCLUDED (unreadable):", d, e, file=sys.stderr)
        return out

    lte, wired = gather(a.lte, "lte"), gather(a.wired, "wired")
    if not lte or not wired:
        sys.exit("need at least one run for each model")
    pd.DataFrame(lte + wired).to_csv(a.out + "_per_run.csv", index=False)
    df, verdict = compare(lte, wired)
    df.to_csv(a.out + "_compare.csv")
    print(df.to_string())
    print(json.dumps(verdict, indent=1))
    json.dump({"verdict": verdict, "params": dict(
        STALL_T=STALL_T, POOR_CQI=POOR_CQI, POOR_FRAC=POOR_FRAC, MERGE_S=MERGE_S,
        RTO_TOL=RTO_TOL, RTO_CENSOR_S=RTO_CENSOR_S)}, open(a.out + "_verdict.json", "w"), indent=1)


if __name__ == "__main__":
    main()
