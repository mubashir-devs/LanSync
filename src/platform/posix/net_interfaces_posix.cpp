// getifaddrs-based interface enumeration (Linux, macOS).
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

#include <memory>

#include "platform/platform.hpp"

namespace lansync::platform {

std::vector<NetInterface> networkInterfaces() {
    std::vector<NetInterface> out;
    ifaddrs* raw = nullptr;
    if (getifaddrs(&raw) != 0) return out;
    std::unique_ptr<ifaddrs, decltype(&freeifaddrs)> list(raw, &freeifaddrs);

    for (const ifaddrs* ifa = list.get(); ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if (!(ifa->ifa_flags & IFF_UP)) continue;

        NetInterface ni;
        ni.name = ifa->ifa_name;
        char buf[INET_ADDRSTRLEN] = {};
        const auto* sin = reinterpret_cast<const sockaddr_in*>(ifa->ifa_addr);
        inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf));
        ni.ipv4 = buf;
        ni.loopback = (ifa->ifa_flags & IFF_LOOPBACK) != 0;
        ni.multicast = (ifa->ifa_flags & IFF_MULTICAST) != 0;
        if ((ifa->ifa_flags & IFF_BROADCAST) && ifa->ifa_broadaddr) {
            const auto* b = reinterpret_cast<const sockaddr_in*>(ifa->ifa_broadaddr);
            char bb[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &b->sin_addr, bb, sizeof(bb));
            ni.broadcast = bb;
        }
        out.push_back(std::move(ni));
    }
    return out;
}

}  // namespace lansync::platform
