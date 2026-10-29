#!/usr/bin/env python3
"""
plot_claim_a.py — figures for the claim-A study (run after the 30-seed batches).

  fig1_headline.png          per-run spurious-timeout rate, share of expiries that are
                             spurious, and time lost in loss recovery, per model
  fig2_example_trace.png     one real blockage episode: RTT vs RTO with every RTO expiry
  fig3_rto_around_recovery   RTO relative to its pre-fade value, aligned at recovery
                             (the "stale RTO after a fade" hypothesis)

Reads analysis/*_per_run.csv and runs/<model>/seed_N/*.csv. Entity colours follow the
reference palette slots 1-3 (validated all-pairs, light mode); text always wears ink
tokens, never the series colour.
"""
import glob
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import analyze_claim_a as A  # noqa: E402

SURFACE, INK, INK2, MUTED, GRID = "#fcfcfb", "#0b0b0b", "#52514e", "#8a8984", "#e6e5e1"
COL = {"wired": "#2a78d6", "radio": "#eb6834", "blk": "#1baf7a"}
NAME = {"wired": "Wired control", "radio": "LTE radio-only", "blk": "LTE + blockage"}
OUT = "analysis"


def style(ax):
    ax.set_facecolor(SURFACE)
    for s in ("top", "right", "left"):
        ax.spines[s].set_visible(False)
    ax.spines["bottom"].set_color(GRID)
    ax.tick_params(axis="both", colors=INK2, length=0, labelsize=9)
    ax.grid(axis="y", color=GRID, linewidth=0.8)
    ax.set_axisbelow(True)


def load():
    b = pd.read_csv(f"{OUT}/blk_vs_wired_per_run.csv")
    r = pd.read_csv(f"{OUT}/radio_vs_wired_per_run.csv")
    return {"wired": b[b.tag == "wired"], "radio": r[r.tag == "lte"], "blk": b[b.tag == "lte"]}


def strip(ax, data, col, scale=1.0, fmt="{:.0f}"):
    rng = np.random.default_rng(1)
    for i, k in enumerate(("wired", "radio", "blk")):
        v = data[k][col].to_numpy() * scale
        ax.scatter(i + rng.uniform(-0.18, 0.18, len(v)), v, s=22, color=COL[k], alpha=0.65,
                   edgecolor=SURFACE, linewidth=0.6, zorder=3)
        m, lo, hi = A.boot_ci(v)
        ax.plot([i - 0.3, i + 0.3], [m, m], color=INK, linewidth=2, zorder=4, solid_capstyle="butt")
        ax.plot([i, i], [lo, hi], color=INK, linewidth=1, zorder=4)
        ax.annotate(fmt.format(m), (i + 0.33, m), va="center", ha="left", fontsize=9,
                    color=INK, fontweight="bold")
    ax.set_xticks(range(3))
    ax.set_xticklabels([f"{NAME[k]}\n(n={len(data[k])})" for k in ("wired", "radio", "blk")])
    ax.set_xlim(-0.5, 2.95)
    ax.set_ylim(bottom=0)


