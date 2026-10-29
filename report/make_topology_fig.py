#!/usr/bin/env python3
"""Topology diagram for the mid-semester report: LTE testbed (a) and wired control twin (b)."""
import os

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch

SURFACE, INK, INK2, GRID = "#fcfcfb", "#0b0b0b", "#52514e", "#e6e5e1"
BOX, ACCENT = "#f1f0ec", "#eb6834"
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "figures")
os.makedirs(OUT, exist_ok=True)


def box(ax, x0, x1, y, text, h=11, fc=BOX, ec=INK2, lw=1.2, fs=9):
    ax.add_patch(FancyBboxPatch((x0, y - h / 2), x1 - x0, h, boxstyle="round,pad=0.3,rounding_size=1.2",
                                facecolor=fc, edgecolor=ec, linewidth=lw, zorder=3))
    ax.text((x0 + x1) / 2, y, text, ha="center", va="center", fontsize=fs, color=INK, zorder=4,
            linespacing=1.25)


def link(ax, p, q, label, lpos, dashed=False, color=INK2, lw=1.6, fs=8, ha="center", va="bottom"):
    ax.annotate("", xy=q, xytext=p, zorder=2,
                arrowprops=dict(arrowstyle="<->", color=color, linewidth=lw,
                                linestyle=(0, (4, 3)) if dashed else "-", shrinkA=0, shrinkB=0))
    ax.text(lpos[0], lpos[1], label, ha=ha, va=va, fontsize=fs, color=INK if color == INK2 else color,
            linespacing=1.25, zorder=4)


def panel(ax, title, ue, enb, gw, link_radio, link_s1a, link_s1b, link_core, bs_name, gw_name, foot):
    ax.set_xlim(0, 100)
    ax.set_ylim(0, 61)
    ax.axis("off")
    ax.set_facecolor(SURFACE)
    ax.text(0, 60.5, title, fontsize=11, fontweight="bold", color=INK, va="top")
    yt, yb = 42, 15
    # top row: sender side -> EPC-A / GW-A, plus the core server
    box(ax, 1, 22.5, yt, ue[0], h=14, fs=8.6)
    box(ax, 41, 55, yt, enb[0])
    box(ax, 68, 84, yt, gw[0], h=13)
    box(ax, 90, 100, yt, "core\nserver", h=9, fs=8.5)
    link(ax, (23.1, yt), (40.4, yt), link_radio[0], (31.8, yt + 1.8), dashed=link_radio[1])
    link(ax, (55.6, yt), (67.4, yt), link_s1a, (61.5, yt + 1.8))
    link(ax, (84.6, yt), (89.4, yt), link_core, (95, yt - 5.2), fs=7.5, va="top")
    # bottom row: receiver side
    box(ax, 1, 22.5, yb, ue[1], h=14, fs=8.6)
    box(ax, 41, 55, yb, enb[1])
    box(ax, 68, 84, yb, gw[1], h=13)
    link(ax, (23.1, yb), (40.4, yb), link_radio[0], (31.8, yb + 1.8), dashed=link_radio[1])
    link(ax, (55.6, yb), (67.4, yb), link_s1b, (61.5, yb + 1.8))
    # SGi bottleneck between the two gateways (the shared, monitored bottleneck)
    link(ax, (76, yt - 6.9), (76, yb + 6.9), "", (0, 0), color=ACCENT, lw=3.0)
    ax.text(78, (yt + yb) / 2, "SGi / inter-gateway link\n5 Mbps, 1 ms, 30-packet DropTail\n(monitored bottleneck)",
            fontsize=8, color=ACCENT, va="center", ha="left", fontweight="bold", linespacing=1.25)
    ax.text(0, 1.0, foot, fontsize=8, color=INK2, va="bottom", ha="left", linespacing=1.3)


fig, axs = plt.subplots(2, 1, figsize=(8.2, 9.2), facecolor=SURFACE)
panel(axs[0], "(a) LTE UE-to-UE testbed: two independent LTE networks (EPC-A and EPC-B)",
      ("UE-A\n(monitored TCP sender)\n+ 4 background UEs\n(random waypoint)",
       "UE-B\n(TCP receiver)\n+ 4 background UEs\n(random waypoint)"),
      ("eNB1\n(0, 0, 30 m)", "eNB2\n(1000, 0, 30 m)"),
      ("SGW / PGW-A\nEPC-A\nUE pool 7.0.0.0/8", "SGW / PGW-B\nEPC-B\nUE pool 9.0.0.0/8"),
      ("radio link:\nPF scheduler, HARQ,\nRLC-AM, fading,\nblockage", True),
      "S1-U 1 Gbps, 2 ms", "S1-U 1 Gbps, 5 ms", "100 Mbps\n1 ms", "eNB", "PGW",
      "Monitored flow: TCP NewReno data UE-A to UE-B, ACKs on the reverse path. Background: UDP on/off\nbetween the core server and the other UEs (downlink 1 Mbps each, UE-B uplink 512 kbps).")
panel(axs[1], "(b) Wired control twin: same network with the radio replaced by 100 Mbps LANs",
      ("hostA0\n(monitored TCP sender)\n+ 4 background hosts\non LAN-A", "hostB0\n(TCP receiver)\n+ 4 background hosts\non LAN-B"),
      ("BS-A\n(router)", "BS-B\n(router)"),
      ("GW-A\n(router)", "GW-B\n(router)"),
      ("CSMA LAN\n100 Mbps, 10 us", False),
      "wired 1 Gbps, 2 ms", "wired 1 Gbps, 5 ms", "100 Mbps\n1 ms", "BS", "GW",
      "Identical TCP settings, traffic mix, bottleneck and wired delays as (a); only the radio access is\nremoved. Outage hook (scheduled link-down) reserved for a later control arm.")
fig.tight_layout(h_pad=2.0)
fig.savefig(os.path.join(OUT, "fig_topology.png"), dpi=200, facecolor=SURFACE)
print("written", os.path.join(OUT, "fig_topology.png"))
