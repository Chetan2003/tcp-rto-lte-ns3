#!/usr/bin/env python3
"""
analyze_sweep.py — MinRto and RLC-buffer sensitivity of the claim-A result.

Reads runs/<model>[_mr<ms>|_rlc<KB>]/seed_N (seeds 1-6; the 200 ms / 32 KB baselines are the
main-study runs restricted to the same seeds), computes per-run metrics with
analyze_claim_a.analyze_run, caches them in analysis/sweep_per_run.csv, and draws:

  fig4_minrto_sweep.png   spurious expiries/hour, wait at genuine loss, and the trade-off
  fig5_rlc_sweep.png      spurious/hour, genuine timeouts/hour and goodput vs RLC buffer

Entity colours match the main figures (wired blue, LTE radio-only orange, LTE+blockage aqua).
"""
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import analyze_claim_a as A  # noqa: E402
import plot_claim_a as P  # noqa: E402

SEEDS = range(1, 7)
CACHE = "analysis/sweep_per_run.csv"
MINRTO = [200, 400, 600, 1000]
RLC = [5, 10, 32, 64, 128]
MODELS = [("wired", "wired", "wired"), ("lte", "lte", "radio"), ("lte_blk", "lte", "blk")]
KEYS = ["M1_spurious_per_hour", "genuine_per_hour", "genuine_wait_ms_mean", "M2_slack_mean",
        "M3_stall_fraction", "M4_goodput_kbps", "rtt_p99_ms", "timeouts_per_hour"]


def configs():
    out = []
    for m in MINRTO:
        for model, tag, ent in MODELS:
            out.append(("minrto", model, tag, ent, m, model if m == 200 else f"{model}_mr{m}"))
    for kb in RLC:
        out.append(("rlc", "lte", "lte", "radio", kb, "lte" if kb == 32 else f"lte_rlc{kb}"))
    return out


def per_run():
    cache = pd.read_csv(CACHE) if os.path.exists(CACHE) else pd.DataFrame()
    have = set(cache["dir"]) if len(cache) else set()
    rows = []
    for sweep, model, tag, ent, x, base in configs():
        for s in SEEDS:
            d = f"runs/{base}/seed_{s}"
            if not os.path.exists(f"{d}/DONE") or d in have:
                continue
            if ent == "radio" and s == 6:
                continue
            try:
                m = A.analyze_run(d, tag)[0]
            except Exception as e:  # noqa: BLE001
                print("skip", d, e, file=sys.stderr)
                continue
            m.update(sweep=sweep, model=model, entity=ent, x=x, seed=s)
            rows.append(m)
            print("analysed", d, flush=True)
    if rows:
        cache = pd.concat([cache, pd.DataFrame(rows)], ignore_index=True)
        cache.to_csv(CACHE, index=False)
    return cache


def agg(df, sweep, ent, xs):
    out = []
    for x in xs:
        d = df[(df.sweep == sweep) & (df.entity == ent) & (df.x == x)]
        if ent == "radio":        # seed 6 crashes inside ns-3 (PDCP assert) in every non-baseline
            d = d[d.seed != 6]    # radio-only configuration, so all radio-only points use seeds 1-5
        row = {"x": x, "n": len(d)}
        for k in KEYS:
            v = d[k].to_numpy(float) if len(d) else np.array([])
            m, lo, hi = A.boot_ci(v) if len(v) else (np.nan,) * 3
            row[k], row[k + "_lo"], row[k + "_hi"] = m, lo, hi
        out.append(row)
    return pd.DataFrame(out)


def line(ax, a, key, ent, scale=1.0, label=True, off=(8, 0)):
    a = a[a.n > 0]
    if a.empty:
        return
    c = P.COL[ent]
    ax.plot(a.x, a[key] * scale, color=c, linewidth=2, marker="o", markersize=7,
            markeredgecolor=P.SURFACE, markeredgewidth=1.2, zorder=3)
    ax.vlines(a.x, a[key + "_lo"] * scale, a[key + "_hi"] * scale, color=c, linewidth=1.2, zorder=2)
    if label:
        ax.annotate(P.NAME[ent], (a.x.iloc[-1], a[key].iloc[-1] * scale), xytext=off,
                    textcoords="offset points", fontsize=9, color=P.INK, va="center")


