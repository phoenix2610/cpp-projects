// Decode network traffic byte by byte: Ethernet, IP, TCP/UDP/ICMP, plus per-flow stats.
//
//   sudo ./sniff --iface any --count 50        # live capture needs CAP_NET_RAW
//   ./sniff --pcap capture.pcap                # or read a pcap file, no privileges
//   ./sniff --demo                             # synthesises a capture, then decodes it
//
// A packet is a stack of headers, each one telling you what the next one is:
// Ethernet's ethertype says "IPv4 follows", IPv4's protocol byte says "TCP follows",
// and the IHL field says how far to jump to find it. Everything hard is in the
// details — IHL is in 4-byte words, TCP's data offset is in the high nibble, and
// the checksum is a one's-complement sum that has to fold its own carries back in.

#include <arpa/inet.h>
#include <errno.h>
#include <net/ethernet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>
#include <netinet/ip_icmp.h>
#include <cstring>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#ifdef __linux__
#include <linux/if_packet.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#endif

struct PcapHeader {
    std::uint32_t magic = 0xa1b2c3d4, version = 0x00040002, timezone = 0, sigfigs = 0;
    std::uint32_t snaplen = 65535, link_type = 1;      // 1 = Ethernet
};
struct PcapRecord { std::uint32_t seconds, microseconds, captured, original; };

struct FlowKey {
    std::string source, destination;
    std::uint16_t source_port, destination_port;
    std::uint8_t protocol;
    bool operator<(const FlowKey& other) const {
        return std::tie(source, source_port, destination, destination_port, protocol) <
               std::tie(other.source, other.source_port, other.destination, other.destination_port, other.protocol);
    }
};
struct FlowStats { std::uint64_t packets = 0, bytes = 0; std::string flags; };

static std::string ip_to_string(std::uint32_t address) {
    char buffer[INET_ADDRSTRLEN];
    ::inet_ntop(AF_INET, &address, buffer, sizeof(buffer));
    return buffer;
}

// One's-complement checksum: add 16-bit words, fold the carries back in, invert.
static std::uint16_t checksum(const void* data, std::size_t length) {
    const auto* words = static_cast<const std::uint16_t*>(data);
    std::uint32_t sum = 0;
    while (length > 1) { sum += *words++; length -= 2; }
    if (length) sum += *reinterpret_cast<const std::uint8_t*>(words);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return std::uint16_t(~sum);
}

static std::string tcp_flag_string(const tcphdr& header) {
    std::string flags;
    if (header.th_flags & TH_SYN) flags += "S";
    if (header.th_flags & TH_ACK) flags += "A";
    if (header.th_flags & TH_PUSH) flags += "P";
    if (header.th_flags & TH_FIN) flags += "F";
    if (header.th_flags & TH_RST) flags += "R";
    if (header.th_flags & TH_URG) flags += "U";
    return flags.empty() ? "-" : flags;
}

static const char* service_name(std::uint16_t port) {
    switch (port) {
        case 22: return "ssh"; case 53: return "dns"; case 80: return "http"; case 443: return "https";
        case 5432: return "postgres"; case 6379: return "redis"; case 3306: return "mysql";
        case 123: return "ntp"; case 25: return "smtp"; case 8080: return "http-alt";
        default: return "";
    }
}

struct Decoder {
    std::map<FlowKey, FlowStats> flows;
    std::uint64_t packets = 0, bytes = 0, bad_checksums = 0, non_ip = 0;
    std::map<std::string, std::uint64_t> protocols;