def fig1(data):
    fig, axs = plt.subplots(1, 3, figsize=(13.2, 4.6), facecolor=SURFACE)
    for ax in axs:
        style(ax)
    strip(axs[0], data, "M1_spurious_per_hour")
    axs[0].set_title("Spurious RTO expiries per hour", loc="left", fontsize=11,
                     color=INK, fontweight="bold")
    axs[0].set_ylabel("per simulated hour", color=INK2, fontsize=9)

    for i, k in enumerate(("wired", "radio", "blk")):
        d = data[k]
        share = 100 * d.n_spurious.sum() / d.n_timeouts.sum()
        axs[1].bar(i, share, width=0.5, color=COL[k], zorder=3)
        axs[1].annotate(f"{share:.0f}%", (i, share), xytext=(0, 4), textcoords="offset points",
                        ha="center", fontsize=10, color=INK, fontweight="bold")
    axs[1].set_xticks(range(3))
    axs[1].set_xticklabels([NAME[k] for k in ("wired", "radio", "blk")])
    axs[1].set_ylim(0, 110)
    axs[1].set_title("Share of RTO expiries that were spurious", loc="left", fontsize=11,
                     color=INK, fontweight="bold")
    axs[1].set_ylabel("% of expiries (pooled over runs)", color=INK2, fontsize=9)

    strip(axs[2], data, "M3_stall_fraction", scale=100, fmt="{:.1f}%")
    axs[2].set_title("Time lost in timeout recovery", loc="left", fontsize=11,
                     color=INK, fontweight="bold")
    axs[2].set_ylabel("% of run time in CA_LOSS", color=INK2, fontsize=9)
    o = data["radio"].sort_values("M3_stall_fraction").iloc[-1]
    axs[2].annotate("seed %s: 14 of 20 expiries were real losses;\nmean without it = %.1f%%"
                    % (os.path.basename(o["dir"]).split("_")[-1],
                       100 * data["radio"].M3_stall_fraction.sort_values().iloc[:-1].mean()),
                    (1.0, 100 * o.M3_stall_fraction), xytext=(10, -4), textcoords="offset points",
                    fontsize=8.5, color=INK2, va="top", ha="left")

    fig.suptitle("Stock RFC 6298 fires spurious timeouts on LTE and never does on the wired twin",
                 x=0.01, ha="left", fontsize=13, color=INK, fontweight="bold")
    fig.text(0.01, 0.01, "Dots = individual 300 s runs; black bar = mean; whisker = 95% bootstrap CI. "
             "Spurious = the data had already reached the receiver (oracle); 99.8% of LTE spurious "
             "expiries were late ACKs.", fontsize=8.5, color=INK2, ha="left")
    fig.tight_layout(rect=(0, 0.04, 1, 0.94))
    fig.savefig(f"{OUT}/fig1_headline.png", dpi=170, facecolor=SURFACE)
    plt.close(fig)


def _gapped(t, v, max_gap):
    """Insert NaN where consecutive samples are > max_gap apart so no line is drawn across a
    period with no measurements (an outage has no RTT samples)."""
    t, v = np.asarray(t, float), np.asarray(v, float)
    k = np.nonzero(np.diff(t) > max_gap)[0] + 1
    return np.insert(t, k, np.nan), np.insert(v, k, np.nan)


