/*
 * IJM Kasus 7: MANET hibrida pos pengungsian.
 * Varian A: 12 node bergerak saja. Varian B: 12 node bergerak + 4 node statis.
 * Routing AODV, Wi-Fi 802.11b ad hoc, RangePropagationLoss.
 *
 * Dasar: examples/routing/manet-routing-compare.cc (ns-3.49)
 *
 * STATUS: CP3 (validasi): tambahan trace relay (UnicastForward), cek konektivitas posisi,
 * dan pemisahan byte hello AODV. Tidak ada RNG yang disentuh, jadi hasil seed 1 harus sama
 * persis dengan CP2 (sent/rcvd).
 */

#include "ns3/aodv-module.h"
#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/network-module.h"
#include "ns3/yans-wifi-helper.h"

#include "ns3/ipv4-header.h"
#include "ns3/ipv4-l3-protocol.h"
#include "ns3/udp-header.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("manet-kasus7");

/**
 * Allocator waypoint: memilih ACAK satu dari daftar titik tiap dipanggil.
 * (ListPositionAllocator bawaan memilih berurutan, bukan acak.)
 */
class WaypointAllocator : public PositionAllocator
{
  public:
    static TypeId GetTypeId()
    {
        static TypeId tid = TypeId("ns3::WaypointAllocator")
                                .SetParent<PositionAllocator>()
                                .SetGroupName("Mobility")
                                .AddConstructor<WaypointAllocator>();
        return tid;
    }

    WaypointAllocator()
    {
        m_rv = CreateObject<UniformRandomVariable>();
    }

    void Add(Vector v)
    {
        m_points.push_back(v);
    }

    Vector GetNext() const override
    {
        uint32_t i = m_rv->GetInteger(0, static_cast<uint32_t>(m_points.size()) - 1);
        return m_points[i];
    }

    int64_t AssignStreams(int64_t stream) override
    {
        m_rv->SetStream(stream);
        return 1;
    }

  private:
    std::vector<Vector> m_points;
    Ptr<UniformRandomVariable> m_rv;
};

NS_OBJECT_ENSURE_REGISTERED(WaypointAllocator);

// ---- Trafik: hitung di sisi aplikasi (definisi PDR dan delay di brief bagian 7) ----
static const uint32_t kPayload = 128; // byte per paket, termasuk SeqTsHeader (12 byte)

struct AppStats
{
    uint64_t sent{0};
    uint64_t rcvd{0};
    uint64_t rcvdBytes{0}; // byte data diterima aplikasi (penyebut overhead)
    double delaySum{0.0};  // detik
};

static AppStats g_stats;

// Paket kontrol AODV = UDP port 654 (RREQ, RREP termasuk hello, RERR).
// Dihitung tiap kali dikirim keluar dari sebuah antarmuka IP (tiap hop dan tiap forward RREQ
// dihitung), di semua node termasuk node statis, hanya setelah warm-up.
static const uint16_t kAodvPort = 654;
static uint64_t g_ctrlPkts = 0;
static uint64_t g_ctrlBytes = 0;
static uint64_t g_helloPkts = 0;  // subset ctrl: hello AODV (RREP broadcast)
static uint64_t g_helloBytes = 0;
static Time g_warmupTime;

static void
OnIpTx(Ptr<const Packet> packet, Ptr<Ipv4>, uint32_t)
{
    if (Simulator::Now() < g_warmupTime)
    {
        return;
    }
    Ptr<Packet> q = packet->Copy();
    Ipv4Header ip;
    q->RemoveHeader(ip);
    if (ip.GetProtocol() != 17) // bukan UDP
    {
        return;
    }
    UdpHeader udp;
    q->RemoveHeader(udp);
    if (udp.GetDestinationPort() == kAodvPort)
    {
        g_ctrlPkts++;
        g_ctrlBytes += packet->GetSize(); // termasuk header IP dan UDP

        // Hello AODV = RREP (tipe 2, byte pertama payload) yang dikirim ke alamat broadcast
        // (RFC 3561 bagian 6.9). RREP biasa selalu unicast, jadi syarat broadcast cukup.
        if (q->GetSize() >= 1)
        {
            uint8_t type = 0;
            q->CopyData(&type, 1);
            Ipv4Address d = ip.GetDestination();
            if (type == 2 && (d.IsBroadcast() || d.IsSubnetDirectedBroadcast(Ipv4Mask("255.255.255.0"))))
            {
                g_helloPkts++;
                g_helloBytes += packet->GetSize();
            }
        }
    }
}