    std::string decode(const std::uint8_t* data, std::size_t length, bool verbose) {
        ++packets;
        bytes += length;
        if (length < sizeof(ether_header)) return "runt frame";

        const auto* ethernet = reinterpret_cast<const ether_header*>(data);
        std::uint16_t ethertype = ntohs(ethernet->ether_type);
        if (ethertype != ETHERTYPE_IP) {
            ++non_ip;
            protocols[ethertype == ETHERTYPE_ARP ? "ARP" : ethertype == ETHERTYPE_IPV6 ? "IPv6" : "other"]++;
            return ethertype == ETHERTYPE_ARP ? "ARP" : ethertype == ETHERTYPE_IPV6 ? "IPv6 (not decoded)" : "unknown ethertype";
        }

        const auto* ip = reinterpret_cast<const struct ip*>(data + sizeof(ether_header));
        std::size_t ip_header_length = std::size_t(ip->ip_hl) * 4;      // IHL counts 32-bit words
        if (ip_header_length < 20 || sizeof(ether_header) + ip_header_length > length) return "truncated IP header";
        if (checksum(ip, ip_header_length) != 0) ++bad_checksums;

        std::string source = ip_to_string(ip->ip_src.s_addr);
        std::string destination = ip_to_string(ip->ip_dst.s_addr);
        const std::uint8_t* payload = data + sizeof(ether_header) + ip_header_length;
        std::size_t payload_length = length - sizeof(ether_header) - ip_header_length;

        char line[256];
        FlowKey key{source, destination, 0, 0, ip->ip_p};

        if (ip->ip_p == IPPROTO_TCP && payload_length >= sizeof(tcphdr)) {
            const auto* tcp = reinterpret_cast<const tcphdr*>(payload);
            key.source_port = ntohs(tcp->th_sport);
            key.destination_port = ntohs(tcp->th_dport);
            std::size_t tcp_header_length = std::size_t(tcp->th_off) * 4;
            std::size_t data_bytes = payload_length > tcp_header_length ? payload_length - tcp_header_length : 0;
            std::string flags = tcp_flag_string(*tcp);
            protocols["TCP"]++;
            flows[key].packets++;
            flows[key].bytes += length;
            if (flows[key].flags.find(flags) == std::string::npos) flows[key].flags += flags + " ";
            std::snprintf(line, sizeof(line), "TCP  %s:%u -> %s:%-5u %-4s seq=%u win=%u %zu bytes%s%s",
                          source.c_str(), key.source_port, destination.c_str(), key.destination_port,
                          flags.c_str(), ntohl(tcp->th_seq), ntohs(tcp->th_win), data_bytes,
                          *service_name(key.destination_port) ? "  " : "", service_name(key.destination_port));
        } else if (ip->ip_p == IPPROTO_UDP && payload_length >= sizeof(udphdr)) {
            const auto* udp = reinterpret_cast<const udphdr*>(payload);
            key.source_port = ntohs(udp->uh_sport);
            key.destination_port = ntohs(udp->uh_dport);
            protocols["UDP"]++;
            flows[key].packets++;
            flows[key].bytes += length;
            std::snprintf(line, sizeof(line), "UDP  %s:%u -> %s:%-5u %u bytes%s%s",
                          source.c_str(), key.source_port, destination.c_str(), key.destination_port,
                          ntohs(udp->uh_ulen) - 8, *service_name(key.destination_port) ? "  " : "",
                          service_name(key.destination_port));
        } else if (ip->ip_p == IPPROTO_ICMP && payload_length >= sizeof(icmphdr)) {
            const auto* icmp = reinterpret_cast<const icmphdr*>(payload);
            protocols["ICMP"]++;
            flows[key].packets++;
            flows[key].bytes += length;
            std::snprintf(line, sizeof(line), "ICMP %s -> %s  type=%u (%s) id=%u seq=%u",
                          source.c_str(), destination.c_str(), icmp->type,
                          icmp->type == ICMP_ECHO ? "echo request" : icmp->type == ICMP_ECHOREPLY ? "echo reply" : "other",
                          ntohs(icmp->un.echo.id), ntohs(icmp->un.echo.sequence));
        } else {
            protocols["other IP"]++;
            std::snprintf(line, sizeof(line), "IP   %s -> %s  protocol %u", source.c_str(), destination.c_str(), ip->ip_p);
        }

        if (verbose) std::printf("       ttl=%u id=%u len=%u\n", ip->ip_ttl, ntohs(ip->ip_id), ntohs(ip->ip_len));
        return line;
    }