def fig2(seed=1):
    d = f"runs/lte_blk/seed_{seed}"
    blk = pd.read_csv(f"{d}/blockage_lte.csv")
    rtt, rto = pd.read_csv(f"{d}/rtt_lte.csv"), pd.read_csv(f"{d}/rto_lte.csv")
    m, tos, eps = A.analyze_run(d, "lte")
    best, nbest = None, -1
    for e in blk.itertuples(index=False):          # illustrative pick: most expiries
        n = ((tos.t >= e.start_s - 1) & (tos.t <= e.end_s + 1)).sum()
        if n > nbest and e.end_s - e.start_s > 1.5:
            best, nbest = e, n
    # Window stops 3 s after recovery so ONE episode is shown; any other blockage that still
    # overlaps the window is shaded (lighter) so no expiry ever appears without its cause.
    t0, t1 = best.start_s - 5, best.end_s + 3
    fig, ax = plt.subplots(figsize=(12.4, 5.0), facecolor=SURFACE)
    for e in blk.itertuples(index=False):
        if e.end_s > t0 and e.start_s < t1 and not (e.start_s == best.start_s):
            ax.axvspan(max(e.start_s, t0), min(e.end_s, t1), color="#efeeea", zorder=0)
    style(ax)
    ax.axvspan(best.start_s, best.end_s, color="#e4e3df", zorder=0)
    ax.text(best.start_s + 0.08, 2380, f"blockage {best.depth_db:.0f} dB, "
            f"{best.end_s - best.start_s:.1f} s\n(no RTT samples: ACKs held)",
            fontsize=9, color=INK2, va="top")
    r = rtt[(rtt.timestamp_s >= t0) & (rtt.timestamp_s <= t1)]
    tt, vv = _gapped(r.timestamp_s, r.rtt_us / 1e3, 0.3)
    ax.plot(tt, vv, color=INK2, linewidth=1.0, label="measured RTT", zorder=3)
    q = rto[(rto.timestamp_s >= t0 - 3) & (rto.timestamp_s <= t1)]
    ax.step(q.timestamp_s, q.rto_us / 1e3, where="post", color=COL["blk"], linewidth=2.2,
            label="RTO (how long the sender waits)", zorder=4)
    w = tos[(tos.t >= t0) & (tos.t <= t1)]
    sp, ge = w[w.cls.str.startswith("spurious")], w[w.cls == "genuine"]
    ax.scatter(sp.t, np.full(len(sp), 40), marker="v", s=70, color=INK, edgecolor=SURFACE,
               linewidth=0.8, zorder=5, label=f"RTO expiry, spurious ({len(sp)}): data had arrived")
    ax.scatter(ge.t, np.full(len(ge), 40), marker="o", s=60, facecolor=SURFACE, edgecolor=INK,
               linewidth=1.4, zorder=5, label=f"RTO expiry, genuine ({len(ge)}): data lost")
    ytop = 2400
    ax.set_ylim(0, ytop)
    ax.set_xlim(t0, t1)
    hi = q[q.rto_us / 1e3 > ytop]
    if len(hi):
        ax.text(t1 - 0.25, ytop - 60, f"RTO keeps backing off, reaching {q.rto_us.max() / 1e6:.1f} s\n"
                "(axis clipped at 2.4 s)", fontsize=8.5, color=INK2, va="top", ha="right")
    ax.set_xlabel("simulated time (s)", color=INK2, fontsize=9)
    ax.set_ylabel("milliseconds", color=INK2, fontsize=9)
    ax.legend(loc="upper left", frameon=False, fontsize=9, labelcolor=INK)
    ax.set_title("A blockage makes the timer fire repeatedly while the ACK is only delayed "
                 "(LTE + blockage, seed %d)" % seed, loc="left", fontsize=12, color=INK,
                 fontweight="bold")
    fig.text(0.01, 0.01, "Illustrative: the blockage with the most expiries among this run's episodes "
             "(one episode shown). Every expiry shown is spurious: the data had already arrived.",
             fontsize=8.5, color=INK2)
    fig.tight_layout(rect=(0, 0.03, 1, 1))
    fig.savefig(f"{OUT}/fig2_example_trace.png", dpi=170, facecolor=SURFACE)
    plt.close(fig)
    return best


