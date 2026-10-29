/*
 * wired_control.cc — Claim-A control arm: wired twin of lte_ue_to_ue.cc
 * ns-3.46 | TCP-RTO research
 *
 * PURPOSE
 *   Control group for "does stock RFC 6298 RTO fail on LTE?". This is the SAME
 *   network as scratch/lte_ue_to_ue.cc with the radio access removed: every
 *   wired element (S1 delays, SGi bottleneck rate/delay/queue, coreServer leg),
 *   the TCP stack (NewReno, MinRto=200ms) and the traffic mix are identical.
 *   Whatever differs in RTT/RTO behaviour is therefore attributable to the
 *   radio (scheduling, HARQ, RLC AM, fading), not to topology or parameters.
 *
 * TOPOLOGY  (5 hosts per side; "BS" = where each eNB sits, "GW" = where each PGW sits)
 *
 *   hostA0 (monitored sender, "UE-A")  ┐
 *   hostA1..4 (background)             ┴─ LAN-A (CSMA 100Mbps) ── BS-A ──[2ms]── GW-A ──┐
 *                                                                                        │
 *                                       coreServer ──[100Mbps/1ms]── GW-A                │
 *                                                                                        │
 *                  SGi bottleneck: GW-A ──[5Mbps / 1ms / 30p DropTail]── GW-B  ──────────┘
 *                                                                                        │
 *   hostB0 (TCP sink, "UE-B")          ┐                                                 │
 *   hostB1..4 (background)             ┴─ LAN-B (CSMA 100Mbps) ── BS-B ──[5ms]── GW-B ──┘
 *
 *   Monitored flow : hostA0 -> hostB0, TCP NewReno BulkSend, port 5000
 *   UDP uplink     : hostB0 -> coreServer, 512Kbps OnOff (Exp ON=10s OFF=5s), port 6000
 *   UDP downlink   : coreServer -> each of the 8 background hosts, 1Mbps OnOff
 *                    (Exp ON=15s OFF=10s), ports 7000..7007 (same offsets as the LTE model)
 *
 *   The S1-equivalent links (2ms / 5ms) are WIRED transport in the LTE model too, so
 *   they are kept; only the air interface is replaced by the 100Mbps CSMA LAN.
 *   Honest wired delays: no padding to match LTE's higher RTT floor. Compare
 *   normalised quantities (RTT/baseline, RTTVAR/SRTT, spurious-RTO rate).
 *
 * OUTAGE HOOK (control "Arm 2", unused by default)
 *   --outageSchedule=<file> with lines "start_s,duration_s" takes the SGi link
 *   down (100% packet loss, both directions) for each window. Replaying the
 *   LTE run's fade windows here lets us separate "any outage" from "LTE-specific".
 *   Also used to verify the spurious-timeout oracle with known outages.
 *
 * OUTPUT (same schema as lte_ue_to_ue.cc so one post-processor serves both)
 *   rtt_wired.csv, cwnd_wired.csv, rto_wired.csv, ack_iat_wired.csv,
 *   queue_wired.csv, throughput_wired.csv,
 *   tx_wired.csv, retx_wired.csv, rx_wired.csv (with TSval),
 *   timeout_wired.csv, cong_state_wired.csv
 *
 * RUN
 *   ./ns3 build scratch/wired_control
 *   ./ns3 run "scratch/wired_control --simTime=60 --RngRun=3"
 */

#include <cmath>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/csma-module.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/tcp-socket-base.h"
#include "ns3/traffic-control-module.h"

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("WiredControl");

static const uint32_t N_BG_PER_SIDE = 4;
static const uint32_t TCP_PORT      = 5000;
static const uint32_t UDP_UL_PORT   = 6000;
static const uint32_t UDP_DL_BASE   = 7000;

// ── CSV streams ──────────────────────────────────────────────────────────────
static std::ofstream g_rttFile, g_cwndFile, g_rtoFile, g_ackIatFile, g_queueFile,
    g_throughputFile, g_txFile, g_retxFile, g_rxFile, g_timeoutFile, g_congStateFile, g_ackRxFile;