    void report() const {
        std::printf("\n%llu packets, %llu bytes, %llu with a bad IP checksum, %llu non-IP frames\n",
                    (unsigned long long)packets, (unsigned long long)bytes,
                    (unsigned long long)bad_checksums, (unsigned long long)non_ip);
        std::printf("protocols: ");
        for (const auto& [name, count] : protocols) std::printf("%s %llu  ", name.c_str(), (unsigned long long)count);
        std::puts("");

        std::vector<std::pair<std::uint64_t, std::string>> ranked;
        for (const auto& [key, stats] : flows) {
            char label[160];
            std::snprintf(label, sizeof(label), "%s:%u -> %s:%u [%s]", key.source.c_str(), key.source_port,
                          key.destination.c_str(), key.destination_port,
                          key.protocol == IPPROTO_TCP ? "TCP" : key.protocol == IPPROTO_UDP ? "UDP" : "ICMP");
            ranked.emplace_back(stats.bytes, std::string(label) + "  " + std::to_string(stats.packets) +
                                             " packets  " + stats.flags);
        }
        std::sort(ranked.rbegin(), ranked.rend());
        std::printf("\ntop flows by bytes (%zu total):\n", flows.size());
        for (std::size_t i = 0; i < std::min<std::size_t>(8, ranked.size()); ++i)
            std::printf("  %7llu B  %s\n", (unsigned long long)ranked[i].first, ranked[i].second.c_str());
    }
};

// ------------------------------------------------------- synthetic capture

static std::vector<std::uint8_t> build_packet(const char* source, const char* destination,
                                              std::uint16_t source_port, std::uint16_t destination_port,
                                              std::uint8_t protocol, std::uint8_t tcp_flags,
                                              std::size_t payload_bytes, std::uint32_t sequence) {
    std::vector<std::uint8_t> packet(sizeof(ether_header) + sizeof(struct ip) + 20 + payload_bytes, 0);
    auto* ethernet = reinterpret_cast<ether_header*>(packet.data());
    for (int i = 0; i < 6; ++i) { ethernet->ether_shost[i] = std::uint8_t(0x02 + i); ethernet->ether_dhost[i] = std::uint8_t(0x0a + i); }
    ethernet->ether_type = htons(ETHERTYPE_IP);

    auto* ip = reinterpret_cast<struct ip*>(packet.data() + sizeof(ether_header));
    ip->ip_v = 4;
    ip->ip_hl = 5;
    ip->ip_len = htons(std::uint16_t(packet.size() - sizeof(ether_header)));
    ip->ip_id = htons(std::uint16_t(sequence & 0xFFFF));
    ip->ip_ttl = 64;
    ip->ip_p = protocol;
    ::inet_pton(AF_INET, source, &ip->ip_src);
    ::inet_pton(AF_INET, destination, &ip->ip_dst);
    ip->ip_sum = 0;
    ip->ip_sum = checksum(ip, sizeof(struct ip));

    std::uint8_t* payload = packet.data() + sizeof(ether_header) + sizeof(struct ip);
    if (protocol == IPPROTO_TCP) {
        auto* tcp = reinterpret_cast<tcphdr*>(payload);
        tcp->th_sport = htons(source_port);
        tcp->th_dport = htons(destination_port);
        tcp->th_seq = htonl(sequence);
        tcp->th_off = 5;
        tcp->th_flags = tcp_flags;
        tcp->th_win = htons(64240);
    } else if (protocol == IPPROTO_UDP) {
        auto* udp = reinterpret_cast<udphdr*>(payload);
        udp->uh_sport = htons(source_port);
        udp->uh_dport = htons(destination_port);
        udp->uh_ulen = htons(std::uint16_t(8 + payload_bytes));
    } else {
        auto* icmp = reinterpret_cast<icmphdr*>(payload);
        icmp->type = std::uint8_t(sequence % 2 ? ICMP_ECHOREPLY : ICMP_ECHO);
        icmp->un.echo.id = htons(4242);
        icmp->un.echo.sequence = htons(std::uint16_t(sequence));
    }
    return packet;
}

static void write_pcap(const std::string& path, const std::vector<std::vector<std::uint8_t>>& packets) {
    std::ofstream file(path, std::ios::binary);
    PcapHeader header;
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    std::uint32_t seconds = 1787000000;
    for (const auto& packet : packets) {
        PcapRecord record{seconds, std::uint32_t((&packet - &packets[0]) * 1200),
                          std::uint32_t(packet.size()), std::uint32_t(packet.size())};
        file.write(reinterpret_cast<const char*>(&record), sizeof(record));
        file.write(reinterpret_cast<const char*>(packet.data()), std::streamsize(packet.size()));
    }
}

