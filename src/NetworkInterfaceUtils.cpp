// Derived from PGStream by Andrulius (AGPL-3.0-only). See NOTICE.md.
#include "NetworkInterfaceUtils.h"

#include <algorithm>
#include <vector>
#if !JUCE_WINDOWS
 #include <ifaddrs.h>
 #include <net/if.h>
 #include <arpa/inet.h>
 #include <sys/socket.h>
 #include <unistd.h>
#endif

#if JUCE_WINDOWS
 #include <winsock2.h>
 #include <ws2tcpip.h>
 #include <iphlpapi.h>
 #include <ifdef.h>
#endif

namespace mconnect
{
namespace
{
struct Candidate
{
    juce::String address;
    juce::String adapterName;
    bool isUp = false;
    bool hasGateway = false;
    bool isWireless = false;
    bool isEthernet = false;
    bool isPrivate = false;
    bool isLinkLocal = false;
    bool isLoopback = false;
    bool isVirtualAdapter = false;
    uint32_t metric = 0;
    int rank = 1000;
};

bool parseIpv4(const juce::String& address, int octets[4])
{
    const auto parts = juce::StringArray::fromTokens(address, ".", {});
    if (parts.size() != 4)
        return false;

    for (int i = 0; i < 4; ++i)
    {
        const auto value = parts[i].getIntValue();
        if (value < 0 || value > 255 || parts[i].trim().isEmpty())
            return false;

        octets[i] = value;
    }

    return true;
}

bool isLoopbackIpv4(const juce::String& address)
{
    int octets[4] {};
    return parseIpv4(address, octets) && octets[0] == 127;
}

bool isLinkLocalIpv4(const juce::String& address)
{
    int octets[4] {};
    return parseIpv4(address, octets) && octets[0] == 169 && octets[1] == 254;
}

bool isPrivateIpv4(const juce::String& address)
{
    int octets[4] {};
    if (! parseIpv4(address, octets))
        return false;

    return octets[0] == 10
        || (octets[0] == 172 && octets[1] >= 16 && octets[1] <= 31)
        || (octets[0] == 192 && octets[1] == 168);
}

bool looksLikeVirtualAdapter(const juce::String& name)
{
    static constexpr const char* markers[] = {
        "virtualbox",
        "vmware",
        "hyper-v",
        "vethernet",
        "docker",
        "wsl",
        "loopback",
        "tap",
        "tun",
        "vpn",
        "host-only"
    };

    for (const auto* marker : markers)
    {
        if (name.containsIgnoreCase(marker))
            return true;
    }

    return false;
}

int rankCandidate(const Candidate& candidate)
{
    if (candidate.isLoopback)
        return 900;

    if (candidate.isLinkLocal)
        return 800;

    if (candidate.isVirtualAdapter)
        return candidate.isPrivate ? 140 : 160;

    if (candidate.isPrivate)
    {
        if (candidate.isUp && candidate.isWireless && candidate.hasGateway)
            return 10;

        if (candidate.isUp && candidate.isEthernet && candidate.hasGateway)
            return 20;

        if (candidate.isUp && candidate.hasGateway)
            return 30;

        if (candidate.isUp)
            return 40;

        return 50;
    }

    if (candidate.isUp && candidate.hasGateway)
        return 60;

    if (candidate.isUp)
        return 70;

    return 100;
}

juce::String makeUrl(const juce::String& address, int port, bool useHttps)
{
    return (useHttps ? "https://" : "http://") + address + ":" + juce::String(port) + "/";
}

juce::String describeCandidate(const Candidate& candidate)
{
    auto flags = juce::StringArray();

    flags.add(candidate.isUp ? "up" : "down");

    if (candidate.isWireless)
        flags.add("Wi-Fi");
    else if (candidate.isEthernet)
        flags.add("Ethernet");
    else
        flags.add("adapter");

    if (candidate.hasGateway)
        flags.add("gateway");

    if (candidate.isVirtualAdapter)
        flags.add("virtual/downranked");

    if (candidate.isPrivate)
        flags.add("private");
    else if (candidate.isLinkLocal)
        flags.add("link-local");

    const auto name = candidate.adapterName.isNotEmpty() ? candidate.adapterName : "unnamed adapter";
    return candidate.address + " - " + name + " (" + flags.joinIntoString(", ") + ")";
}

void classify(Candidate& candidate)
{
    candidate.isLoopback = isLoopbackIpv4(candidate.address);
    candidate.isLinkLocal = isLinkLocalIpv4(candidate.address);
    candidate.isPrivate = isPrivateIpv4(candidate.address);
    candidate.isVirtualAdapter = looksLikeVirtualAdapter(candidate.adapterName);
    candidate.rank = rankCandidate(candidate);
}

#if JUCE_WINDOWS
bool hasIpv4Gateway(const IP_ADAPTER_ADDRESSES* adapter)
{
    for (auto* gateway = adapter != nullptr ? adapter->FirstGatewayAddress : nullptr;
         gateway != nullptr;
         gateway = gateway->Next)
    {
        const auto* socketAddress = gateway->Address.lpSockaddr;
        if (socketAddress == nullptr || socketAddress->sa_family != AF_INET)
            continue;

        const auto* ipv4 = reinterpret_cast<const sockaddr_in*> (socketAddress);
        if (ipv4->sin_addr.s_addr != 0)
            return true;
    }

    return false;
}

std::vector<Candidate> enumerateWindowsCandidates()
{
    std::vector<Candidate> result;
    ULONG bufferSize = 0;

    auto flags = static_cast<ULONG> (GAA_FLAG_SKIP_ANYCAST
                                     | GAA_FLAG_SKIP_MULTICAST
                                     | GAA_FLAG_SKIP_DNS_SERVER);

    auto status = GetAdaptersAddresses(AF_INET, flags, nullptr, nullptr, &bufferSize);
    if (status != ERROR_BUFFER_OVERFLOW || bufferSize == 0)
        return result;

    std::vector<unsigned char> buffer(bufferSize);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*> (buffer.data());
    status = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &bufferSize);
    if (status != NO_ERROR)
        return result;