def fig4(df):
    fig, axs = plt.subplots(1, 3, figsize=(14.2, 4.8), facecolor=P.SURFACE)
    for ax in axs:
        P.style(ax)
    tab = {ent: agg(df, "minrto", ent, MINRTO) for _, _, ent in MODELS}
    for ent in ("wired", "radio", "blk"):
        off0 = {"wired": (8, 15), "radio": (8, 3), "blk": (8, 0)}[ent]
        off1 = {"wired": (8, -13), "radio": (8, 3), "blk": (8, 0)}[ent]
        line(axs[0], tab[ent], "M1_spurious_per_hour", ent, off=off0)
        line(axs[1], tab[ent], "genuine_wait_ms_mean", ent, off=off1)
        a = tab[ent][tab[ent].n > 0]
        if len(a):
            axs[2].plot(a.genuine_wait_ms_mean, a.M1_spurious_per_hour, color=P.COL[ent], linewidth=2,
                        marker="o", markersize=7, markeredgecolor=P.SURFACE, markeredgewidth=1.2, zorder=3)
            if ent != "wired":
                for r in a.itertuples():
                    axs[2].annotate(f"{int(r.x)}", (r.genuine_wait_ms_mean, r.M1_spurious_per_hour),
                                    xytext=(0, 9), textcoords="offset points", ha="center",
                                    fontsize=8, color=P.INK2)
            lab = {"wired": ((a.genuine_wait_ms_mean.iloc[0], 0), (0, 12), "left"),
                   "radio": ((a.genuine_wait_ms_mean.iloc[0], a.M1_spurious_per_hour.iloc[0]),
                             (-10, 0), "right"),
                   "blk": ((a.genuine_wait_ms_mean.iloc[-1], a.M1_spurious_per_hour.iloc[-1]),
                           (0, -16), "center")}[ent]
            axs[2].annotate(P.NAME[ent], lab[0], xytext=lab[1], textcoords="offset points",
                            ha=lab[2], fontsize=9, color=P.INK)
    for ax in axs[:2]:
        ax.set_xticks(MINRTO)
        ax.set_xlim(150, 1250)
        ax.set_xlabel("TCP minimum RTO (ms)", color=P.INK2, fontsize=9)
    axs[0].set_ylabel("spurious RTO expiries per hour", color=P.INK2, fontsize=9)
    axs[0].set_title("Does a higher floor remove the spurious timeouts?", loc="left", fontsize=11,
                     color=P.INK, fontweight="bold")
    axs[1].set_ylabel("mean RTO waited at a genuine loss (ms)", color=P.INK2, fontsize=9)
    axs[1].set_title("What it costs: waiting longer on real losses", loc="left", fontsize=11,
                     color=P.INK, fontweight="bold")
    axs[2].set_xlabel("mean wait at a genuine loss (ms)", color=P.INK2, fontsize=9)
    axs[2].set_ylabel("spurious RTO expiries per hour", color=P.INK2, fontsize=9)
    axs[2].set_title("The trade-off a learned RTO must beat", loc="left", fontsize=11,
                     color=P.INK, fontweight="bold")
    axs[2].text(0.98, 0.97, "numbers = minimum RTO (ms)\nlower-left is better", transform=axs[2].transAxes,
                ha="right", va="top", fontsize=8.5, color=P.INK2)
    for ax in axs:
        ax.set_ylim(bottom=0)
    fig.suptitle("MinRto sensitivity (wired/blockage: seeds 1-6, radio-only: seeds 1-5; whiskers = 95% bootstrap CI)", x=0.01,
                 ha="left", fontsize=13, color=P.INK, fontweight="bold")
    fig.tight_layout(rect=(0, 0, 0.96, 0.94))
    fig.savefig("analysis/fig4_minrto_sweep.png", dpi=170, facecolor=P.SURFACE)
    plt.close(fig)


def fig5(df):
    a = agg(df, "rlc", "radio", RLC)
    fig, axs = plt.subplots(1, 3, figsize=(14.2, 4.6), facecolor=P.SURFACE)
    spec = [("M1_spurious_per_hour", "spurious RTO expiries per hour", 1.0,
             "Spurious timeouts vs RLC buffer"),
            ("genuine_per_hour", "genuine RTO expiries per hour (real losses)", 1.0,
             "Do shallow buffers turn delay into loss?"),
            ("M4_goodput_kbps", "goodput (kbit/s)", 1.0, "Goodput vs RLC buffer")]
    for ax, (k, yl, sc, ti) in zip(axs, spec):
        P.style(ax)
        line(ax, a, k, "radio", sc, label=False)
        ax.set_xscale("log")
        ax.set_xticks(RLC)
        ax.set_xticklabels([str(v) for v in RLC])
        ax.minorticks_off()
        ax.set_xlabel("RLC transmit buffer (KB)   [ns-3 default 10, study 32]", color=P.INK2, fontsize=9)
        ax.set_ylabel(yl, color=P.INK2, fontsize=9)
        ax.set_title(ti, loc="left", fontsize=11, color=P.INK, fontweight="bold")
        ax.set_ylim(bottom=0)
    fig.suptitle("RLC buffer sensitivity, LTE radio-only (seeds 1-5 per point; whiskers = 95% bootstrap CI)",
                 x=0.01, ha="left", fontsize=13, color=P.INK, fontweight="bold")
    fig.tight_layout(rect=(0, 0, 1, 0.94))
    fig.savefig("analysis/fig5_rlc_sweep.png", dpi=170, facecolor=P.SURFACE)
    plt.close(fig)


if __name__ == "__main__":
    df = per_run()
    pd.set_option("display.width", 220, "display.float_format", "{:.1f}".format)
    for sweep, ent_list, xs in (("minrto", ("wired", "radio", "blk"), MINRTO), ("rlc", ("radio",), RLC)):
        for ent in ent_list:
            t = agg(df, sweep, ent, xs)
            print(f"\n== {sweep}  {P.NAME[ent]}")
            print(t[["x", "n", "M1_spurious_per_hour", "M1_spurious_per_hour_lo", "M1_spurious_per_hour_hi",
                     "genuine_per_hour", "genuine_wait_ms_mean", "M3_stall_fraction", "M4_goodput_kbps"]]
                  .to_string(index=False))
    fig4(df)
    fig5(df)
    print("\nfigures written")