// ---- Validasi CP3 ----
// (a) Bukti relay: hitung paket DATA (UDP port 9) yang di-forward tiap node.
static const uint16_t kDataPort = 9;
static std::vector<uint64_t> g_fwdData; // indeks = id node

static void
OnFwd(uint32_t nodeId, const Ipv4Header& ip, Ptr<const Packet> packet, uint32_t)
{
    if (Simulator::Now() < g_warmupTime || ip.GetProtocol() != 17)
    {
        return;
    }
    UdpHeader udp;
    packet->PeekHeader(udp);
    if (udp.GetDestinationPort() == kDataPort)
    {
        g_fwdData[nodeId]++;
    }
}

// (b) Cek partisi: tiap 5 s, bangun graf dari posisi + radius (sama dengan RangePropagationLoss),
// lalu cek apakah pasangan sumber-tujuan terhubung lewat jalur multi-hop apa pun.
// Ini batas atas yang "diizinkan topologi", bukan prediksi PDR AODV.
static uint64_t g_connSamples = 0;
static uint64_t g_connAllOk = 0; // seluruh node dalam 1 komponen
static uint64_t g_pairSamples = 0;
static uint64_t g_pairOk = 0;     // pasangan (i, i+6) terhubung

static void
CheckConn(NodeContainer nodes, double range, uint32_t nFlows, Time stop)
{
    if (Simulator::Now() > stop)
    {
        return;
    }
    const uint32_t n = nodes.GetN();
    std::vector<Vector> pos(n);
    for (uint32_t i = 0; i < n; i++)
    {
        pos[i] = nodes.Get(i)->GetObject<MobilityModel>()->GetPosition();
    }
    std::vector<int> comp(n, -1);
    int nComp = 0;
    for (uint32_t s = 0; s < n; s++)
    {
        if (comp[s] >= 0)
        {
            continue;
        }
        std::vector<uint32_t> stack{s};
        comp[s] = nComp;
        while (!stack.empty())
        {
            uint32_t u = stack.back();
            stack.pop_back();
            for (uint32_t v = 0; v < n; v++)
            {
                if (comp[v] < 0 && CalculateDistance(pos[u], pos[v]) <= range)
                {
                    comp[v] = nComp;
                    stack.push_back(v);
                }
            }
        }
        nComp++;
    }
    g_connSamples++;
    if (nComp == 1)
    {
        g_connAllOk++;
    }
    for (uint32_t f = 0; f < nFlows; f++)
    {
        g_pairSamples++;
        if (comp[f] == comp[f + nFlows])
        {
            g_pairOk++;
        }
    }
    Simulator::Schedule(Seconds(5.0), &CheckConn, nodes, range, nFlows, stop);
}

static void
SendOne(Ptr<Socket> sock, uint32_t seq, Time interval, Time stop)
{
    if (Simulator::Now() >= stop)
    {
        return;
    }
    SeqTsHeader h;
    h.SetSeq(seq);
    Ptr<Packet> p = Create<Packet>(kPayload - h.GetSerializedSize());
    p->AddHeader(h);
    sock->Send(p);
    g_stats.sent++;
    Simulator::Schedule(interval, &SendOne, sock, seq + 1, interval, stop);
}

static void
OnRecv(Ptr<Socket> sock)
{
    Ptr<Packet> p;
    Address from;
    while ((p = sock->RecvFrom(from)))
    {
        g_stats.rcvdBytes += p->GetSize();
        SeqTsHeader h;
        p->RemoveHeader(h);
        g_stats.rcvd++;
        g_stats.delaySum += (Simulator::Now() - h.GetTs()).GetSeconds();
    }
}

