/*
 * lte_ue_to_ue.cc — TCP RTO Research Simulation: Mobile LTE UE-to-UE Topology
 * ns-3.46 | Author: TCP-RTO Research
 *
 * ═══════════════════════════════════════════════════════════════════════════
 * TOPOLOGY
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *  UE-A (50,0)  ──[eNB1 (0,0)]──[S1/2ms]── PGW-A ──[SGi P2P]── PGW-B ──[S1/5ms]──[eNB2 (1000,0)]── UE-B (950,0)
 *  bgUE-1..4    ──[eNB1]────────────────────┘                    └──[eNB2]── bgUE-5..8
 *                                            │
 *                                       coreServer (off PGW-A, [P2P 5Mbps/1ms/30p])
 *
 *  TWO INDEPENDENT EPCs (two PGWs, two subnets — no hairpin):
 *    EPC-A: eNB1 + UE-A + bgUE-1..4, UE pool 7.0.0.0/8, PGW-A
 *    EPC-B: eNB2 + UE-B + bgUE-5..8, UE pool 9.0.0.0/8 (rebased — see below), PGW-B
 *    PGW-A ↔ PGW-B connected via a P2P link standing in for the real-world
 *    SGi interface (a PGW's link out to the internet/inter-carrier
 *    interconnect). UE-A and UE-B are on genuinely different subnets;
 *    traffic between them must cross the inter-PGW SGi link instead of
 *    hairpinning at a single shared PGW.
 *
 *  Monitored TCP path (UE-A → UE-B):
 *    UE-A → eNB1 (UL) → PGW-A → [SGi] → PGW-B → eNB2 (DL) → UE-B
 *    ACK: UE-B → eNB2 (UL) → PGW-B → [SGi] → PGW-A → eNB1 (DL) → UE-A
 *
 *  NOTE: cross-EPC X2/handover is not modeled — eNB1 and eNB2 now belong to
 *  independent EPCs (separate MME/PGW), and X2 requires a shared MME.
 *
 * ═══════════════════════════════════════════════════════════════════════════
 * UE PLACEMENT AND MOBILITY
 * ═══════════════════════════════════════════════════════════════════════════
 *   eNB1         : (0,    0,    30m) — fixed
 *   eNB2         : (1000, 0,    30m) — fixed
 *   UE-A         : (50,   0,    1.5m) — stationary, attached to eNB1
 *   UE-B         : (950,  0,    1.5m) — stationary, attached to eNB2
 *   bgUE-1..4    : RandomWaypoint, X∈[-300,+300], Y∈[-300,+300] — attached to eNB1
 *   bgUE-5..8    : RandomWaypoint, X∈[700,1300],  Y∈[-300,+300] — attached to eNB2
 *
 * ═══════════════════════════════════════════════════════════════════════════
 * TRAFFIC FLOWS
 * ═══════════════════════════════════════════════════════════════════════════
 *   UE-A → UE-B         : TCP BulkSend, port 5000 — MONITORED
 *   UE-B → coreServer   : UDP OnOff, port 6000, 512Kbps, Poisson ON=10s OFF=5s
 *   coreServer → bgUE-1 : UDP OnOff, port 7000, 1Mbps, Poisson ON=15s OFF=10s
 *   coreServer → bgUE-2 : UDP OnOff, port 7001, 1Mbps (same Poisson params)
 *   ...
 *   coreServer → bgUE-8 : UDP OnOff, port 7007, 1Mbps
 *
 * ═══════════════════════════════════════════════════════════════════════════
 * IP ADDRESSING
 * ═══════════════════════════════════════════════════════════════════════════
 *   EPC-A UE addresses : 7.0.0.x/8  (UE-A, bgUE-1..4 — assigned by EpcHelper)
 *   EPC-B UE addresses : 9.0.0.x/8  (UE-B, bgUE-5..8 — rebased manually from
 *                        the EpcHelper's hardcoded 7.0.0.0/8 default, since
 *                        NoBackhaulEpcHelper has no public API to change its
 *                        UE pool base. Rebase happens before Attach(), since
 *                        ActivateEpsBearer() reads the UE's live IP
 *                        synchronously inside Attach().)
 *   Inter-PGW SGi link : 15.0.0.0/30 (PGW-A=15.0.0.1, PGW-B=15.0.0.2)
 *   P2P PGW-A↔coreServer: 10.1.0.0/24  (PGW-A=10.1.0.1, coreServer=10.1.0.2)
 *
 * ═══════════════════════════════════════════════════════════════════════════
 * CONGESTION AND FADING LABELS
 * ═══════════════════════════════════════════════════════════════════════════
 *   Congestion: P2P backhaul queue depth > queueThreshold (default 10p)
 *               → label=1 in features_lte.csv
 *               When all 4 eNB2 bg UEs ON: 4Mbps into 5Mbps link → ~80% load
 *               → queue builds up on coreServer outgoing interface
 *               → eNB2 DL PRBs shared among 5 UEs (UE-B + 4 bg) → RTT rises
 *
 *   Fading     : Synthetic CQI drop (3–6) / RSRP (-120 to -110 dBm) on UE-A
 *               → label=2; Poisson events with mean interval 30s, duration 5–15s
 *
 *   event_label: 2 if fading (takes priority), 1 if congestion, 0 otherwise
 *
 * ═══════════════════════════════════════════════════════════════════════════
 * OUTPUT FILES (10 total)
 * ═══════════════════════════════════════════════════════════════════════════
 *   rtt_lte.csv        — timestamp_s, node_id, rtt_us
 *   cwnd_lte.csv       — timestamp_s, node_id, cwnd_bytes
 *                        +ve = CWND update;  −ve = ssthresh sentinel
 *   rto_lte.csv        — timestamp_s, node_id, rto_us
 *   ack_iat_lte.csv    — timestamp_s, node_id, ack_iat_us
 *   queue_lte.csv      — timestamp_s, queue_depth_packets (coreServer→PGW queue)
 *   conn_lte.csv       — timestamp_s, node_id, event
 *                        events: congestion_start, congestion_end,
 *                                fading_start, fading_end
 *   cqi_lte.csv        — timestamp_s, rsrp_dbm, sinr_db, cqi_index (on UE-A)
 *   handover_lte.csv   — timestamp_s, imsi, cell_id, rnti
 *   throughput_lte.csv — timestamp_s, rx_bytes_total, throughput_kbps
 *                        UE-B TCP sink, sampled every 500ms
 *   features_lte.csv   — timestamp_s, rtt_us, cwnd_bytes, srtt_us, rttvar_us,
 *                        rto_us, ack_iat_us, queue_depth, cqi, sinr_db,
 *                        throughput_kbps, is_congestion, is_fading, event_label
 *
 * ═══════════════════════════════════════════════════════════════════════════
 * PROTOCOL EVENTS
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * LTE Attach Procedure:
 *   UE sends RRC Connection Request → eNB responds with RRC Connection Setup
 *   UE sends RRC Connection Setup Complete → MME auth/key derivation → bearer setup
 *   GTP-U tunnel established: eNB-TEID ↔ PGW-TEID; UE gets EPC IP (7.0.0.x)
 *
 * LTE Air Interface (UL/DL):
 *   OFDMA (DL): eNB allocates resource blocks (RBs) per UE per subframe (1ms)
 *   SC-FDMA (UL): UE transmits on allocated RBs
 *   Scheduler (PfFfMacScheduler): proportional-fair; balances throughput × fairness
 *   When bg UEs active on eNB2: fewer RBs available for UE-B DL TCP data
 *   → TCP receive buffer fills slower → ACKs delayed → RTT rises at UE-A
 *
 * LTE HARQ:
 *   Failed transmission retried up to 4 times within 8ms HARQ RTT
 *   HARQ retransmissions add latency visible as RTT jitter
 *   RLC UM (no reordering) used here to avoid AM window stall under load
 *
 * TCP Slow Start / Congestion Avoidance: (see wired_lan_to_lan.cc)
 * RFC 6298 SRTT/RTTVAR: computed locally (ns-3 does not expose them)
 * RTO backoff: doubles on each timeout, reset on first successful ACK
 *
 * CQI Measurement:
 *   UE-A PHY measures DL RSRP and SINR each subframe; converts SINR to CQI
 *   3GPP TS 36.213 Table 7.2.3-1 SINR→CQI mapping (CQI 1–15)
 *   ReportCurrentCellRsrpSinr trace fires on each CQI update
 *
 * Fading Simulation:
 *   Synthetic: override g_featCqi/g_featSinr with low values (CQI 3–6)
 *   CQI callback suppressed during fading window (g_fadingOverride = true)
 *   Real TCP socket NOT disrupted — label assigned by event timing
 *   Fading causes UE-A to see lower MCS → fewer bits per RB → RTT rises
 *
 * ═══════════════════════════════════════════════════════════════════════════
 * NS-3.46 CONSTRAINTS HANDLED
 * ═══════════════════════════════════════════════════════════════════════════
 *   1. EpcHelper installs internet stack on PGW/SGW internally — do NOT
 *      call internet.Install(pgw).
 *   2. Set epcHelper S1uLinkDelay BEFORE each InstallEnbDevice() call.
 *   3. Two independent EpcHelper/LteHelper instances (EPC-A, EPC-B) — no
 *      AddX2Interface(), since X2/handover requires a shared MME.
 *   3b. NoBackhaulEpcHelper hardcodes its UE pool to 7.0.0.0/8 with no
 *      public setter — EPC-B's addresses are manually rebased to 9.0.0.0/8
 *      (see RebaseTo9 lambda in main()) BEFORE Attach(), since
 *      ActivateEpsBearer() reads the UE's live IP synchronously inside
 *      Attach() to register it with the PGW.
 *   4. Ipv4StaticRoutingHelper used — GlobalRoutingHelper crashes with LTE
 *      virtual devices (loopback routes on GTP tunnel interfaces).
 *   5. SRTT and RTTVAR not exposed as ns-3 trace sources; computed locally
 *      per RFC 6298 from the "RTT" trace (which IS the SRTT internally).
 *   6. CQI path: /NodeList/N/DeviceList/D/ComponentCarrierMapUe/0/
 *                LteUePhy/ReportCurrentCellRsrpSinr
 *      sinr argument is LINEAR — convert with 10*log10() before CQI table.
 *   7. PHY error model disabled; RLF thresholds raised to prevent
 *      spurious CONNECTED_PHY_PROBLEM cycling under shared air interface.
 *   8. TCP traces hooked via BulkSendApplication::GetSocket() →
 *      DynamicCast<TcpSocketBase>; scheduled at appStart+0.001s;
 *      Heartbeat re-checks every 5s.
 *
 * ═══════════════════════════════════════════════════════════════════════════
 * BUILD AND RUN
 * ═══════════════════════════════════════════════════════════════════════════
 *   cd <ns-3.46-root>
 *   python3 ns3 configure
 *   python3 ns3 build scratch/lte_ue_to_ue
 *   ./ns3 run "scratch/lte_ue_to_ue --simTime=10"    # smoke test
 *   ./ns3 run "scratch/lte_ue_to_ue --simTime=300"   # full dataset run
 *
 * ═══════════════════════════════════════════════════════════════════════════
 * EXPECTED OUTPUT RANGES
 * ═══════════════════════════════════════════════════════════════════════════
 *   Baseline RTT   : ~15–35ms  (S1 delays: 2+5=7ms each way + air latency)
 *   CWND peak      : 100–400 KB (bounded by LTE air interface, not a P2P queue)
 *   RTO baseline   : ~40–80ms  (MinRto=200ms clamps lower bound for LTE)
 *   CQI normal     : 9–15  (UE-A is 50m from eNB1 — excellent SINR)
 *   CQI fading     : 3–6   (synthetic override during fading events)
 *   label=0 (normal)    : ~50% of 200ms samples over 300s
 *   label=1 (congested) : ~35% (when bg UEs active, queue > threshold)
 *   label=2 (fading)    : ~15% (Poisson fading events, mean 30s interval)
 */