// ── State ────────────────────────────────────────────────────────────────────
static double g_currentRtt_us = 0.0;
static double g_currentRto_us = 1e6;
static double g_srtt_us = 0.0, g_rttvar_us = 0.0;
static bool   g_rttFirst = true;
static Time   g_lastAckTime;
static bool   g_firstAck = true;
static Ptr<PacketSink>            g_sinkB;
static uint64_t                   g_lastRxBytes = 0;
static Ptr<DropTailQueue<Packet>> g_btlQueue;
static Ptr<Node>                  g_monSrcNode;
static std::set<void*>            g_hookedSockets;

// ── Per-segment traces (oracle ground truth; identical to the LTE model) ─────
static uint32_t TsValOf(const TcpHeader& h)
{
    if (!h.HasOption(TcpOption::TS))
        return 0;
    Ptr<const TcpOptionTS> ts = DynamicCast<const TcpOptionTS>(h.GetOption(TcpOption::TS));
    return ts ? ts->GetTimestamp() : 0;
}

static void TraceTx(Ptr<const Packet> p, const TcpHeader& h, Ptr<const TcpSocketBase>)
{
    if (p->GetSize() == 0)
        return;
    g_txFile << std::fixed << std::setprecision(6) << Simulator::Now().GetSeconds() << ","
             << h.GetSequenceNumber().GetValue() << "," << p->GetSize() << "," << TsValOf(h)
             << "\n";
}