// Hanya untuk validasi gerak identik antar varian (aktif dengan --debugPos=1).
static void
PrintPos(Ptr<Node> n)
{
    Vector p = n->GetObject<MobilityModel>()->GetPosition();
    NS_LOG_UNCOND("t=" << Simulator::Now().GetSeconds() << " node" << n->GetId() << " (" << p.x
                       << "," << p.y << ")");
}

int
main(int argc, char* argv[])
{
    // ---- Parameter (nilai default = brief bagian 4, semuanya ASUMSI) ----
    std::string variant = "A"; // A = tanpa node statis, B = dengan 4 node statis
    uint32_t seed = 1;         // RngRun, 1..15
    uint32_t nMobile = 12;
    uint32_t nStatic = 4;
    double range = 250.0;      // m
    double simTime = 600.0;    // s
    double warmup = 60.0;      // s, dipakai di Step 3
    std::string outDir = "results"; // folder CSV, dibuat otomatis kalau belum ada
    bool debugPos = false;     // cetak posisi beberapa node untuk validasi

    CommandLine cmd(__FILE__);
    cmd.AddValue("variant", "A (tanpa node statis) atau B (dengan node statis)", variant);
    cmd.AddValue("seed", "Nomor run RNG (1..15)", seed);
    cmd.AddValue("range", "Radius radio (m)", range);
    cmd.AddValue("simTime", "Durasi simulasi (s)", simTime);
    cmd.AddValue("warmup", "Warm-up sebelum trafik mulai (s)", warmup);
    cmd.AddValue("outDir", "Folder keluaran CSV", outDir);
    cmd.AddValue("debugPos", "Cetak posisi node 0 dan 1 pada t=100,300,500", debugPos);
    cmd.Parse(argc, argv);

    NS_ABORT_MSG_IF(variant != "A" && variant != "B", "variant harus A atau B");
    const bool withStatic = (variant == "B");

    // Seed tetap 1, yang divariasikan nomor run. Harus dipanggil sebelum membuat objek acak apa pun.
    RngSeedManager::SetSeed(1);
    RngSeedManager::SetRun(seed);

    Config::SetDefault("ns3::WifiRemoteStationManager::NonUnicastMode",
                       StringValue("DsssRate11Mbps"));

    // ---- Node ----
    // Node bergerak dibuat DULUAN (id 0..11) supaya id dan IP mereka identik di A dan B.
    NodeContainer mobileNodes;
    mobileNodes.Create(nMobile);
    NodeContainer staticNodes;
    if (withStatic)
    {
        staticNodes.Create(nStatic); // id 12..15, hanya di varian B
    }
    NodeContainer allNodes(mobileNodes, staticNodes);

    // ---- Wi-Fi ad hoc + RangePropagationLoss ----
    WifiHelper wifi;
    wifi.SetStandard(WIFI_STANDARD_80211b);

    YansWifiChannelHelper wifiChannel;
    wifiChannel.SetPropagationDelay("ns3::ConstantSpeedPropagationDelayModel");
    wifiChannel.AddPropagationLoss("ns3::RangePropagationLossModel",
                                   "MaxRange",
                                   DoubleValue(range));
    YansWifiPhyHelper wifiPhy;
    wifiPhy.SetChannel(wifiChannel.Create());

    wifi.SetRemoteStationManager("ns3::ConstantRateWifiManager",
                                 "DataMode",
                                 StringValue("DsssRate11Mbps"),
                                 "ControlMode",
                                 StringValue("DsssRate11Mbps"));
    WifiMacHelper wifiMac;
    wifiMac.SetType("ns3::AdhocWifiMac");
    NetDeviceContainer devices = wifi.Install(wifiPhy, wifiMac, allNodes);

    // ---- Mobilitas ----
    // 12 petugas: RandomWaypoint, waypoint = 4 pos + 4 titik tengah antar pos.
    Ptr<WaypointAllocator> waypoints = CreateObject<WaypointAllocator>();
    waypoints->Add(Vector(200, 200, 0));
    waypoints->Add(Vector(600, 200, 0));
    waypoints->Add(Vector(200, 600, 0));
    waypoints->Add(Vector(600, 600, 0));
    waypoints->Add(Vector(400, 200, 0));
    waypoints->Add(Vector(200, 400, 0));
    waypoints->Add(Vector(600, 400, 0));
    waypoints->Add(Vector(400, 600, 0));

    // Stream RNG mobilitas ditetapkan eksplisit supaya gerak petugas identik di A dan B.
    // Hanya mobileNodes yang di-assign, jadi menambah node statis tidak menggeser stream.
    int64_t streamIndex = 0;
    streamIndex += waypoints->AssignStreams(streamIndex);

    MobilityHelper mobileMob;
    mobileMob.SetMobilityModel("ns3::RandomWaypointMobilityModel",
                               "Speed",
                               StringValue("ns3::UniformRandomVariable[Min=1.0|Max=5.0]"),
                               "Pause",
                               StringValue("ns3::UniformRandomVariable[Min=30.0|Max=120.0]"),
                               "PositionAllocator",
                               PointerValue(waypoints));
    mobileMob.SetPositionAllocator(waypoints);
    mobileMob.Install(mobileNodes);
    streamIndex += mobileMob.AssignStreams(mobileNodes, streamIndex);

    // Node statis di 4 pos. Container kosong di varian A, jadi Install() tidak melakukan apa-apa.
    Ptr<ListPositionAllocator> staticPos = CreateObject<ListPositionAllocator>();
    staticPos->Add(Vector(200, 200, 0));
    staticPos->Add(Vector(600, 200, 0));
    staticPos->Add(Vector(200, 600, 0));
    staticPos->Add(Vector(600, 600, 0));
    MobilityHelper staticMob;
    staticMob.SetPositionAllocator(staticPos);
    staticMob.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    staticMob.Install(staticNodes);

    // ---- AODV + IP ----
    AodvHelper aodv;
    InternetStackHelper internet;
    internet.SetRoutingHelper(aodv);
    internet.Install(allNodes);

    Ipv4AddressHelper address;
    address.SetBase("10.1.1.0", "255.255.255.0");
    Ipv4InterfaceContainer interfaces = address.Assign(devices);

    g_warmupTime = Seconds(warmup);
    g_fwdData.assign(allNodes.GetN(), 0);
    for (uint32_t i = 0; i < allNodes.GetN(); i++)
    {
        allNodes.Get(i)->GetObject<Ipv4L3Protocol>()->TraceConnectWithoutContext(
            "UnicastForward",
            MakeBoundCallback(&OnFwd, i));
    }
    Config::ConnectWithoutContext("/NodeList/*/$ns3::Ipv4L3Protocol/Tx", MakeCallback(&OnIpTx));

    NS_LOG_UNCOND("variant=" << variant << " seed=" << seed << " withStatic=" << withStatic
                             << " totalNodes=" << allNodes.GetN() << " range=" << range);

    // ---- Trafik: 6 aliran UDP petugas ke petugas, 128 byte tiap 2 s ----
    // Pasangan tetap dan identik di A dan B: node i mengirim ke node i+6 (i = 0..5).
    // Pengiriman berhenti 5 s sebelum simulasi usai (ASUMSI) supaya paket terakhir sempat tiba
    // dan PDR tidak terpotong oleh akhir simulasi.
    const uint16_t port = 9;
    const uint32_t nFlows = 6;
    const Time interval = Seconds(2.0);
    const Time stopSend = Seconds(simTime - 5.0);

    // Jitter waktu mulai 0..1 s per aliran, stream eksplisit agar sama di A dan B.
    Ptr<UniformRandomVariable> startJitter = CreateObject<UniformRandomVariable>();
    startJitter->SetStream(streamIndex++);

    TypeId udp = TypeId::LookupByName("ns3::UdpSocketFactory");
    for (uint32_t i = 0; i < nFlows; i++)
    {
        uint32_t src = i;
        uint32_t dst = i + nFlows;

        Ptr<Socket> sink = Socket::CreateSocket(mobileNodes.Get(dst), udp);
        sink->Bind(InetSocketAddress(Ipv4Address::GetAny(), port));
        sink->SetRecvCallback(MakeCallback(&OnRecv));

        Ptr<Socket> source = Socket::CreateSocket(mobileNodes.Get(src), udp);
        source->Connect(InetSocketAddress(interfaces.GetAddress(dst), port));

        Time start = Seconds(warmup + startJitter->GetValue(0.0, 1.0));
        Simulator::Schedule(start, &SendOne, source, 0, interval, stopSend);
    }

    Simulator::Schedule(Seconds(warmup), &CheckConn, allNodes, range, nFlows, stopSend);

    if (debugPos)
    {
        for (double t : {100.0, 300.0, 500.0})
        {
            Simulator::Schedule(Seconds(t), &PrintPos, mobileNodes.Get(0));
            Simulator::Schedule(Seconds(t), &PrintPos, mobileNodes.Get(1));
        }
    }

    Simulator::Stop(Seconds(simTime));
    Simulator::Run();

    double pdr = g_stats.sent > 0 ? 100.0 * g_stats.rcvd / g_stats.sent : 0.0;
    double meanDelay = g_stats.rcvd > 0 ? g_stats.delaySum / g_stats.rcvd : 0.0;
    double overhead = g_stats.rcvdBytes > 0 ? double(g_ctrlBytes) / g_stats.rcvdBytes : 0.0;

    uint64_t fwdMobile = 0;
    uint64_t fwdStatic = 0;
    for (uint32_t i = 0; i < allNodes.GetN(); i++)
    {
        (i < nMobile ? fwdMobile : fwdStatic) += g_fwdData[i];
        if (i >= nMobile)
        {
            NS_LOG_UNCOND("  node statis " << i << " meneruskan " << g_fwdData[i] << " paket data");
        }
    }
    double connAllPct = g_connSamples ? 100.0 * g_connAllOk / g_connSamples : 0.0;
    double connPairPct = g_pairSamples ? 100.0 * g_pairOk / g_pairSamples : 0.0;

    NS_LOG_UNCOND("sent=" << g_stats.sent << " rcvd=" << g_stats.rcvd << " PDR=" << pdr
                          << "% meanDelay=" << meanDelay << " s ctrlPkts=" << g_ctrlPkts
                          << " ctrlBytes=" << g_ctrlBytes << " overhead=" << overhead
                          << " helloBytes=" << g_helloBytes << " fwdStatic=" << fwdStatic
                          << " fwdMobile=" << fwdMobile << " connPair=" << connPairPct
                          << "% connAll=" << connAllPct << "%");

    // ---- CSV per run: satu header, satu baris. Parameter ikut dicatat untuk reproduksibilitas ----
    std::filesystem::create_directories(outDir);
    std::string seedStr = (seed < 10 ? "0" : "") + std::to_string(seed);
    std::string path = outDir + "/result_" + variant + "_seed" + seedStr + ".csv";
    std::ofstream out(path);
    NS_ABORT_MSG_UNLESS(out.is_open(), "Gagal membuka " << path);
    out << "variant,seed,nMobile,nStatic,range_m,simTime_s,warmup_s,"
           "sent,rcvd,pdr_pct,mean_delay_s,ctrl_pkts,ctrl_bytes,rcvd_bytes,overhead,"
           "hello_pkts,hello_bytes,fwd_static_data,fwd_mobile_data,conn_pair_pct,conn_all_pct\n";
    out << variant << "," << seed << "," << nMobile << "," << (withStatic ? nStatic : 0) << ","
        << range << "," << simTime << "," << warmup << "," << g_stats.sent << ","
        << g_stats.rcvd << "," << pdr << "," << meanDelay << "," << g_ctrlPkts << ","
        << g_ctrlBytes << "," << g_stats.rcvdBytes << "," << overhead << "," << g_helloPkts << ","
        << g_helloBytes << "," << fwdStatic << "," << fwdMobile << "," << connPairPct << ","
        << connAllPct << "\n";
    out.close();
    NS_LOG_UNCOND("CSV: " << path);

    Simulator::Destroy();
    return 0;
}