#include <cmath>
#include <fstream>
#include <iomanip>
#include <set>
#include <string>

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/lte-module.h"
#include "ns3/mobility-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/propagation-module.h"
#include "ns3/tcp-socket-base.h"
#include "ns3/traffic-control-module.h"

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("LteUeToUe");

// ─────────────────────────────────────────────────────────────────────────────
// Constants
// ─────────────────────────────────────────────────────────────────────────────

static const uint32_t N_BG_UES   = 4;    // background UEs per eNB
static const uint32_t TCP_PORT   = 5000;
static const uint32_t UDP_UL_PORT= 6000; // UE-B → coreServer
static const uint32_t UDP_DL_BASE= 7000; // coreServer → bgUE-i on port 7000+i

// ─────────────────────────────────────────────────────────────────────────────
// Global CSV streams
// ─────────────────────────────────────────────────────────────────────────────

static std::ofstream g_rttFile;        // rtt_lte.csv
static std::ofstream g_cwndFile;       // cwnd_lte.csv
static std::ofstream g_rtoFile;        // rto_lte.csv
static std::ofstream g_ackIatFile;     // ack_iat_lte.csv
static std::ofstream g_queueFile;      // queue_lte.csv
static std::ofstream g_connFile;       // conn_lte.csv
static std::ofstream g_cqiFile;        // cqi_lte.csv
static std::ofstream g_handoverFile;   // handover_lte.csv
static std::ofstream g_throughputFile; // throughput_lte.csv
static std::ofstream g_featFile;       // features_lte.csv
static std::ofstream g_txFile;         // tx_lte.csv        (all data segments sent)
static std::ofstream g_retxFile;       // retx_lte.csv      (retransmissions only)
static std::ofstream g_rxFile;         // rx_lte.csv        (segments delivered at UE-B)
static std::ofstream g_timeoutFile;    // timeout_lte.csv   (one row per RTO expiry)
static std::ofstream g_congStateFile;  // cong_state_lte.csv (TCP congestion-state changes)
static std::ofstream g_ackRxFile;      // ack_rx_lte.csv     (ACKs received by the sender, with TSecr)

// ─────────────────────────────────────────────────────────────────────────────
// Per-callback state
// ─────────────────────────────────────────────────────────────────────────────

static double   g_currentRtt_us    = 0.0;
static uint32_t g_currentCwnd      = 0;
static double   g_currentRto_us    = 1e6;
static double   g_currentAckIat_us = 0.0;

// RFC 6298 SRTT/RTTVAR — computed locally
static double g_srtt_us   = 0.0;
static double g_rttvar_us = 0.0;
static bool   g_rttFirst  = true;

// ACK IAT tracking
static Time g_lastAckTime;
static bool g_firstAck = true;

// CQI / SINR / RSRP state (updated by CqiCallback)
static int    g_featCqi     = 10;
static double g_featSinr_dB = 15.0;
static double g_featRsrp_dBm= -70.0;

// Synthetic fading override
// Channel-derived (NOT synthetic): true while the real SINR from the UE PHY
// maps to CQI <= 3 (SINR < 0.2 dB, 3GPP TS 36.213 table). Used only for the
// in-sim feature label; the analysis re-derives fade episodes from cqi_lte.csv
// with hysteresis (see plan: definition (c)).
static bool g_fadingActive   = false;

// Congestion state (queue-based)
static uint32_t g_currentQueueDepth = 0;
static bool     g_isCongestion      = false;
static uint32_t g_queueThreshold    = 10; // set from cmdline

// Throughput state
static Ptr<PacketSink> g_ueBSink    = nullptr;
static uint64_t        g_lastRxBytes= 0;
static double          g_tputKbps   = 0.0;

// Bottleneck queue
static Ptr<DropTailQueue<Packet>> g_btlQueue;

// Monitored sender node (UE-A)
static Ptr<Node> g_monSrcNode;

// Avoid double-hooking
static std::set<void*> g_hookedSockets;

// Simulation end time (for fading scheduling)
static double g_simEnd = 300.0;

// ─────────────────────────────────────────────────────────────────────────────
// 3GPP TS 36.213 Table 7.2.3-1: SINR (dB) → CQI 1–15
// ─────────────────────────────────────────────────────────────────────────────

static uint8_t SinrToCqi(double sinr_dB)
{
    static const double thresh[15] = {
        -6.7, -4.7, -2.3,  0.2,  2.4,
         4.3,  5.9,  8.1, 10.3, 11.7,
        14.1, 16.3, 18.7, 21.0, 22.7
    };
    for (int c = 14; c >= 0; --c)
        if (sinr_dB >= thresh[c])
            return static_cast<uint8_t>(c + 1);
    return 1;
}

// ─────────────────────────────────────────────────────────────────────────────
// Blockage / shadowing episodes — seconds-scale outages on UE-A's radio link
//
// Why this exists: the shipped Rayleigh traces (EPA/ETU/EVA) fade for
// milliseconds and are averaged over the 50 RBs, so at a 50 m high-SNR link
// the wideband SINR never drops (observed: min 14 dB, CQI never <= 3). The
// mechanism under study (fade -> RTO backoff -> channel recovers -> stale
// RTO) needs outages of at least several RTOs, i.e. hundreds of ms to
// seconds: a UE entering a lift, tunnel or building. BlockageLossModel
// subtracts a scheduled extra loss (dB) on every link that involves UE-A, in
// both directions, with a 100 ms ramp at each edge. Episodes are drawn from the
// run's RNG (so they differ per --RngRun) and written to blockage_lte.csv.
// ─────────────────────────────────────────────────────────────────────────────

struct BlockEpisode
{
    double start;
    double end;
    double depth_db;
};

static std::vector<BlockEpisode> g_blockEpisodes;                // sorted, non-overlapping
static Ptr<MobilityModel> g_blockTargetMobility;                // UE-A's mobility model
static const double BLOCK_RAMP_S = 0.1;

static double BlockageExtraLossDb(double t)
{
    for (const auto& e : g_blockEpisodes)
    {
        if (t < e.start)
            break; // episodes are sorted: nothing later can apply
        if (t <= e.end)
        {
            double up   = (t - e.start) / BLOCK_RAMP_S;
            double down = (e.end - t) / BLOCK_RAMP_S;
            return e.depth_db * std::max(0.0, std::min(1.0, std::min(up, down)));
        }
    }
    return 0.0;
}

class BlockageLossModel : public PropagationLossModel
{
  public:
    static TypeId GetTypeId()
    {
        static TypeId tid =
            TypeId("ns3::BlockageLossModel")
                .SetParent<PropagationLossModel>()
                .SetGroupName("Propagation")
                .AddConstructor<BlockageLossModel>()
                .AddAttribute("Exponent",
                              "Log-distance path-loss exponent of the wrapped base model",
                              DoubleValue(3.76),
                              MakeDoubleAccessor(&BlockageLossModel::SetExponent,
                                                 &BlockageLossModel::GetExponent),
                              MakeDoubleChecker<double>());
        return tid;
    }

    BlockageLossModel()
        : m_inner(CreateObject<LogDistancePropagationLossModel>())
    {
    }

    void SetExponent(double e)
    {
        m_inner->SetAttribute("Exponent", DoubleValue(e));
        m_exponent = e;
    }

    double GetExponent() const
    {
        return m_exponent;
    }

  private:
    double DoCalcRxPower(double txPowerDbm, Ptr<MobilityModel> a, Ptr<MobilityModel> b) const override
    {
        double rx = m_inner->CalcRxPower(txPowerDbm, a, b);
        // Pointer comparison (not GetObject<Node>()): this runs for every
        // tx/rx pair every subframe, so it must be cheap.
        if (g_blockTargetMobility && (a == g_blockTargetMobility || b == g_blockTargetMobility))
            rx -= BlockageExtraLossDb(Simulator::Now().GetSeconds());
        return rx;
    }

    int64_t DoAssignStreams(int64_t /*stream*/) override
    {
        return 0;
    }

    Ptr<LogDistancePropagationLossModel> m_inner;
    double                               m_exponent{3.76};
};

NS_OBJECT_ENSURE_REGISTERED(BlockageLossModel);