static void TraceRetx(Ptr<const Packet> p, const TcpHeader& h, const Address&, const Address&,
                      Ptr<const TcpSocketBase>)
{
    g_retxFile << std::fixed << std::setprecision(6) << Simulator::Now().GetSeconds() << ","
               << h.GetSequenceNumber().GetValue() << "," << p->GetSize() << "," << TsValOf(h)
               << "\n";
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

static void TraceCongState(TcpSocketState::TcpCongState_t o, TcpSocketState::TcpCongState_t n)
{
    g_congStateFile << std::fixed << std::setprecision(6) << Simulator::Now().GetSeconds()
                    << "," << TcpSocketState::TcpCongStateName[o] << ","
                    << TcpSocketState::TcpCongStateName[n] << "\n";
}

static void RxLocalDeliver(const Ipv4Header& ip, Ptr<const Packet> pkt, uint32_t)
{
    if (ip.GetProtocol() != 6)
        return;
    Ptr<Packet> copy = pkt->Copy();
    TcpHeader th;
    uint32_t hlen = copy->PeekHeader(th);
    if (th.GetDestinationPort() != TCP_PORT || copy->GetSize() <= hlen)
        return;
    g_rxFile << std::fixed << std::setprecision(6) << Simulator::Now().GetSeconds() << ","
             << th.GetSequenceNumber().GetValue() << "," << (copy->GetSize() - hlen) << ","
             << TsValOf(th) << "\n";
}

// ── Sender-side TCP callbacks ────────────────────────────────────────────────
static void TraceRtt(uint32_t nodeId, Time, Time newVal)
{
    double rtt_us = static_cast<double>(newVal.GetMicroSeconds());
    if (rtt_us <= 0.0)
        return;
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
    g_rttFile << std::fixed << std::setprecision(6) << Simulator::Now().GetSeconds() << ","
              << nodeId << "," << static_cast<int64_t>(rtt_us) << "\n";
}

static void TraceCwnd(uint32_t nodeId, uint32_t, uint32_t newVal)
{
    g_cwndFile << std::fixed << std::setprecision(6) << Simulator::Now().GetSeconds() << ","
               << nodeId << "," << static_cast<int64_t>(newVal) << "\n";
}

static void TraceSsThresh(uint32_t nodeId, uint32_t, uint32_t newVal)
{
    g_cwndFile << std::fixed << std::setprecision(6) << Simulator::Now().GetSeconds() << ","
               << nodeId << "," << -static_cast<int64_t>(newVal) << "\n";
}

static void TraceRto(uint32_t nodeId, Time oldVal, Time newVal)
{
    g_currentRto_us = static_cast<double>(newVal.GetMicroSeconds());
    g_rtoFile << std::fixed << std::setprecision(6) << Simulator::Now().GetSeconds() << ","
              << nodeId << "," << static_cast<int64_t>(g_currentRto_us) << "\n";
    // RTO expiry = exact doubling by ReTxTimeout's exponential backoff (see
    // lte_ue_to_ue.cc TraceRto for the full rationale).
    if (oldVal.IsStrictlyPositive() && newVal == oldVal + oldVal)
    {
        g_timeoutFile << std::fixed << std::setprecision(6) << Simulator::Now().GetSeconds()
                      << "," << nodeId << "," << oldVal.GetMicroSeconds() << ","
                      << static_cast<int64_t>(g_currentRtt_us) << ","
                      << static_cast<int64_t>(g_srtt_us) << ","
                      << static_cast<int64_t>(g_rttvar_us) << "\n";
    }
}

static void TraceAckIat(uint32_t nodeId, SequenceNumber32, SequenceNumber32)
{
    Time now = Simulator::Now();
    if (!g_firstAck)
    {
        double iat_us = static_cast<double>((now - g_lastAckTime).GetMicroSeconds());
        if (iat_us > 0.0)
            g_ackIatFile << std::fixed << std::setprecision(6) << now.GetSeconds() << ","
                         << nodeId << "," << static_cast<int64_t>(iat_us) << "\n";
    }
    g_lastAckTime = now;
    g_firstAck    = false;
}

static void HookSocket(uint32_t nodeId)
{
    for (uint32_t k = 0; k < g_monSrcNode->GetNApplications(); ++k)
    {
        Ptr<BulkSendApplication> bulk =
            DynamicCast<BulkSendApplication>(g_monSrcNode->GetApplication(k));
        if (!bulk)
            continue;
        Ptr<Socket> sock = bulk->GetSocket();
        if (!sock || g_hookedSockets.count(PeekPointer(sock)))
            continue;
        Ptr<TcpSocketBase> tcp = DynamicCast<TcpSocketBase>(sock);
        if (!tcp)
            continue;
        tcp->TraceConnectWithoutContext("RTT", MakeBoundCallback(&TraceRtt, nodeId));
        tcp->TraceConnectWithoutContext("CongestionWindow", MakeBoundCallback(&TraceCwnd, nodeId));
        tcp->TraceConnectWithoutContext("SlowStartThreshold",
                                        MakeBoundCallback(&TraceSsThresh, nodeId));
        tcp->TraceConnectWithoutContext("RTO", MakeBoundCallback(&TraceRto, nodeId));
        tcp->TraceConnectWithoutContext("HighestRxAck", MakeBoundCallback(&TraceAckIat, nodeId));
        tcp->TraceConnectWithoutContext("Tx", MakeCallback(&TraceTx));
        tcp->TraceConnectWithoutContext("Retransmission", MakeCallback(&TraceRetx));
        tcp->TraceConnectWithoutContext("CongState", MakeCallback(&TraceCongState));
        tcp->TraceConnectWithoutContext("Rx",            MakeCallback(&TraceAckRx));
        g_hookedSockets.insert(PeekPointer(sock));
        NS_LOG_UNCOND("[HookSocket] t=" << Simulator::Now().GetSeconds()
                                        << "s  hooked sender nodeId=" << nodeId);
    }
}

static void SampleQueue()
{
    uint32_t depth = g_btlQueue ? g_btlQueue->GetNPackets() : 0;
    g_queueFile << std::fixed << std::setprecision(6) << Simulator::Now().GetSeconds() << ","
                << depth << "\n";
    Simulator::Schedule(MilliSeconds(100), &SampleQueue);
}

static void SampleThroughput()
{
    uint64_t rx    = g_sinkB ? g_sinkB->GetTotalRx() : 0;
    uint64_t delta = (rx >= g_lastRxBytes) ? (rx - g_lastRxBytes) : 0;
    double   kbps  = (static_cast<double>(delta) * 8.0) / (0.5 * 1e3);
    g_throughputFile << std::fixed << std::setprecision(6) << Simulator::Now().GetSeconds()
                     << "," << rx << "," << std::setprecision(2) << kbps << "\n";
    g_lastRxBytes = rx;
    Simulator::Schedule(MilliSeconds(500), &SampleThroughput);
}

static void Heartbeat(uint32_t monNodeId)
{
    NS_LOG_UNCOND("t=" << std::fixed << std::setprecision(1) << Simulator::Now().GetSeconds()
                       << "s  RTT=" << static_cast<int64_t>(g_currentRtt_us / 1000)
                       << "ms  RTO=" << static_cast<int64_t>(g_currentRto_us / 1000)
                       << "ms  Queue=" << (g_btlQueue ? g_btlQueue->GetNPackets() : 0) << "p");
    HookSocket(monNodeId); // retry in case the socket did not exist yet
    Simulator::Schedule(Seconds(5.0), &Heartbeat, monNodeId);
}

// ── Outage hook (Arm 2 / oracle verification) ────────────────────────────────
static void SetOutage(Ptr<RateErrorModel> a, Ptr<RateErrorModel> b, bool down)
{
    if (down)
    {
        a->Enable();
        b->Enable();
    }
    else
    {
        a->Disable();
        b->Disable();
    }
    NS_LOG_UNCOND("[OUTAGE] t=" << std::fixed << std::setprecision(3)
                                << Simulator::Now().GetSeconds() << "s  SGi link "
                                << (down ? "DOWN" : "UP"));
}

int main(int argc, char* argv[])
{
    double      simTime         = 300.0;
    double      appStart        = 5.0;
    std::string s1DelayA        = "2ms";   // BS-A <-> GW-A  (S1-U equivalent)
    std::string s1DelayB        = "5ms";   // BS-B <-> GW-B
    std::string bottleneckBw    = "5Mbps";
    uint32_t    bottleneckQueue = 30;
    std::string bgRate          = "1Mbps";
    double      bgOnMean        = 15.0;
    double      bgOffMean       = 10.0;
    std::string ueBUlRate       = "512Kbps";
    std::string tcpVariant      = "TcpNewReno";
    std::string outageSchedule  = "";
    std::string minRto          = "200ms"; // test-only knob; keep 200ms for the study

    CommandLine cmd(__FILE__);
    cmd.AddValue("simTime", "Simulation duration (s)", simTime);
    cmd.AddValue("appStart", "Application start time (s)", appStart);
    cmd.AddValue("s1DelayA", "BS-A<->GW-A one-way delay", s1DelayA);
    cmd.AddValue("s1DelayB", "BS-B<->GW-B one-way delay", s1DelayB);
    cmd.AddValue("bottleneckBw", "SGi bottleneck bandwidth", bottleneckBw);
    cmd.AddValue("bottleneckQueue", "SGi queue depth (packets)", bottleneckQueue);
    cmd.AddValue("bgRate", "Background DL rate per host when ON", bgRate);
    cmd.AddValue("bgOnMean", "Background mean ON duration (s)", bgOnMean);
    cmd.AddValue("bgOffMean", "Background mean OFF duration (s)", bgOffMean);
    cmd.AddValue("ueBUlRate", "hostB0 UDP uplink rate", ueBUlRate);
    cmd.AddValue("tcpVariant", "TCP variant for the monitored flow", tcpVariant);
    cmd.AddValue("minRto", "TcpSocketBase MinRto (study uses 200ms; lower only for oracle tests)", minRto);
    cmd.AddValue("outageSchedule", "File of 'start_s,duration_s' SGi outage windows", outageSchedule);
    cmd.Parse(argc, argv);

    {
        UintegerValue run;
        GlobalValue::GetValueByName("RngRun", run);
        NS_LOG_UNCOND("[SEED] RngRun=" << run.Get() << "  outageSchedule='" << outageSchedule << "'");
    }

    // ── TCP defaults: IDENTICAL to lte_ue_to_ue.cc ───────────────────────────
    Config::SetDefault("ns3::TcpL4Protocol::SocketType",
                       TypeIdValue(TypeId::LookupByName("ns3::" + tcpVariant)));
    Config::SetDefault("ns3::TcpSocket::SndBufSize", UintegerValue(512 * 1024));
    Config::SetDefault("ns3::TcpSocket::RcvBufSize", UintegerValue(512 * 1024));
    Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(1448));
    Config::SetDefault("ns3::TcpSocket::DelAckCount", UintegerValue(1));
    Config::SetDefault("ns3::TcpSocket::DataRetries", UintegerValue(20));
    Config::SetDefault("ns3::TcpSocket::ConnCount", UintegerValue(20));
    Config::SetDefault("ns3::TcpSocketBase::MinRto", TimeValue(Time(minRto)));

    // ── Nodes ────────────────────────────────────────────────────────────────
    NodeContainer hostsA, hostsB, bsA, bsB, gwA, gwB, core;
    hostsA.Create(1 + N_BG_PER_SIDE); // [0] = monitored sender, [1..4] = background
    hostsB.Create(1 + N_BG_PER_SIDE); // [0] = monitored sink,   [1..4] = background
    bsA.Create(1);
    bsB.Create(1);
    gwA.Create(1);
    gwB.Create(1);
    core.Create(1);

    InternetStackHelper internet;
    internet.InstallAll();

    // ── LANs (replace the air interface) ─────────────────────────────────────
    CsmaHelper csma;
    csma.SetChannelAttribute("DataRate", StringValue("100Mbps"));
    csma.SetChannelAttribute("Delay", StringValue("10us"));

    NodeContainer lanA(hostsA, bsA);
    NodeContainer lanB(hostsB, bsB);
    NetDeviceContainer lanADevs = csma.Install(lanA);
    NetDeviceContainer lanBDevs = csma.Install(lanB);

    // ── Wired transport (S1-equivalent), SGi bottleneck, coreServer leg ──────
    PointToPointHelper s1A;
    s1A.SetDeviceAttribute("DataRate", StringValue("1Gbps"));
    s1A.SetChannelAttribute("Delay", StringValue(s1DelayA));
    NetDeviceContainer s1ADevs = s1A.Install(bsA.Get(0), gwA.Get(0));

    PointToPointHelper s1B;
    s1B.SetDeviceAttribute("DataRate", StringValue("1Gbps"));
    s1B.SetChannelAttribute("Delay", StringValue(s1DelayB));
    NetDeviceContainer s1BDevs = s1B.Install(bsB.Get(0), gwB.Get(0));

    PointToPointHelper sgi;
    sgi.SetDeviceAttribute("DataRate", StringValue(bottleneckBw));
    sgi.SetChannelAttribute("Delay", StringValue("1ms"));
    sgi.SetQueue("ns3::DropTailQueue", "MaxSize",
                 StringValue(std::to_string(bottleneckQueue) + "p"));
    NetDeviceContainer sgiDevs = sgi.Install(gwA.Get(0), gwB.Get(0));

    PointToPointHelper coreLink;
    coreLink.SetDeviceAttribute("DataRate", StringValue("100Mbps"));
    coreLink.SetChannelAttribute("Delay", StringValue("1ms"));
    coreLink.SetQueue("ns3::DropTailQueue", "MaxSize", StringValue("100p"));
    NetDeviceContainer coreDevs = coreLink.Install(core.Get(0), gwA.Get(0));

    // ── Addressing ───────────────────────────────────────────────────────────
    Ipv4AddressHelper ip;
    ip.SetBase("192.168.1.0", "255.255.255.0");
    Ipv4InterfaceContainer lanAIf = ip.Assign(lanADevs);
    ip.SetBase("192.168.2.0", "255.255.255.0");
    Ipv4InterfaceContainer lanBIf = ip.Assign(lanBDevs);
    ip.SetBase("10.0.1.0", "255.255.255.252");
    ip.Assign(s1ADevs);
    ip.SetBase("10.0.2.0", "255.255.255.252");
    ip.Assign(s1BDevs);
    ip.SetBase("15.0.0.0", "255.255.255.252");
    ip.Assign(sgiDevs);
    ip.SetBase("10.1.0.0", "255.255.255.0");
    Ipv4InterfaceContainer coreIf = ip.Assign(coreDevs);

    // Assign() installs a default FqCoDel queue disc; remove it on the SGi link so the
    // 30-packet DropTail device queue is the only buffer (identical to the LTE model).
    {
        TrafficControlHelper tch;
        tch.Uninstall(sgiDevs);
    }

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    Ipv4Address hostB0Addr  = lanBIf.GetAddress(0);
    Ipv4Address coreAddr    = coreIf.GetAddress(0);

    g_btlQueue = DynamicCast<DropTailQueue<Packet>>(
        DynamicCast<PointToPointNetDevice>(sgiDevs.Get(0))->GetQueue());
    NS_ASSERT_MSG(g_btlQueue, "Failed to get SGi DropTailQueue");

    NS_LOG_UNCOND("[WIRED] hostA0=" << lanAIf.GetAddress(0) << "  hostB0=" << hostB0Addr
                                    << "  coreServer=" << coreAddr << "  SGi=" << bottleneckBw
                                    << " queue=" << bottleneckQueue << "p");

    // ── Applications ─────────────────────────────────────────────────────────
    // Monitored TCP: hostA0 -> hostB0
    {
        PacketSinkHelper sinkH("ns3::TcpSocketFactory",
                               InetSocketAddress(Ipv4Address::GetAny(), TCP_PORT));
        ApplicationContainer sinkApp = sinkH.Install(hostsB.Get(0));
        sinkApp.Start(Seconds(appStart - 2.0));
        sinkApp.Stop(Seconds(simTime));
        g_sinkB = DynamicCast<PacketSink>(sinkApp.Get(0));

        BulkSendHelper bulkH("ns3::TcpSocketFactory", InetSocketAddress(hostB0Addr, TCP_PORT));
        bulkH.SetAttribute("MaxBytes", UintegerValue(0));
        bulkH.SetAttribute("SendSize", UintegerValue(1448));
        ApplicationContainer srcApp = bulkH.Install(hostsA.Get(0));
        srcApp.Start(Seconds(appStart));
        srcApp.Stop(Seconds(simTime));
        g_monSrcNode = hostsA.Get(0);
    }

    // UDP uplink: hostB0 -> coreServer
    {
        PacketSinkHelper ulSink("ns3::UdpSocketFactory",
                                InetSocketAddress(Ipv4Address::GetAny(), UDP_UL_PORT));
        ApplicationContainer s = ulSink.Install(core.Get(0));
        s.Start(Seconds(appStart - 2.0));
        s.Stop(Seconds(simTime));

        OnOffHelper ul("ns3::UdpSocketFactory", InetSocketAddress(coreAddr, UDP_UL_PORT));
        ul.SetAttribute("DataRate", DataRateValue(DataRate(ueBUlRate)));
        ul.SetAttribute("PacketSize", UintegerValue(512));
        ul.SetAttribute("OnTime", StringValue("ns3::ExponentialRandomVariable[Mean=10]"));
        ul.SetAttribute("OffTime", StringValue("ns3::ExponentialRandomVariable[Mean=5]"));
        ApplicationContainer a = ul.Install(hostsB.Get(0));
        a.Start(Seconds(appStart));
        a.Stop(Seconds(simTime));
    }

    // UDP downlink: coreServer -> 4 background hosts per side (same ports/offsets as LTE)
    std::string onStr  = "ns3::ExponentialRandomVariable[Mean=" +
                         std::to_string(static_cast<int>(bgOnMean)) + "]";
    std::string offStr = "ns3::ExponentialRandomVariable[Mean=" +
                         std::to_string(static_cast<int>(bgOffMean)) + "]";
    for (uint32_t i = 0; i < N_BG_PER_SIDE; ++i)
    {
        // side A background host i (ports 7000..7003)
        PacketSinkHelper s1("ns3::UdpSocketFactory",
                            InetSocketAddress(Ipv4Address::GetAny(),
                                              static_cast<uint16_t>(UDP_DL_BASE + i)));
        s1.Install(hostsA.Get(1 + i)).Start(Seconds(appStart - 2.0));

        OnOffHelper d1("ns3::UdpSocketFactory",
                       InetSocketAddress(lanAIf.GetAddress(1 + i),
                                         static_cast<uint16_t>(UDP_DL_BASE + i)));
        d1.SetAttribute("DataRate", DataRateValue(DataRate(bgRate)));
        d1.SetAttribute("PacketSize", UintegerValue(1400));
        d1.SetAttribute("OnTime", StringValue(onStr));
        d1.SetAttribute("OffTime", StringValue(offStr));
        ApplicationContainer da = d1.Install(core.Get(0));
        da.Start(Seconds(appStart + i * 2.0));
        da.Stop(Seconds(simTime));

        // side B background host i (ports 7004..7007)
        PacketSinkHelper s2("ns3::UdpSocketFactory",
                            InetSocketAddress(Ipv4Address::GetAny(),
                                              static_cast<uint16_t>(UDP_DL_BASE + N_BG_PER_SIDE + i)));
        s2.Install(hostsB.Get(1 + i)).Start(Seconds(appStart - 2.0));

        OnOffHelper d2("ns3::UdpSocketFactory",
                       InetSocketAddress(lanBIf.GetAddress(1 + i),
                                         static_cast<uint16_t>(UDP_DL_BASE + N_BG_PER_SIDE + i)));
        d2.SetAttribute("DataRate", DataRateValue(DataRate(bgRate)));
        d2.SetAttribute("PacketSize", UintegerValue(1400));
        d2.SetAttribute("OnTime", StringValue(onStr));
        d2.SetAttribute("OffTime", StringValue(offStr));
        ApplicationContainer db = d2.Install(core.Get(0));
        db.Start(Seconds(appStart + i * 2.0 + 1.0));
        db.Stop(Seconds(simTime));
    }

    // ── Outage schedule (Arm 2 hook) ─────────────────────────────────────────
    if (!outageSchedule.empty())
    {
        Ptr<RateErrorModel> emA = CreateObject<RateErrorModel>();
        Ptr<RateErrorModel> emB = CreateObject<RateErrorModel>();
        for (auto em : {emA, emB})
        {
            em->SetUnit(RateErrorModel::ERROR_UNIT_PACKET);
            em->SetRate(1.0);
            em->Disable();
        }
        sgiDevs.Get(0)->SetAttribute("ReceiveErrorModel", PointerValue(emA));
        sgiDevs.Get(1)->SetAttribute("ReceiveErrorModel", PointerValue(emB));

        std::ifstream f(outageSchedule);
        std::string   line;
        uint32_t      n = 0;
        while (std::getline(f, line))
        {
            if (line.empty() || line[0] == '#')
                continue;
            double start = 0.0, dur = 0.0;
            char   comma;
            std::istringstream ss(line);
            if (!(ss >> start >> comma >> dur))
                continue;
            Simulator::Schedule(Seconds(start), &SetOutage, emA, emB, true);
            Simulator::Schedule(Seconds(start + dur), &SetOutage, emA, emB, false);
            ++n;
        }
        NS_LOG_UNCOND("[OUTAGE] loaded " << n << " windows from " << outageSchedule);
    }

    // ── CSVs ─────────────────────────────────────────────────────────────────
    g_rttFile.open("rtt_wired.csv");           g_rttFile << "timestamp_s,node_id,rtt_us\n";
    g_cwndFile.open("cwnd_wired.csv");         g_cwndFile << "timestamp_s,node_id,cwnd_bytes\n";
    g_rtoFile.open("rto_wired.csv");           g_rtoFile << "timestamp_s,node_id,rto_us\n";
    g_ackIatFile.open("ack_iat_wired.csv");    g_ackIatFile << "timestamp_s,node_id,ack_iat_us\n";
    g_queueFile.open("queue_wired.csv");       g_queueFile << "timestamp_s,queue_depth_packets\n";
    g_throughputFile.open("throughput_wired.csv");
    g_throughputFile << "timestamp_s,rx_bytes_total,throughput_kbps\n";
    g_txFile.open("tx_wired.csv");             g_txFile << "timestamp_s,seq,size,tsval_ms\n";
    g_retxFile.open("retx_wired.csv");         g_retxFile << "timestamp_s,seq,size,tsval_ms\n";
    g_rxFile.open("rx_wired.csv");             g_rxFile << "timestamp_s,seq,size,tsval_ms\n";
    g_timeoutFile.open("timeout_wired.csv");
    g_timeoutFile << "timestamp_s,node_id,expired_rto_us,last_rtt_us,srtt_us,rttvar_us\n";
    g_congStateFile.open("cong_state_wired.csv");
    g_congStateFile << "timestamp_s,old_state,new_state\n";
    g_ackRxFile.open("ack_rx_wired.csv");
    g_ackRxFile << "timestamp_s,ack,tsecr_ms\n";

    Config::ConnectWithoutContext("/NodeList/" + std::to_string(hostsB.Get(0)->GetId()) +
                                      "/$ns3::Ipv4L3Protocol/LocalDeliver",
                                  MakeCallback(&RxLocalDeliver));

    uint32_t monNodeId = hostsA.Get(0)->GetId();
    Simulator::Schedule(Seconds(appStart + 0.001), &HookSocket, monNodeId);
    Simulator::Schedule(Seconds(1.0), &SampleQueue);
    Simulator::Schedule(Seconds(appStart), &SampleThroughput);
    Simulator::Schedule(Seconds(appStart), &Heartbeat, monNodeId);

    Simulator::Stop(Seconds(simTime));
    Simulator::Run();
    Simulator::Destroy();

    for (auto* f : {&g_rttFile, &g_cwndFile, &g_rtoFile, &g_ackIatFile, &g_queueFile,
                    &g_throughputFile, &g_txFile, &g_retxFile, &g_rxFile, &g_timeoutFile,
                    &g_congStateFile, &g_ackRxFile})
        f->close();

    NS_LOG_UNCOND("=== Wired control complete. CSV files written. ===");
    return 0;
}