    for (auto* adapter = adapters; adapter != nullptr; adapter = adapter->Next)
    {
        const auto isUp = adapter->OperStatus == IfOperStatusUp;
        const auto hasGateway = hasIpv4Gateway(adapter);
        const auto isWireless = adapter->IfType == IF_TYPE_IEEE80211;
        const auto isEthernet = adapter->IfType == IF_TYPE_ETHERNET_CSMACD;
        const auto metric = adapter->Ipv4Metric;

        juce::String adapterName;
        auto adapterNames = juce::StringArray();
        if (adapter->FriendlyName != nullptr)
            adapterNames.add(juce::String(adapter->FriendlyName));
        if (adapter->Description != nullptr)
            adapterNames.add(juce::String(adapter->Description));
        if (adapter->AdapterName != nullptr)
            adapterNames.add(juce::String(adapter->AdapterName));

        adapterName = adapterNames.joinIntoString(" / ");

        for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr; unicast = unicast->Next)
        {
            const auto* socketAddress = unicast->Address.lpSockaddr;
            if (socketAddress == nullptr || socketAddress->sa_family != AF_INET)
                continue;

            if (unicast->DadState == IpDadStateInvalid || unicast->DadState == IpDadStateDuplicate)
                continue;

            const auto* ipv4 = reinterpret_cast<const sockaddr_in*> (socketAddress);
            char text[INET_ADDRSTRLEN] {};
            if (inet_ntop(AF_INET, const_cast<IN_ADDR*> (&ipv4->sin_addr), text, sizeof(text)) == nullptr)
                continue;

            Candidate candidate;
            candidate.address = text;
            candidate.adapterName = adapterName;
            candidate.isUp = isUp;
            candidate.hasGateway = hasGateway;
            candidate.isWireless = isWireless;
            candidate.isEthernet = isEthernet;
            candidate.metric = metric;
            classify(candidate);

            if (! candidate.address.startsWith("0."))
                result.push_back(candidate);
        }
    }

    return result;
}
#endif