static void BuildBlockageSchedule(double t0, double t1, double meanGap, double minDur,
                                  double maxDur, double minDepth, double maxDepth)
{
    Ptr<ExponentialRandomVariable> gap = CreateObject<ExponentialRandomVariable>();
    gap->SetAttribute("Mean", DoubleValue(meanGap));
    Ptr<UniformRandomVariable> uni = CreateObject<UniformRandomVariable>();
    double t = t0;
    while (true)
    {
        t += std::max(1.0, gap->GetValue()); // at least 1 s of clear channel between episodes
        double dur   = std::exp(uni->GetValue(std::log(minDur), std::log(maxDur))); // log-uniform
        double depth = uni->GetValue(minDepth, maxDepth);
        if (t + dur > t1 - 1.0)
            break;
        g_blockEpisodes.push_back({t, t + dur, depth});
        t += dur;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// CQI callback — fires on each RSRP/SINR report from UE-A PHY
// ─────────────────────────────────────────────────────────────────────────────

static void CqiCallback(uint16_t /*cellId*/, uint16_t /*rnti*/,
                        double rsrp_W, double sinr_linear, uint8_t /*ccId*/)
{
    double sinr_dB  = (sinr_linear > 1e-15) ? 10.0 * std::log10(sinr_linear) : -30.0;
    double rsrp_dBm = (rsrp_W      > 1e-15) ? 10.0 * std::log10(rsrp_W * 1000.0) : -140.0;

    g_featCqi      = static_cast<int>(SinrToCqi(sinr_dB));
    g_featSinr_dB  = sinr_dB;
    g_featRsrp_dBm = rsrp_dBm;
    g_fadingActive = (g_featCqi <= 3);

    g_cqiFile << std::fixed << std::setprecision(6)
              << Simulator::Now().GetSeconds() << ","
              << rsrp_dBm << "," << sinr_dB << "," << g_featCqi << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// Handover callback
// ─────────────────────────────────────────────────────────────────────────────

static void HandoverEndOkCallback(uint64_t imsi, uint16_t cellId, uint16_t rnti)
{
    double now = Simulator::Now().GetSeconds();
    g_handoverFile << std::fixed << std::setprecision(6)
                   << now << "," << imsi << "," << cellId << "," << rnti << "\n";
    NS_LOG_UNCOND("[HO-OK] t=" << now << "  imsi=" << imsi << "  cell=" << cellId);
}

// ─────────────────────────────────────────────────────────────────────────────
// TCP trace callbacks
// ─────────────────────────────────────────────────────────────────────────────

static void TraceRtt(uint32_t nodeId, Time /*oldVal*/, Time newVal)
{
    double rtt_us = static_cast<double>(newVal.GetMicroSeconds());
    if (rtt_us <= 0.0)
        return;

    // RFC 6298 SRTT/RTTVAR update
    if (g_rttFirst)
    {
        g_srtt_us   = rtt_us;
        g_rttvar_us = rtt_us / 2.0;
        g_rttFirst  = false;
    }
    else
    {
        g_rttvar_us = 0.75 * g_rttvar_us + 0.25 * std::abs(g_srtt_us - rtt_us);
        g_srtt_us   = 0.875 * g_srtt_us + 0.125 * rtt_us;
    }
    g_currentRtt_us = rtt_us;

    g_rttFile << std::fixed << std::setprecision(6)
              << Simulator::Now().GetSeconds() << ","
              << nodeId << "," << static_cast<int64_t>(rtt_us) << "\n";
}

static void TraceCwnd(uint32_t nodeId, uint32_t /*oldVal*/, uint32_t newVal)
{
    g_currentCwnd = newVal;
    g_cwndFile << std::fixed << std::setprecision(6)
               << Simulator::Now().GetSeconds() << ","
               << nodeId << "," << static_cast<int64_t>(newVal) << "\n";
}

static void TraceSsThresh(uint32_t nodeId, uint32_t /*oldVal*/, uint32_t newVal)
{
    g_cwndFile << std::fixed << std::setprecision(6)
               << Simulator::Now().GetSeconds() << ","
               << nodeId << "," << -static_cast<int64_t>(newVal) << "\n";
}

static void TraceRto(uint32_t nodeId, Time oldVal, Time newVal)
{
    g_currentRto_us = static_cast<double>(newVal.GetMicroSeconds());
    g_rtoFile << std::fixed << std::setprecision(6)
              << Simulator::Now().GetSeconds() << ","
              << nodeId << "," << static_cast<int64_t>(g_currentRto_us) << "\n";

    // RTO EXPIRY detection. ns-3 gives no timeout trace source, but the
    // exponential backoff in TcpSocketBase::ReTxTimeout sets m_rto to exactly
    // twice its previous value, and nothing else does (estimator updates come
    // from ACKs and never produce an exact doubling). The row records the RTO
    // that just EXPIRED (oldVal) and the most recent RTT sample, which is what
    // the "slack at genuine loss" metric needs.
    if (oldVal.IsStrictlyPositive() && newVal == oldVal + oldVal)
    {
        g_timeoutFile << std::fixed << std::setprecision(6)
                      << Simulator::Now().GetSeconds() << ","
                      << nodeId << ","
                      << oldVal.GetMicroSeconds() << ","
                      << static_cast<int64_t>(g_currentRtt_us) << ","
                      << static_cast<int64_t>(g_srtt_us) << ","
                      << static_cast<int64_t>(g_rttvar_us) << "\n";
    }
}

static void TraceAckIat(uint32_t nodeId,
                        SequenceNumber32 /*oldVal*/,
                        SequenceNumber32 /*newVal*/)
{
    Time now = Simulator::Now();
    if (!g_firstAck)
    {
        double iat_us = static_cast<double>((now - g_lastAckTime).GetMicroSeconds());
        if (iat_us > 0.0)
        {
            g_currentAckIat_us = iat_us;
            g_ackIatFile << std::fixed << std::setprecision(6)
                         << now.GetSeconds() << ","
                         << nodeId << "," << static_cast<int64_t>(iat_us) << "\n";
        }
    }
    g_lastAckTime = now;
    g_firstAck    = false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Per-segment traces — ground truth for the spurious-timeout oracle
//
//   tx_lte.csv      every data segment the sender emits (originals + retx)
//   retx_lte.csv    the subset that are retransmissions
//   rx_lte.csv      every data segment delivered to UE-B's TCP layer
//   timeout_lte.csv one row per RTO EXPIRY (see TraceRto)
//
// The TCP timestamp option's TSval (sender clock, ms) is logged on both sides.
// A retransmission gets a new TSval, so at the receiver the original copy of a
// segment is distinguishable from its retransmitted copy. Post-processing uses
// this: a timeout is spurious iff the ORIGINAL copy reached the receiver.
// ─────────────────────────────────────────────────────────────────────────────

static uint32_t TsValOf(const TcpHeader& h)
{
    if (!h.HasOption(TcpOption::TS))
        return 0;
    Ptr<const TcpOptionTS> ts = DynamicCast<const TcpOptionTS>(h.GetOption(TcpOption::TS));
    return ts ? ts->GetTimestamp() : 0;
}

static void TraceTx(Ptr<const Packet> p, const TcpHeader& h, Ptr<const TcpSocketBase> /*s*/)
{
    if (p->GetSize() == 0)
        return; // pure ACK / SYN / FIN
    g_txFile << std::fixed << std::setprecision(6) << Simulator::Now().GetSeconds() << ","
             << h.GetSequenceNumber().GetValue() << "," << p->GetSize() << ","
             << TsValOf(h) << "\n";
}

static void TraceRetx(Ptr<const Packet> p, const TcpHeader& h,
                      const Address& /*local*/, const Address& /*peer*/,
                      Ptr<const TcpSocketBase> /*s*/)
{
    g_retxFile << std::fixed << std::setprecision(6) << Simulator::Now().GetSeconds() << ","
               << h.GetSequenceNumber().GetValue() << "," << p->GetSize() << ","
               << TsValOf(h) << "\n";
}

// Sender side: every pure ACK received, with the echoed timestamp (TSecr). Lets the
// analysis tell a LATE ACK (echo of the original copy arrives after the timeout:
// the RTO was too short) from a LOST ACK (data delivered, ACK never arrives).
static void TraceAckRx(Ptr<const Packet> p, const TcpHeader& h, Ptr<const TcpSocketBase>)
{
    if (p->GetSize() != 0 || !(h.GetFlags() & TcpHeader::ACK))
        return;
    uint32_t echo = 0;
    if (h.HasOption(TcpOption::TS))
    {
        Ptr<const TcpOptionTS> ts = DynamicCast<const TcpOptionTS>(h.GetOption(TcpOption::TS));
        if (ts)
            echo = ts->GetEcho();
    }
    g_ackRxFile << std::fixed << std::setprecision(6) << Simulator::Now().GetSeconds() << ","
                << h.GetAckNumber().GetValue() << "," << echo << "\n";
}

static void TraceCongState(TcpSocketState::TcpCongState_t oldS,
                           TcpSocketState::TcpCongState_t newS)
{
    g_congStateFile << std::fixed << std::setprecision(6) << Simulator::Now().GetSeconds()
                    << "," << TcpSocketState::TcpCongStateName[oldS]
                    << "," << TcpSocketState::TcpCongStateName[newS] << "\n";
}

// Receiver side: tap UE-B's IPv4 local delivery and parse the TCP header, so no
// socket-hook race with the first segments (the accepted socket is created
// only after the handshake completes).
static void RxLocalDeliver(const Ipv4Header& ip, Ptr<const Packet> pkt, uint32_t /*iface*/)
{
    if (ip.GetProtocol() != 6)
        return; // TCP only
    Ptr<Packet> copy = pkt->Copy();
    TcpHeader th;
    uint32_t hlen = copy->PeekHeader(th);
    if (th.GetDestinationPort() != TCP_PORT)
        return;
    if (copy->GetSize() <= hlen)
        return; // no payload
    g_rxFile << std::fixed << std::setprecision(6) << Simulator::Now().GetSeconds() << ","
             << th.GetSequenceNumber().GetValue() << "," << (copy->GetSize() - hlen) << ","
             << TsValOf(th) << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// Periodic: queue sampling every 100ms
// ─────────────────────────────────────────────────────────────────────────────

static void SampleQueue()
{
    uint32_t depth = g_btlQueue ? g_btlQueue->GetNPackets() : 0;
    double   now   = Simulator::Now().GetSeconds();

    g_currentQueueDepth = depth;
    g_queueFile << std::fixed << std::setprecision(6) << now << "," << depth << "\n";

    bool wasCong   = g_isCongestion;
    g_isCongestion = (depth > g_queueThreshold);

    if (g_isCongestion && !wasCong)
        g_connFile << std::fixed << std::setprecision(6)
                   << now << ",0,congestion_start\n";
    else if (!g_isCongestion && wasCong)
        g_connFile << std::fixed << std::setprecision(6)
                   << now << ",0,congestion_end\n";

    Simulator::Schedule(MilliSeconds(100), &SampleQueue);
}

// ─────────────────────────────────────────────────────────────────────────────
// Periodic: UE-B TCP throughput every 500ms
// ─────────────────────────────────────────────────────────────────────────────

static void SampleThroughput()
{
    uint64_t rx    = g_ueBSink ? g_ueBSink->GetTotalRx() : 0;
    uint64_t delta = (rx >= g_lastRxBytes) ? (rx - g_lastRxBytes) : 0;
    g_tputKbps     = (static_cast<double>(delta) * 8.0) / (0.5 * 1e3); // 500ms window → kbps

    g_throughputFile << std::fixed << std::setprecision(6)
                     << Simulator::Now().GetSeconds() << ","
                     << rx << ","
                     << std::setprecision(2) << g_tputKbps << "\n";
    g_lastRxBytes = rx;
    Simulator::Schedule(MilliSeconds(500), &SampleThroughput);
}

// ─────────────────────────────────────────────────────────────────────────────
// Periodic: feature vector every 200ms
// ─────────────────────────────────────────────────────────────────────────────

static void SampleFeatures()
{
    double now = Simulator::Now().GetSeconds();

    // Priority: fading=2, congestion=1, normal=0
    int label = g_fadingActive ? 2 : (g_isCongestion ? 1 : 0);

    g_featFile << std::fixed << std::setprecision(6)
               << now
               << "," << static_cast<int64_t>(g_currentRtt_us)
               << "," << g_currentCwnd
               << "," << static_cast<int64_t>(g_srtt_us)
               << "," << static_cast<int64_t>(g_rttvar_us)
               << "," << static_cast<int64_t>(g_currentRto_us)
               << "," << static_cast<int64_t>(g_currentAckIat_us)
               << "," << g_currentQueueDepth
               << "," << g_featCqi
               << "," << std::setprecision(3) << g_featSinr_dB
               << "," << std::setprecision(2) << g_tputKbps
               << "," << (g_isCongestion ? 1 : 0)
               << "," << (g_fadingActive  ? 1 : 0)
               << "," << label << "\n";

    Simulator::Schedule(MilliSeconds(200), &SampleFeatures);
}

// ─────────────────────────────────────────────────────────────────────────────
// Socket hookup — called at appStart+0.001s and re-checked via Heartbeat
// ─────────────────────────────────────────────────────────────────────────────

static void HookSocket(uint32_t nodeId)
{
    for (uint32_t k = 0; k < g_monSrcNode->GetNApplications(); ++k)
    {
        Ptr<BulkSendApplication> bulk =
            DynamicCast<BulkSendApplication>(g_monSrcNode->GetApplication(k));
        if (!bulk)
            continue;
        Ptr<Socket> sock = bulk->GetSocket();
        if (!sock)
            continue;
        void* raw = PeekPointer(sock);
        if (g_hookedSockets.count(raw))
            continue;
        Ptr<TcpSocketBase> tcp = DynamicCast<TcpSocketBase>(sock);
        if (!tcp)
            continue;

        tcp->TraceConnectWithoutContext(
            "RTT",               MakeBoundCallback(&TraceRtt,      nodeId));
        tcp->TraceConnectWithoutContext(
            "CongestionWindow",  MakeBoundCallback(&TraceCwnd,     nodeId));
        tcp->TraceConnectWithoutContext(
            "SlowStartThreshold",MakeBoundCallback(&TraceSsThresh, nodeId));
        tcp->TraceConnectWithoutContext(
            "RTO",               MakeBoundCallback(&TraceRto,      nodeId));
        tcp->TraceConnectWithoutContext(
            "HighestRxAck",      MakeBoundCallback(&TraceAckIat,   nodeId));
        tcp->TraceConnectWithoutContext("Tx",            MakeCallback(&TraceTx));
        tcp->TraceConnectWithoutContext("Retransmission",MakeCallback(&TraceRetx));
        tcp->TraceConnectWithoutContext("CongState",     MakeCallback(&TraceCongState));
        tcp->TraceConnectWithoutContext("Rx",            MakeCallback(&TraceAckRx));

        g_hookedSockets.insert(raw);
        NS_LOG_UNCOND("[HookSocket] t=" << std::fixed << std::setprecision(3)
                      << Simulator::Now().GetSeconds()
                      << "s  Hooked TCP on UE-A nodeId=" << nodeId);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Heartbeat — live metrics every 5s; retries HookSocket
// ─────────────────────────────────────────────────────────────────────────────

static void Heartbeat(uint32_t monNodeId)
{
    NS_LOG_UNCOND("t=" << std::fixed << std::setprecision(1)
                  << Simulator::Now().GetSeconds() << "s"
                  << "  RTT="   << static_cast<int64_t>(g_currentRtt_us / 1000) << "ms"
                  << "  CWND="  << g_currentCwnd / 1024 << "KB"
                  << "  RTO="   << static_cast<int64_t>(g_currentRto_us / 1000) << "ms"
                  << "  Queue=" << g_currentQueueDepth << "p"
                  << "  CQI="   << g_featCqi
                  << "  SINR="  << std::setprecision(1) << g_featSinr_dB << "dB"
                  << "  Tput="  << std::setprecision(0) << g_tputKbps << "kbps"
                  << "  "       << (g_fadingActive  ? "FADING "    : "")
                  << "          " << (g_isCongestion  ? "CONGESTED" : "normal"));

    HookSocket(monNodeId);
    Simulator::Schedule(Seconds(5.0), &Heartbeat, monNodeId);
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[])
{
    // ── CommandLine parameters ────────────────────────────────────────────────
    double      simTime        = 300.0;
    double      appStart       = 5.0;
    std::string s1DelayEnb1   = "2ms";
    std::string s1DelayEnb2   = "5ms";
    std::string bottleneckBw   = "5Mbps";
    uint32_t    bottleneckQueue= 30;
    std::string bgRate         = "1Mbps";     // DL rate per bg UE when ON
    double      bgOnMean       = 15.0;        // mean ON duration (s)
    double      bgOffMean      = 10.0;        // mean OFF duration (s)
    std::string ueBUlRate      = "512Kbps";   // UE-B UDP uplink rate
    std::string tcpVariant     = "TcpNewReno";
    uint32_t    queueThreshold = 10;          // congestion if depth > this
    // Radio realism (claim-A study): real HARQ/BLER, RLC AM, trace fading.
    // RlcAmAlways = RLC Acknowledged Mode on every bearer; every UE here only
    // has the default bearer (QCI 9), which real LTE runs on AM. (ns-3.46 has
    // no "PerBearer" value; valid: RlcSmAlways|RlcUmAlways|RlcAmAlways|
    // PacketErrorRateBased.) RlcUmAlways is the documented fallback if AM
    // window stalls reappear.
    std::string rlcMode        = "RlcAmAlways";
    // Sensitivity-sweep knobs (study defaults reproduce the main 30-seed results).
    std::string minRto         = "200ms";    // TcpSocketBase::MinRto (RFC 6298 itself says 1 s)
    uint32_t    rlcBufferKB    = 32;         // LteRlcAm/Um::MaxTxBufferSize (ns-3 default is 10)
    bool        errorModels    = true;        // PHY BLER model (HARQ losses)
    bool        fadingModel    = true;        // TraceFadingLossModel (Rayleigh)
    std::string fadingTrace    =
        "src/lte/model/fading-traces/fading_trace_EPA_3kmph.fad";
    // Seconds-scale blockage episodes on UE-A's link (the claim-A fade mechanism).
    bool        blockage       = false;
    double      blockMean      = 20.0;   // mean clear-channel gap between episodes (s)
    double      blockMinDur    = 0.3;    // episode duration, log-uniform (s)
    double      blockMaxDur    = 6.0;
    double      blockMinDepth  = 10.0;   // extra loss, uniform (dB); >~35 dB = full outage
    double      blockMaxDepth  = 40.0;

    CommandLine cmd(__FILE__);
    cmd.AddValue("simTime",        "Simulation duration (s)",             simTime);
    cmd.AddValue("appStart",       "Application start time (s)",          appStart);
    cmd.AddValue("s1DelayEnb1",    "S1-U one-way delay for eNB1",         s1DelayEnb1);
    cmd.AddValue("s1DelayEnb2",    "S1-U one-way delay for eNB2",         s1DelayEnb2);
    cmd.AddValue("bottleneckBw",   "P2P PGW-coreServer bandwidth",        bottleneckBw);
    cmd.AddValue("bottleneckQueue","P2P queue depth (packets)",           bottleneckQueue);
    cmd.AddValue("bgRate",         "Background DL rate per UE when ON",   bgRate);
    cmd.AddValue("bgOnMean",       "Background UE mean ON duration (s)",  bgOnMean);
    cmd.AddValue("bgOffMean",      "Background UE mean OFF duration (s)", bgOffMean);
    cmd.AddValue("ueBUlRate",      "UE-B UDP uplink data rate",           ueBUlRate);
    cmd.AddValue("tcpVariant",     "TCP CC for UE-A monitored flow",      tcpVariant);
    cmd.AddValue("queueThreshold", "Queue depth threshold for label=1",   queueThreshold);
    cmd.AddValue("rlcMode",        "EpsBearerToRlcMapping: RlcAmAlways|RlcUmAlways|PacketErrorRateBased", rlcMode);
    cmd.AddValue("minRto",         "TCP minimum RTO (study: 200ms; RFC 6298: 1s)",  minRto);
    cmd.AddValue("rlcBufferKB",    "RLC transmit buffer per bearer (KB)",           rlcBufferKB);
    cmd.AddValue("errorModels",    "Enable PHY data/control error models",  errorModels);
    cmd.AddValue("fadingModel",    "Enable TraceFadingLossModel",           fadingModel);
    cmd.AddValue("fadingTrace",    "Path to .fad fading trace file",        fadingTrace);
    cmd.AddValue("blockage",       "Enable seconds-scale blockage episodes on UE-A", blockage);
    cmd.AddValue("blockMean",      "Mean clear gap between blockage episodes (s)",   blockMean);
    cmd.AddValue("blockMinDur",    "Min blockage duration (s, log-uniform)",         blockMinDur);
    cmd.AddValue("blockMaxDur",    "Max blockage duration (s, log-uniform)",         blockMaxDur);
    cmd.AddValue("blockMinDepth",  "Min blockage depth (dB)",                        blockMinDepth);
    cmd.AddValue("blockMaxDepth",  "Max blockage depth (dB)",                        blockMaxDepth);
    cmd.Parse(argc, argv);

    g_queueThreshold = queueThreshold;
    g_simEnd         = simTime;

    {
        UintegerValue run;
        GlobalValue::GetValueByName("RngRun", run);
        NS_LOG_UNCOND("[SEED] RngRun=" << run.Get()
            << "  rlcMode=" << rlcMode << "  errorModels=" << errorModels
            << "  fadingModel=" << fadingModel << "  minRto=" << minRto
            << "  rlcBufferKB=" << rlcBufferKB);
    }

    // ── Global TCP defaults ───────────────────────────────────────────────────
    Config::SetDefault("ns3::TcpL4Protocol::SocketType",
        TypeIdValue(TypeId::LookupByName("ns3::" + tcpVariant)));
    Config::SetDefault("ns3::TcpSocket::SndBufSize",   UintegerValue(512 * 1024));
    Config::SetDefault("ns3::TcpSocket::RcvBufSize",   UintegerValue(512 * 1024));
    Config::SetDefault("ns3::TcpSocket::SegmentSize",  UintegerValue(1448));
    Config::SetDefault("ns3::TcpSocket::DelAckCount",  UintegerValue(1));
    Config::SetDefault("ns3::TcpSocket::DataRetries",  UintegerValue(20));
    Config::SetDefault("ns3::TcpSocket::ConnCount",    UintegerValue(20));
    // LTE needs MinRto ≥ ~40ms (subframe scheduling); 200ms default is safer
    Config::SetDefault("ns3::TcpSocketBase::MinRto",   TimeValue(Time(minRto)));

    // ── LTE MAC / RLC defaults ────────────────────────────────────────────────
    // RLC mode is a command-line option (default PerBearer -> AM). It affects
    // what a fade looks like to TCP: AM turns a fade into delay (RLC
    // retransmission), UM turns it into loss.
    Config::SetDefault("ns3::LteEnbRrc::EpsBearerToRlcMapping",
        StringValue(rlcMode));
    Config::SetDefault("ns3::LteRlcUm::MaxTxBufferSize", UintegerValue(rlcBufferKB * 1024));
    Config::SetDefault("ns3::LteRlcAm::MaxTxBufferSize", UintegerValue(rlcBufferKB * 1024));

    // ── LTE PHY defaults ──────────────────────────────────────────────────────
    Config::SetDefault("ns3::LteEnbPhy::TxPower",  DoubleValue(46.0));
    Config::SetDefault("ns3::LteUePhy::TxPower",   DoubleValue(23.0));
    // 10 MHz (50 RBs): comfortable for 1 monitored + 4 bg UEs per eNB
    Config::SetDefault("ns3::LteEnbNetDevice::DlBandwidth", UintegerValue(50));
    Config::SetDefault("ns3::LteEnbNetDevice::UlBandwidth", UintegerValue(50));

    // PHY error model ON by default: BLER-driven HARQ retransmissions and
    // residual losses are what make fades visible to TCP. (Earlier versions
    // disabled it for a benign channel; that cannot exhibit the RTO behaviour
    // under study.) --errorModels=false restores the old benign radio.
    Config::SetDefault("ns3::LteSpectrumPhy::DataErrorModelEnabled", BooleanValue(errorModels));
    Config::SetDefault("ns3::LteSpectrumPhy::CtrlErrorModelEnabled", BooleanValue(errorModels));

    // Raise RLF thresholds — prevents CONNECTED_PHY_PROBLEM cycling when SINR is low
    Config::SetDefault("ns3::LteUeRrc::N310", UintegerValue(20));
    Config::SetDefault("ns3::LteUeRrc::T310", TimeValue(MilliSeconds(2000)));
    Config::SetDefault("ns3::LteUeRrc::N311", UintegerValue(1));
    Config::SetDefault("ns3::LteUeRrc::T300", TimeValue(MilliSeconds(2000)));
    Config::SetDefault("ns3::LteUePhy::Qout", DoubleValue(-1e30));

    // ── EPC + LTE helpers — TWO independent EPCs (no shared PGW/MME) ─────────
    // eNB1/UE-A/bgUE-1..4 live under EPC-A; eNB2/UE-B/bgUE-5..8 under EPC-B.
    // No X2/AddX2Interface — cross-EPC handover requires a shared MME, which
    // doesn't exist once the two eNBs belong to independent EPCs.
    //
    // A SINGLE LteHelper is shared across both EPCs (its EpcHelper pointer is
    // swapped via SetEpcHelper() right before each EPC's install/attach
    // calls, which is all it consults it for). This is deliberate: LteHelper
    // keeps its own per-instance CellId/IMSI counters, both starting at 1/0 —
    // two separate LteHelper objects would each hand out CellId=1 to their
    // eNB, and the LTE RRC/PHY layers assume CellId is globally unique
    // (measurement reports, control-channel search, RNTI bookkeeping all key
    // off it), which crashes/corrupts UE attach when duplicated. Sharing one
    // LteHelper keeps CellId/IMSI unique across both eNBs while each
    // PointToPointEpcHelper still gets its own independent PGW/SGW/MME.
    Ptr<LteHelper> lteHelper = CreateObject<LteHelper>();
    lteHelper->SetPathlossModelType(TypeId::LookupByName(
        blockage ? "ns3::BlockageLossModel" : "ns3::LogDistancePropagationLossModel"));
    lteHelper->SetPathlossModelAttribute("Exponent", DoubleValue(3.76));
    lteHelper->SetSchedulerType("ns3::PfFfMacScheduler");
    if (blockage)
    {
        // Episodes start after TCP has settled (appStart + 10 s) and stop 1 s before the end.
        BuildBlockageSchedule(appStart + 10.0, simTime, blockMean, blockMinDur, blockMaxDur,
                              blockMinDepth, blockMaxDepth);
        NS_LOG_UNCOND("[BLOCKAGE] " << g_blockEpisodes.size() << " episodes, mean gap "
                                    << blockMean << "s, dur " << blockMinDur << "-" << blockMaxDur
                                    << "s, depth " << blockMinDepth << "-" << blockMaxDepth << "dB");
    }
    if (fadingModel)
    {
        // Rayleigh small-scale fading from the shipped EPA 3 km/h trace. The
        // trace is 10 s long and loops, so appStart must not be a multiple of 10.
        lteHelper->SetFadingModel("ns3::TraceFadingLossModel");
        lteHelper->SetFadingModelAttribute("TraceFilename", StringValue(fadingTrace));
        lteHelper->SetFadingModelAttribute("TraceLength",   TimeValue(Seconds(10.0)));
        lteHelper->SetFadingModelAttribute("SamplesNum",    UintegerValue(10000));
        lteHelper->SetFadingModelAttribute("WindowSize",    TimeValue(Seconds(0.5)));
        lteHelper->SetFadingModelAttribute("RbNum",         UintegerValue(100));
        NS_LOG_UNCOND("[FADING-PHY] TraceFadingLossModel " << fadingTrace);
    }

    Ptr<PointToPointEpcHelper> epcHelperA = CreateObject<PointToPointEpcHelper>();

    // Ipv4AddressGenerator is a process-wide singleton that rejects
    // re-handing-out an address already given out — but NoBackhaulEpcHelper
    // assigns its own hardcoded addresses (UE pool base, S1/S5/S11 links) at
    // CONSTRUCTION time, so a second independent EpcHelper instance collides
    // immediately unless the generator's bookkeeping is reset first. Each
    // EPC's internal links are physically isolated point-to-point pairs, so
    // reusing the same address ranges across the two EPCs is safe.
    Ipv4AddressGenerator::Reset();
    Ptr<PointToPointEpcHelper> epcHelperB = CreateObject<PointToPointEpcHelper>();

    // ── eNB nodes + positions ─────────────────────────────────────────────────
    NodeContainer enbNodes;
    enbNodes.Create(2);

    {
        MobilityHelper enbMob;
        Ptr<ListPositionAllocator> enbPos = CreateObject<ListPositionAllocator>();
        enbPos->Add(Vector(   0.0, 0.0, 30.0)); // eNB1
        enbPos->Add(Vector(1000.0, 0.0, 30.0)); // eNB2
        enbMob.SetMobilityModel("ns3::ConstantPositionMobilityModel");
        enbMob.SetPositionAllocator(enbPos);
        enbMob.Install(enbNodes);
    }

    // Install eNB LTE devices — swap the active EpcHelper per eNB
    lteHelper->SetEpcHelper(epcHelperA);
    epcHelperA->SetAttribute("S1uLinkDelay",    TimeValue(Time(s1DelayEnb1)));
    epcHelperA->SetAttribute("S1uLinkDataRate", DataRateValue(DataRate("1Gbps")));
    NetDeviceContainer enbDev1 =
        lteHelper->InstallEnbDevice(NodeContainer(enbNodes.Get(0)));

    lteHelper->SetEpcHelper(epcHelperB);
    epcHelperB->SetAttribute("S1uLinkDelay",    TimeValue(Time(s1DelayEnb2)));
    epcHelperB->SetAttribute("S1uLinkDataRate", DataRateValue(DataRate("1Gbps")));
    // EPC-B's S1-U link address helper starts from the same hardcoded base
    // as EPC-A's (independent object, but identical starting counter) — the
    // two are physically isolated links, so reset the global duplicate
    // registry again before EPC-B claims it.
    Ipv4AddressGenerator::Reset();
    NetDeviceContainer enbDev2 =
        lteHelper->InstallEnbDevice(NodeContainer(enbNodes.Get(1)));

    // ── UE-A (monitored): stationary near eNB1 ───────────────────────────────
    NodeContainer ueANode;
    ueANode.Create(1);
    {
        MobilityHelper mob;
        Ptr<ListPositionAllocator> pos = CreateObject<ListPositionAllocator>();
        pos->Add(Vector(50.0, 0.0, 1.5));
        mob.SetMobilityModel("ns3::ConstantPositionMobilityModel");
        mob.SetPositionAllocator(pos);
        mob.Install(ueANode);
        NS_LOG_UNCOND("[UE-A] ConstantPosition (50, 0, 1.5)");
    }
    g_blockTargetMobility = ueANode.Get(0)->GetObject<MobilityModel>();

    // ── UE-B (TCP sink + UDP uplink): stationary near eNB2 ───────────────────
    NodeContainer ueBNode;
    ueBNode.Create(1);
    {
        MobilityHelper mob;
        Ptr<ListPositionAllocator> pos = CreateObject<ListPositionAllocator>();
        pos->Add(Vector(950.0, 0.0, 1.5));
        mob.SetMobilityModel("ns3::ConstantPositionMobilityModel");
        mob.SetPositionAllocator(pos);
        mob.Install(ueBNode);
        NS_LOG_UNCOND("[UE-B] ConstantPosition (950, 0, 1.5)");
    }

    // ── Background UEs on eNB1: RandomWaypoint X:[-300,+300] Y:[-300,+300] ───
    NodeContainer bgNodes1;
    bgNodes1.Create(N_BG_UES);
    {
        Ptr<RandomBoxPositionAllocator> wpAlloc1 = CreateObject<RandomBoxPositionAllocator>();
        wpAlloc1->SetAttribute("X", StringValue("ns3::UniformRandomVariable[Min=-300|Max=300]"));
        wpAlloc1->SetAttribute("Y", StringValue("ns3::UniformRandomVariable[Min=-300|Max=300]"));
        wpAlloc1->SetAttribute("Z", StringValue("ns3::ConstantRandomVariable[Constant=1.5]"));

        MobilityHelper mob;
        mob.SetPositionAllocator("ns3::RandomBoxPositionAllocator",
            "X", StringValue("ns3::UniformRandomVariable[Min=-300|Max=300]"),
            "Y", StringValue("ns3::UniformRandomVariable[Min=-300|Max=300]"),
            "Z", StringValue("ns3::ConstantRandomVariable[Constant=1.5]"));
        mob.SetMobilityModel("ns3::RandomWaypointMobilityModel",
            "Speed", StringValue("ns3::UniformRandomVariable[Min=1|Max=5]"),
            "Pause", StringValue("ns3::ConstantRandomVariable[Constant=2.0]"),
            "PositionAllocator", PointerValue(wpAlloc1));
        mob.Install(bgNodes1);
        NS_LOG_UNCOND("[BG-eNB1] " << N_BG_UES << " UEs RandomWaypoint X:[-300,+300] Y:[-300,+300]");
    }

    // ── Background UEs on eNB2: RandomWaypoint X:[700,1300] Y:[-300,+300] ────
    NodeContainer bgNodes2;
    bgNodes2.Create(N_BG_UES);
    {
        Ptr<RandomBoxPositionAllocator> wpAlloc2 = CreateObject<RandomBoxPositionAllocator>();
        wpAlloc2->SetAttribute("X", StringValue("ns3::UniformRandomVariable[Min=700|Max=1300]"));
        wpAlloc2->SetAttribute("Y", StringValue("ns3::UniformRandomVariable[Min=-300|Max=300]"));
        wpAlloc2->SetAttribute("Z", StringValue("ns3::ConstantRandomVariable[Constant=1.5]"));

        MobilityHelper mob;
        mob.SetPositionAllocator("ns3::RandomBoxPositionAllocator",
            "X", StringValue("ns3::UniformRandomVariable[Min=700|Max=1300]"),
            "Y", StringValue("ns3::UniformRandomVariable[Min=-300|Max=300]"),
            "Z", StringValue("ns3::ConstantRandomVariable[Constant=1.5]"));
        mob.SetMobilityModel("ns3::RandomWaypointMobilityModel",
            "Speed", StringValue("ns3::UniformRandomVariable[Min=1|Max=5]"),
            "Pause", StringValue("ns3::ConstantRandomVariable[Constant=2.0]"),
            "PositionAllocator", PointerValue(wpAlloc2));
        mob.Install(bgNodes2);
        NS_LOG_UNCOND("[BG-eNB2] " << N_BG_UES << " UEs RandomWaypoint X:[700,1300] Y:[-300,+300]");
    }

    // ── Internet stack (NOT on PGW — EpcHelper handles it) ───────────────────
    InternetStackHelper internet;
    internet.Install(ueANode);
    internet.Install(ueBNode);
    internet.Install(bgNodes1);
    internet.Install(bgNodes2);

    // ── LTE UE devices — EPC-A owns UE-A/bgUE-1..4, EPC-B owns UE-B/bgUE-5..8 ─
    // InstallUeDevice() registers the UE with whichever EpcHelper is
    // currently active (LteHelper::AddUe() calls m_epcHelper->AddUe()), so
    // the active EpcHelper must be switched before each group.
    lteHelper->SetEpcHelper(epcHelperA);
    NetDeviceContainer ueADev = lteHelper->InstallUeDevice(ueANode);
    NetDeviceContainer bgDev1 = lteHelper->InstallUeDevice(bgNodes1);

    // ── Assign EPC IP addresses ───────────────────────────────────────────────
    // EPC-A keeps the EpcHelper default pool (7.0.0.x/8).
    Ipv4InterfaceContainer ueAIface = epcHelperA->AssignUeIpv4Address(ueADev);
    Ipv4InterfaceContainer bgIface1 = epcHelperA->AssignUeIpv4Address(bgDev1);

    lteHelper->SetEpcHelper(epcHelperB);
    NetDeviceContainer ueBDev = lteHelper->InstallUeDevice(ueBNode);
    NetDeviceContainer bgDev2 = lteHelper->InstallUeDevice(bgNodes2);

    // EPC-B also gets 7.0.0.x/8 from AssignUeIpv4Address (NoBackhaulEpcHelper
    // hardcodes this pool with no public setter) — rebase every EPC-B
    // interface (UEs + PGW-B's tun device) to 9.0.0.x/8 so UE-A and UE-B end
    // up on genuinely different subnets. This MUST happen before Attach():
    // LteHelper::Attach() calls EpcHelper::ActivateEpsBearer() synchronously,
    // which reads the UE's live IPv4 address right there to register it with
    // the PGW's IMSI↔address map — rebasing afterward would leave the PGW
    // holding the stale 7.0.0.x address.
    //
    // EPC-B's UE pool helper independently starts from the same hardcoded
    // 7.0.0.x counter as EPC-A's — reset the global duplicate registry again
    // so it doesn't reject EPC-B re-using the same literal values.
    Ipv4AddressGenerator::Reset();
    Ipv4InterfaceContainer ueBIface = epcHelperB->AssignUeIpv4Address(ueBDev);
    Ipv4InterfaceContainer bgIface2 = epcHelperB->AssignUeIpv4Address(bgDev2);

    auto RebaseTo9 = [](Ptr<Node> node, Ptr<NetDevice> dev) -> Ipv4Address {
        Ptr<Ipv4> ipv4    = node->GetObject<Ipv4>();
        int32_t   ifIndex = ipv4->GetInterfaceForDevice(dev);
        NS_ASSERT_MSG(ifIndex >= 0, "device has no IPv4 interface");
        Ipv4Address oldAddr  = ipv4->GetAddress(ifIndex, 0).GetLocal();
        uint32_t    hostByte = oldAddr.Get() & 0xFF;
        Ipv4Address newAddr(("9.0.0." + std::to_string(hostByte)).c_str());
        ipv4->RemoveAddress(ifIndex, 0);
        ipv4->AddAddress(ifIndex, Ipv4InterfaceAddress(newAddr, Ipv4Mask("255.0.0.0")));
        return newAddr;
    };

    // Rebase PGW-B's tun-device interface (index 1 — see
    // NoBackhaulEpcHelper::GetUeDefaultGatewayAddress(), which reads this
    // same interface) from 7.0.0.1 to 9.0.0.1.
    {
        Ptr<Ipv4> pgwBIpv4 = epcHelperB->GetPgwNode()->GetObject<Ipv4>();
        pgwBIpv4->RemoveAddress(1, 0);
        pgwBIpv4->AddAddress(1,
            Ipv4InterfaceAddress(Ipv4Address("9.0.0.1"), Ipv4Mask("255.0.0.0")));
    }
    Ipv4Address ueBAddr = RebaseTo9(ueBNode.Get(0), ueBDev.Get(0));
    for (uint32_t i = 0; i < N_BG_UES; ++i)
        RebaseTo9(bgNodes2.Get(i), bgDev2.Get(i));

    Ipv4Address ueAAddr = ueAIface.GetAddress(0);
    NS_LOG_UNCOND("[IP] UE-A=" << ueAAddr << " (EPC-A)"
        << "  UE-B=" << ueBAddr << " (EPC-B, rebased)");

    // ── Attach UEs to eNBs (after EPC-B rebase — see note above) ─────────────
    // Attach() reads m_epcHelper too (via ActivateEpsBearer), so re-set it
    // per group even though it's already active from the install step above.
    lteHelper->SetEpcHelper(epcHelperA);
    lteHelper->Attach(ueADev.Get(0), enbDev1.Get(0));
    for (uint32_t i = 0; i < N_BG_UES; ++i)
        lteHelper->Attach(bgDev1.Get(i), enbDev1.Get(0));
    lteHelper->SetEpcHelper(epcHelperB);
    lteHelper->Attach(ueBDev.Get(0), enbDev2.Get(0));
    for (uint32_t i = 0; i < N_BG_UES; ++i)
        lteHelper->Attach(bgDev2.Get(i), enbDev2.Get(0));

    // ── Static routing: each UE uses its own EPC's GW as default route ───────
    Ipv4StaticRoutingHelper staticRouting;
    Ipv4Address ueGwA = epcHelperA->GetUeDefaultGatewayAddress();
    Ipv4Address ueGwB = epcHelperB->GetUeDefaultGatewayAddress();

    staticRouting.GetStaticRouting(ueANode.Get(0)->GetObject<Ipv4>())
        ->SetDefaultRoute(ueGwA, 1);
    staticRouting.GetStaticRouting(ueBNode.Get(0)->GetObject<Ipv4>())
        ->SetDefaultRoute(ueGwB, 1);
    for (uint32_t i = 0; i < N_BG_UES; ++i)
    {
        staticRouting.GetStaticRouting(bgNodes1.Get(i)->GetObject<Ipv4>())
            ->SetDefaultRoute(ueGwA, 1);
        staticRouting.GetStaticRouting(bgNodes2.Get(i)->GetObject<Ipv4>())
            ->SetDefaultRoute(ueGwB, 1);
    }

    // ── Handover trace ────────────────────────────────────────────────────────
    Config::ConnectWithoutContext(
        "/NodeList/*/DeviceList/*/LteUeRrc/HandoverEndOk",
        MakeCallback(&HandoverEndOkCallback));

    // ── Two PGW nodes ─────────────────────────────────────────────────────────
    Ptr<Node> pgwA = epcHelperA->GetPgwNode();
    Ptr<Node> pgwB = epcHelperB->GetPgwNode();

    // ── Inter-PGW "SGi" link — stands in for the real-world SGi interface ────
    // (a PGW's link out to the internet/inter-carrier interconnect). This is
    // the ONLY path between EPC-A and EPC-B, and is the monitored bottleneck
    // for the UE-A↔UE-B TCP flow (queue depth drives the congestion label).
    PointToPointHelper sgiLink;
    sgiLink.SetDeviceAttribute ("DataRate", StringValue(bottleneckBw));
    sgiLink.SetChannelAttribute("Delay",    StringValue("1ms"));
    sgiLink.SetQueue("ns3::DropTailQueue",
                     "MaxSize", StringValue(std::to_string(bottleneckQueue) + "p"));
    NetDeviceContainer sgiDevs = sgiLink.Install(pgwA, pgwB);

    Ipv4AddressHelper ipv4h;
    ipv4h.SetBase("15.0.0.0", "255.255.255.252");
    Ipv4InterfaceContainer sgiIface = ipv4h.Assign(sgiDevs);
    Ipv4Address pgwASgiAddr = sgiIface.GetAddress(0); // 15.0.0.1
    Ipv4Address pgwBSgiAddr = sgiIface.GetAddress(1); // 15.0.0.2

    // Ipv4AddressHelper::Assign() silently installs a default FqCoDel queue
    // disc on any device without one. That would put an AQM in front of the
    // "30p DropTail" buffer and change loss/delay behaviour in a way the
    // wired control would have to replicate. Remove it so the 30-packet
    // DropTail device queue is the ONLY buffer on the SGi link (identical in
    // scratch/wired_control.cc).
    {
        TrafficControlHelper tch;
        tch.Uninstall(sgiDevs);
    }

    // Monitor the SGi link queue on the PGW-A side (DL direction into EPC-B,
    // i.e. UE-A's monitored TCP data and coreServer's DL traffic to EPC-B).
    g_btlQueue = DynamicCast<DropTailQueue<Packet>>(
        DynamicCast<PointToPointNetDevice>(sgiDevs.Get(0))->GetQueue());
    NS_ASSERT_MSG(g_btlQueue, "Failed to get bottleneck DropTailQueue");

    int32_t pgwASgiIf = pgwA->GetObject<Ipv4>()->GetInterfaceForDevice(sgiDevs.Get(0));
    int32_t pgwBSgiIf = pgwB->GetObject<Ipv4>()->GetInterfaceForDevice(sgiDevs.Get(1));

    // Cross-EPC routes: each PGW reaches the other EPC's UE subnet via SGi.
    staticRouting.GetStaticRouting(pgwA->GetObject<Ipv4>())
        ->AddNetworkRouteTo(Ipv4Address("9.0.0.0"), Ipv4Mask("255.0.0.0"),
                            pgwBSgiAddr, pgwASgiIf);
    staticRouting.GetStaticRouting(pgwB->GetObject<Ipv4>())
        ->AddNetworkRouteTo(Ipv4Address("7.0.0.0"), Ipv4Mask("255.0.0.0"),
                            pgwASgiAddr, pgwBSgiIf);

    NS_LOG_UNCOND("[SGi] " << bottleneckBw << " queue=" << bottleneckQueue << "p"
        << "  pgwA=" << pgwASgiAddr << "  pgwB=" << pgwBSgiAddr);

    // ── Core server, generously-provisioned P2P leg off PGW-A ────────────────
    // (not the monitored bottleneck — that's the SGi link above)
    NodeContainer coreServerContainer;
    coreServerContainer.Create(1);
    Ptr<Node> coreServer = coreServerContainer.Get(0);
    internet.Install(coreServerContainer);

    PointToPointHelper coreLink;
    coreLink.SetDeviceAttribute ("DataRate", StringValue("100Mbps"));
    coreLink.SetChannelAttribute("Delay",    StringValue("1ms"));
    coreLink.SetQueue("ns3::DropTailQueue", "MaxSize", StringValue("100p"));

    // Install: coreServer (index 0) ↔ PGW-A (index 1)
    NetDeviceContainer btlDevs = coreLink.Install(coreServer, pgwA);

    Ipv4AddressHelper ipv4h2;
    ipv4h2.SetBase("10.1.0.0", "255.255.255.0");
    Ipv4InterfaceContainer btlIface = ipv4h2.Assign(btlDevs);
    Ipv4Address coreServerBtlAddr = btlIface.GetAddress(0); // 10.1.0.1
    Ipv4Address pgwABtlAddr       = btlIface.GetAddress(1); // 10.1.0.2

    // Routing: coreServer → EPC-A UEs (7.0.0.0/8) via PGW-A directly,
    //          coreServer → EPC-B UEs (9.0.0.0/8) via PGW-A, then across SGi
    staticRouting.GetStaticRouting(coreServer->GetObject<Ipv4>())
        ->AddNetworkRouteTo(Ipv4Address("7.0.0.0"), Ipv4Mask("255.0.0.0"),
                            pgwABtlAddr, 1);
    staticRouting.GetStaticRouting(coreServer->GetObject<Ipv4>())
        ->AddNetworkRouteTo(Ipv4Address("9.0.0.0"), Ipv4Mask("255.0.0.0"),
                            pgwABtlAddr, 1);

    // Routing: PGW-A → coreServer subnet (10.1.0.0/24) via its P2P interface
    {
        int32_t pgwABtlIf = pgwA->GetObject<Ipv4>()
                               ->GetInterfaceForDevice(btlDevs.Get(1));
        staticRouting.GetStaticRouting(pgwA->GetObject<Ipv4>())
            ->AddNetworkRouteTo(Ipv4Address("10.1.0.0"), Ipv4Mask("255.255.255.0"),
                                pgwABtlIf);
    }
    // Routing: PGW-B → coreServer subnet (10.1.0.0/24) via SGi (through PGW-A)
    staticRouting.GetStaticRouting(pgwB->GetObject<Ipv4>())
        ->AddNetworkRouteTo(Ipv4Address("10.1.0.0"), Ipv4Mask("255.255.255.0"),
                            pgwASgiAddr, pgwBSgiIf);

    NS_LOG_UNCOND("[BACKHAUL] coreServer=" << coreServerBtlAddr
        << "  pgwABtlAddr=" << pgwABtlAddr
        << "  UE-GW-A=" << ueGwA << "  UE-GW-B=" << ueGwB);

    // ── CQI trace on UE-A ─────────────────────────────────────────────────────
    {
        Ptr<Node> monNode = ueANode.Get(0);
        int32_t   lteIdx  = -1;
        for (uint32_t d = 0; d < monNode->GetNDevices(); ++d)
            if (DynamicCast<LteUeNetDevice>(monNode->GetDevice(d)))
            { lteIdx = static_cast<int32_t>(d); break; }

        if (lteIdx >= 0)
        {
            std::string cqiPath =
                "/NodeList/" + std::to_string(monNode->GetId())
                + "/DeviceList/" + std::to_string(lteIdx)
                + "/ComponentCarrierMapUe/0/LteUePhy/ReportCurrentCellRsrpSinr";
            Config::ConnectWithoutContext(cqiPath, MakeCallback(&CqiCallback));
            NS_LOG_UNCOND("[CQI] nodeId=" << monNode->GetId()
                << "  device=" << lteIdx);
        }
        else
        {
            NS_LOG_WARN("[CQI] No LteUeNetDevice found on UE-A — CQI trace skipped");
        }
    }

    // ── Applications ─────────────────────────────────────────────────────────

    // Override TCP CC for UE-A (already set globally, but explicit for clarity)
    Config::Set(
        "/NodeList/" + std::to_string(ueANode.Get(0)->GetId()) +
        "/$ns3::TcpL4Protocol/SocketType",
        TypeIdValue(TypeId::LookupByName("ns3::" + tcpVariant)));

    // TCP PacketSink on UE-B (port 5000)
    {
        PacketSinkHelper sinkH("ns3::TcpSocketFactory",
            InetSocketAddress(Ipv4Address::GetAny(), TCP_PORT));
        ApplicationContainer sinkApp = sinkH.Install(ueBNode.Get(0));
        sinkApp.Start(Seconds(appStart - 2.0));
        sinkApp.Stop(Seconds(simTime));
        g_ueBSink = DynamicCast<PacketSink>(sinkApp.Get(0));
    }

    // TCP BulkSend on UE-A → UE-B (monitored flow)
    {
        BulkSendHelper bulkH("ns3::TcpSocketFactory",
            InetSocketAddress(ueBAddr, TCP_PORT));
        bulkH.SetAttribute("MaxBytes",  UintegerValue(0));
        bulkH.SetAttribute("SendSize",  UintegerValue(1448));
        ApplicationContainer srcApp = bulkH.Install(ueANode.Get(0));
        srcApp.Start(Seconds(appStart));
        srcApp.Stop(Seconds(simTime));

        g_monSrcNode = ueANode.Get(0);
    }

    // UDP PacketSink on coreServer (UE-B uplink, port 6000)
    {
        PacketSinkHelper ulSinkH("ns3::UdpSocketFactory",
            InetSocketAddress(Ipv4Address::GetAny(), UDP_UL_PORT));
        ApplicationContainer ulSink = ulSinkH.Install(coreServer);
        ulSink.Start(Seconds(appStart - 2.0));
        ulSink.Stop(Seconds(simTime));
    }

    // UDP OnOff on UE-B → coreServer (uplink, Poisson ON=10s OFF=5s, 512Kbps)
    {
        OnOffHelper ueBUlHelper("ns3::UdpSocketFactory",
            InetSocketAddress(coreServerBtlAddr, UDP_UL_PORT));
        ueBUlHelper.SetAttribute("DataRate",   DataRateValue(DataRate(ueBUlRate)));
        ueBUlHelper.SetAttribute("PacketSize", UintegerValue(512));
        ueBUlHelper.SetAttribute("OnTime",
            StringValue("ns3::ExponentialRandomVariable[Mean=10]"));
        ueBUlHelper.SetAttribute("OffTime",
            StringValue("ns3::ExponentialRandomVariable[Mean=5]"));
        ApplicationContainer ueBUl = ueBUlHelper.Install(ueBNode.Get(0));
        ueBUl.Start(Seconds(appStart));
        ueBUl.Stop(Seconds(simTime));
    }

    // UDP PacketSink on each bg UE (DL from coreServer)
    for (uint32_t i = 0; i < N_BG_UES; ++i)
    {
        // eNB1 bg UEs: ports 7000–7003
        PacketSinkHelper s1("ns3::UdpSocketFactory",
            InetSocketAddress(Ipv4Address::GetAny(),
                static_cast<uint16_t>(UDP_DL_BASE + i)));
        s1.Install(bgNodes1.Get(i)).Start(Seconds(appStart - 2.0));

        // eNB2 bg UEs: ports 7004–7007
        PacketSinkHelper s2("ns3::UdpSocketFactory",
            InetSocketAddress(Ipv4Address::GetAny(),
                static_cast<uint16_t>(UDP_DL_BASE + N_BG_UES + i)));
        s2.Install(bgNodes2.Get(i)).Start(Seconds(appStart - 2.0));
    }

    // UDP OnOff on coreServer → each bg UE (DL, Poisson ON=bgOnMean OFF=bgOffMean, bgRate)
    std::string onTimeStr  = "ns3::ExponentialRandomVariable[Mean=" +
                             std::to_string(static_cast<int>(bgOnMean))  + "]";
    std::string offTimeStr = "ns3::ExponentialRandomVariable[Mean=" +
                             std::to_string(static_cast<int>(bgOffMean)) + "]";
    for (uint32_t i = 0; i < N_BG_UES; ++i)
    {
        // coreServer → bgUE-i on eNB1
        OnOffHelper dlH1("ns3::UdpSocketFactory",
            InetSocketAddress(bgIface1.GetAddress(i),
                static_cast<uint16_t>(UDP_DL_BASE + i)));
        dlH1.SetAttribute("DataRate",   DataRateValue(DataRate(bgRate)));
        dlH1.SetAttribute("PacketSize", UintegerValue(1400));
        dlH1.SetAttribute("OnTime",  StringValue(onTimeStr));
        dlH1.SetAttribute("OffTime", StringValue(offTimeStr));
        ApplicationContainer dl1 = dlH1.Install(coreServer);
        dl1.Start(Seconds(appStart + i * 2.0)); // stagger starts by 2s each
        dl1.Stop(Seconds(simTime));

        // coreServer → bgUE-i on eNB2
        OnOffHelper dlH2("ns3::UdpSocketFactory",
            InetSocketAddress(bgIface2.GetAddress(i),
                static_cast<uint16_t>(UDP_DL_BASE + N_BG_UES + i)));
        dlH2.SetAttribute("DataRate",   DataRateValue(DataRate(bgRate)));
        dlH2.SetAttribute("PacketSize", UintegerValue(1400));
        dlH2.SetAttribute("OnTime",  StringValue(onTimeStr));
        dlH2.SetAttribute("OffTime", StringValue(offTimeStr));
        ApplicationContainer dl2 = dlH2.Install(coreServer);
        dl2.Start(Seconds(appStart + i * 2.0 + 1.0)); // offset by 1s from eNB1 group
        dl2.Stop(Seconds(simTime));
    }

    NS_LOG_UNCOND("[BG-UES] " << N_BG_UES << " UEs/eNB  rate=" << bgRate
        << "  ON=" << bgOnMean << "s  OFF=" << bgOffMean << "s");

    // ── Open CSV files and write headers ──────────────────────────────────────
    g_rttFile.open("rtt_lte.csv");
    g_rttFile        << "timestamp_s,node_id,rtt_us\n";

    g_cwndFile.open("cwnd_lte.csv");
    g_cwndFile       << "timestamp_s,node_id,cwnd_bytes\n";

    g_rtoFile.open("rto_lte.csv");
    g_rtoFile        << "timestamp_s,node_id,rto_us\n";

    g_ackIatFile.open("ack_iat_lte.csv");
    g_ackIatFile     << "timestamp_s,node_id,ack_iat_us\n";

    g_queueFile.open("queue_lte.csv");
    g_queueFile      << "timestamp_s,queue_depth_packets\n";

    g_connFile.open("conn_lte.csv");
    g_connFile       << "timestamp_s,node_id,event\n";

    g_cqiFile.open("cqi_lte.csv");
    g_cqiFile        << "timestamp_s,rsrp_dbm,sinr_db,cqi_index\n";

    g_handoverFile.open("handover_lte.csv");
    g_handoverFile   << "timestamp_s,imsi,cell_id,rnti\n";

    g_throughputFile.open("throughput_lte.csv");
    g_throughputFile << "timestamp_s,rx_bytes_total,throughput_kbps\n";

    g_featFile.open("features_lte.csv");
    g_featFile       << "timestamp_s,rtt_us,cwnd_bytes,srtt_us,rttvar_us,"
                     << "rto_us,ack_iat_us,queue_depth,cqi,sinr_db,"
                     << "throughput_kbps,is_congestion,is_fading,event_label\n";

    // Claim-A ground-truth traces (see "Per-segment traces" above)
    g_txFile.open("tx_lte.csv");
    g_txFile        << "timestamp_s,seq,size,tsval_ms\n";
    g_retxFile.open("retx_lte.csv");
    g_retxFile      << "timestamp_s,seq,size,tsval_ms\n";
    g_rxFile.open("rx_lte.csv");
    g_rxFile        << "timestamp_s,seq,size,tsval_ms\n";
    g_timeoutFile.open("timeout_lte.csv");
    g_timeoutFile   << "timestamp_s,node_id,expired_rto_us,last_rtt_us,srtt_us,rttvar_us\n";
    g_congStateFile.open("cong_state_lte.csv");
    g_congStateFile << "timestamp_s,old_state,new_state\n";
    g_ackRxFile.open("ack_rx_lte.csv");
    g_ackRxFile     << "timestamp_s,ack,tsecr_ms\n";

    {
        std::ofstream bf("blockage_lte.csv");
        bf << "start_s,end_s,depth_db\n";
        for (const auto& e : g_blockEpisodes)
            bf << std::fixed << std::setprecision(6) << e.start << "," << e.end << ","
               << std::setprecision(2) << e.depth_db << "\n";
    }

    // Receiver tap on UE-B: every TCP data segment delivered to its IP layer.
    Config::ConnectWithoutContext(
        "/NodeList/" + std::to_string(ueBNode.Get(0)->GetId())
            + "/$ns3::Ipv4L3Protocol/LocalDeliver",
        MakeCallback(&RxLocalDeliver));

    // ── Schedule periodic events ──────────────────────────────────────────────
    uint32_t monNodeId = ueANode.Get(0)->GetId();

    Simulator::Schedule(Seconds(appStart + 0.001), &HookSocket, monNodeId);
    Simulator::Schedule(Seconds(1.0),              &SampleQueue);
    Simulator::Schedule(Seconds(appStart),         &SampleThroughput);
    Simulator::Schedule(Seconds(appStart),         &SampleFeatures);
    Simulator::Schedule(Seconds(appStart),         &Heartbeat, monNodeId);

    NS_LOG_UNCOND("=== LTE UE-to-UE Simulation ===");
    NS_LOG_UNCOND("  UE-A=" << ueAAddr << "  UE-B=" << ueBAddr);
    NS_LOG_UNCOND("  TCP=" << tcpVariant
        << "  simTime=" << simTime
        << "  appStart=" << appStart);
    NS_LOG_UNCOND("  Bottleneck=" << bottleneckBw
        << "  queue=" << bottleneckQueue << "p"
        << "  threshold=" << queueThreshold);

    // ── Run ───────────────────────────────────────────────────────────────────
    Simulator::Stop(Seconds(simTime));
    Simulator::Run();
    Simulator::Destroy();

    // ── Close CSV files ───────────────────────────────────────────────────────
    g_rttFile.close();
    g_cwndFile.close();
    g_rtoFile.close();
    g_ackIatFile.close();
    g_queueFile.close();
    g_connFile.close();
    g_cqiFile.close();
    g_handoverFile.close();
    g_throughputFile.close();
    g_featFile.close();
    g_txFile.close();
    g_retxFile.close();
    g_rxFile.close();
    g_timeoutFile.close();
    g_congStateFile.close();
    g_ackRxFile.close();

    NS_LOG_UNCOND("=== Simulation complete. CSV files written. ===");
    return 0;
}