static int read_pcap(const std::string& path, Decoder& decoder, int limit, bool verbose) {
    std::ifstream file(path, std::ios::binary);
    if (!file) { std::printf("cannot open %s\n", path.c_str()); return 1; }
    PcapHeader header;
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (header.magic != 0xa1b2c3d4) { std::printf("not a pcap file (magic 0x%x)\n", header.magic); return 1; }
    std::vector<std::uint8_t> buffer(65536);
    int shown = 0;
    PcapRecord record{};
    while (file.read(reinterpret_cast<char*>(&record), sizeof(record))) {
        if (record.captured > buffer.size()) break;
        if (!file.read(reinterpret_cast<char*>(buffer.data()), record.captured)) break;
        std::string line = decoder.decode(buffer.data(), record.captured, verbose);
        if (shown < limit) { std::printf("  %s\n", line.c_str()); ++shown; }
    }
    return 0;
}

int main(int argc, char** argv) {
    std::string pcap_path, interface;
    int count = 20;
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--pcap" && i + 1 < argc) pcap_path = argv[++i];
        else if (arg == "--iface" && i + 1 < argc) interface = argv[++i];
        else if (arg == "--count" && i + 1 < argc) count = std::atoi(argv[++i]);
        else if (arg == "-v") verbose = true;
    }

    Decoder decoder;

    if (!interface.empty()) {
#ifdef __linux__
        int fd = ::socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
        if (fd < 0) {
            std::printf("raw socket refused: %s\n", std::strerror(errno));
            std::puts("live capture needs root or CAP_NET_RAW:  sudo setcap cap_net_raw+ep ./sniff");
            std::puts("falling back to --demo so you can still see the decoder work\n");
        } else {
            std::printf("capturing %d packets on %s\n\n", count, interface.c_str());
            std::vector<std::uint8_t> buffer(65536);
            for (int i = 0; i < count; ++i) {
                ssize_t got = ::recv(fd, buffer.data(), buffer.size(), 0);
                if (got <= 0) break;
                std::printf("  %s\n", decoder.decode(buffer.data(), std::size_t(got), verbose).c_str());
            }
            ::close(fd);
            decoder.report();
            return 0;
        }
#endif
    }

    if (pcap_path.empty()) {
        pcap_path = "/tmp/sniff-demo.pcap";
        std::vector<std::vector<std::uint8_t>> packets;
        packets.push_back(build_packet("192.168.1.42", "93.184.216.34", 51234, 443, IPPROTO_TCP, TH_SYN, 0, 1000));
        packets.push_back(build_packet("93.184.216.34", "192.168.1.42", 443, 51234, IPPROTO_TCP, TH_SYN | TH_ACK, 0, 5000));
        packets.push_back(build_packet("192.168.1.42", "93.184.216.34", 51234, 443, IPPROTO_TCP, TH_ACK, 0, 1001));
        for (int i = 0; i < 12; ++i)
            packets.push_back(build_packet("192.168.1.42", "93.184.216.34", 51234, 443, IPPROTO_TCP,
                                           TH_ACK | TH_PUSH, 512, std::uint32_t(1001 + i * 512)));
        for (int i = 0; i < 5; ++i)
            packets.push_back(build_packet("192.168.1.42", "1.1.1.1", std::uint16_t(40000 + i), 53, IPPROTO_UDP, 0, 40, std::uint32_t(i)));
        for (int i = 0; i < 4; ++i)
            packets.push_back(build_packet("192.168.1.42", "8.8.8.8", 0, 0, IPPROTO_ICMP, 0, 56, std::uint32_t(i)));
        packets.push_back(build_packet("192.168.1.42", "93.184.216.34", 51234, 443, IPPROTO_TCP, TH_FIN | TH_ACK, 0, 8000));
        // one deliberately corrupted packet, to prove the checksum check works
        auto corrupt = build_packet("10.0.0.9", "10.0.0.1", 22, 51000, IPPROTO_TCP, TH_ACK, 100, 77);
        corrupt[sizeof(ether_header) + 10] ^= 0xFF;
        packets.push_back(corrupt);
        write_pcap(pcap_path, packets);
        std::printf("synthesised %zu packets -> %s\n\n", packets.size(), pcap_path.c_str());
    }

    std::printf("decoding %s:\n", pcap_path.c_str());
    int result = read_pcap(pcap_path, decoder, count, verbose);
    decoder.report();
    return result;
}