std::vector<Candidate> enumerateFallbackCandidates()
{
    std::vector<Candidate> result;

#if !JUCE_WINDOWS
    // UDP connect selects a route without sending packets or contacting a server.
    juce::String routeAddress;
    if (const int fd = socket(AF_INET, SOCK_DGRAM, 0); fd >= 0)
    {
        sockaddr_in destination {}; destination.sin_family = AF_INET; destination.sin_port = htons(9);
        inet_pton(AF_INET, "192.0.2.1", &destination.sin_addr);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&destination), sizeof(destination)) == 0)
        {
            sockaddr_in local {}; socklen_t length = sizeof(local);
            if (getsockname(fd, reinterpret_cast<sockaddr*>(&local), &length) == 0)
            {
                char value[INET_ADDRSTRLEN] {};
                if (inet_ntop(AF_INET, &local.sin_addr, value, sizeof(value))) routeAddress = value;
            }
        }
        ::close(fd);
    }
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) == 0)
    {
        for (auto* item = interfaces; item; item = item->ifa_next)
        {
            if (!item->ifa_addr || item->ifa_addr->sa_family != AF_INET || !(item->ifa_flags & IFF_UP)) continue;
            char value[INET_ADDRSTRLEN] {};
            auto* ipv4 = reinterpret_cast<sockaddr_in*>(item->ifa_addr);
            if (!inet_ntop(AF_INET, &ipv4->sin_addr, value, sizeof(value))) continue;
            Candidate candidate;
            candidate.address = value; candidate.adapterName = item->ifa_name;
            candidate.isUp = true; candidate.hasGateway = candidate.address == routeAddress;
            classify(candidate);
            result.push_back(candidate);
        }
        freeifaddrs(interfaces);
    }
    if (!result.empty()) return result;
#endif

    for (const auto& address : juce::IPAddress::getAllAddresses(false))
    {
        Candidate candidate;
        candidate.address = address.toString();
        candidate.adapterName = "JUCE network address";
        candidate.isUp = true;
        candidate.hasGateway = false;
        candidate.isWireless = false;
        candidate.isEthernet = false;
        candidate.metric = 0;
        classify(candidate);

        if (! candidate.address.isEmpty() && ! candidate.address.startsWith("0."))
            result.push_back(candidate);
    }

    return result;
}

std::vector<Candidate> enumerateCandidates()
{
#if JUCE_WINDOWS
    auto candidates = enumerateWindowsCandidates();
    if (! candidates.empty())
        return candidates;
#endif

    return enumerateFallbackCandidates();
}
}

NetworkInterfaceSelection selectBestLanInterface(int port, bool useHttps)
{
    auto candidates = enumerateCandidates();
    std::sort(candidates.begin(), candidates.end(), [] (const Candidate& a, const Candidate& b)
    {
        if (a.rank != b.rank)
            return a.rank < b.rank;

        if (a.metric != b.metric)
            return a.metric < b.metric;

        return a.address < b.address;
    });

    const auto hasNonLinkLocalNonLoopback = std::any_of(candidates.begin(), candidates.end(), [] (const Candidate& candidate)
    {
        return ! candidate.isLoopback && ! candidate.isLinkLocal;
    });

    NetworkInterfaceSelection selection;

    for (const auto& candidate : candidates)
    {
        if (candidate.isLoopback)
            continue;

        if (candidate.isLinkLocal && hasNonLinkLocalNonLoopback)
            continue;

        const auto url = makeUrl(candidate.address, port, useHttps);
        selection.candidateUrls.addIfNotAlreadyThere(url);
        selection.candidateDescriptions.addIfNotAlreadyThere(describeCandidate(candidate));

        if (selection.primaryAddress.isEmpty())
        {
            selection.primaryAddress = candidate.address;
            selection.primaryUrl = url;
        }
    }

    if (selection.primaryAddress.isEmpty())
    {
        selection.primaryAddress = "127.0.0.1";
        selection.primaryUrl = makeUrl(selection.primaryAddress, port, useHttps);
        selection.candidateUrls.addIfNotAlreadyThere(selection.primaryUrl);
        selection.candidateDescriptions.addIfNotAlreadyThere("127.0.0.1 - loopback fallback");
    }

    return selection;
}
}