def fig3():
    """RTO / pre-fade RTO, aligned at recovery, vs a matched control: the identical ratio at
    random times with no blockage nearby (same gap between 'before' and 'now'). The control
    shows how far the RTO drifts anyway, so only the excess over it is attributable to a fade."""
    grid = np.arange(-3.0, 6.01, 0.1)
    curves, ctrl, lens = [], [], []
    rng = np.random.default_rng(7)
    for d in sorted(glob.glob("runs/lte_blk/seed_*")):
        if not os.path.exists(os.path.join(d, "DONE")):
            continue
        ack, rto, blk = (pd.read_csv(f"{d}/{n}_lte.csv") for n in ("ack_iat", "rto", "blockage"))
        wins = [(r.start_s, r.end_s) for r in blk.itertuples()]
        eps, _ = A.find_outages(ack, None, None, wins)
        rt, rv = rto.timestamp_s.to_numpy(), rto.rto_us.to_numpy() / 1e3
        at = lambda t: rv[max(np.searchsorted(rt, t, side="right") - 1, 0)]  # noqa: E731
        end_t = float(rt.max())
        for s0, e0 in eps:
            base = at(s0 - 0.01)
            curves.append([at(e0 + t) / base for t in grid])
            lens.append(e0 - s0)
            for _ in range(200):                          # matched control time, no blockage near it
                r0 = rng.uniform(A.APP_START + 12 + (e0 - s0), end_t - 7)
                if all(r0 + 7 < ws - 0 or r0 - (e0 - s0) - 3 > we + 0 for ws, we in wins):
                    cb = at(r0 - (e0 - s0) - 0.01)
                    ctrl.append([at(r0 + t) / cb for t in grid])
                    break
    c, k = np.array(curves), np.array(ctrl)
    med, q1, q3, p90 = np.median(c, 0), np.percentile(c, 25, 0), np.percentile(c, 75, 0), np.percentile(c, 90, 0)
    kmed, kp90 = np.median(k, 0), np.percentile(k, 90, 0)
    fig, ax = plt.subplots(figsize=(11.2, 5.0), facecolor=SURFACE)
    style(ax)
    ax.axvspan(grid[0], 0, color="#e4e3df", zorder=0)
    ax.text(grid[0] + 0.1, 0.12, "inside the outage", fontsize=9, color=INK2)
    ax.fill_between(grid, q1, q3, color=COL["blk"], alpha=0.25, linewidth=0, zorder=2,
                    label="after a blockage: middle 50% of episodes")
    ax.plot(grid, med, color=COL["blk"], linewidth=2.4, zorder=5, label="after a blockage: median")
    ax.plot(grid, p90, color=COL["blk"], linewidth=1.3, linestyle=(0, (4, 3)), zorder=4,
            label="after a blockage: 90th percentile")
    ax.plot(grid, kp90, color=INK2, linewidth=1.3, linestyle=(0, (1, 2)), zorder=3,
            label="control (no blockage): 90th percentile")
    ax.plot(grid, kmed, color=INK2, linewidth=1.6, zorder=3, label="control (no blockage): median")
    ax.axhline(1.0, color=INK, linewidth=0.8, zorder=1)
    ax.axvline(0, color=INK2, linewidth=1.0, linestyle=":", zorder=3)
    ax.text(0.08, 5.75, "channel recovers", fontsize=9, color=INK2, va="top")
    ax.set_ylim(0, 6)
    ax.set_xlim(grid[0], grid[-1])
    ax.set_xlabel("seconds relative to recovery (first ACK advance after the outage)",
                  color=INK2, fontsize=9)
    ax.set_ylabel("RTO ÷ its value before the fade  (1.0 = unchanged)", color=INK2, fontsize=9)
    ax.legend(loc="upper right", bbox_to_anchor=(1.0, 0.93), frameon=False, fontsize=8.5,
              labelcolor=INK)
    ax.set_title(f"After the channel recovers the median RTO is back to normal at once "
                 f"({len(c)} outage episodes, 29 runs)", loc="left", fontsize=12, color=INK,
                 fontweight="bold")
    fig.tight_layout()
    fig.savefig(f"{OUT}/fig3_rto_around_recovery.png", dpi=170, facecolor=SURFACE)
    plt.close(fig)
    return c, k, grid


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    data = load()
    fig1(data)
    print("fig1 done")
    b = fig2(seed=1)
    print("fig2 done: episode", b)
    c, k, grid = fig3()
    i0 = np.searchsorted(grid, 0.0)
    print("fig3 done: episodes=%d controls=%d" % (len(c), len(k)))
    for nm, off in (("at recovery", 0), ("+1 s", 10), ("+3 s", 30), ("+6 s", 60)):
        print("  %-12s blockage median %.2f p90 %.2f | control median %.2f p90 %.2f"
              % (nm, np.median(c[:, i0 + off]), np.percentile(c[:, i0 + off], 90),
                 np.median(k[:, i0 + off]), np.percentile(k[:, i0 + off], 90)))
