// peer.cpp
// Peer program for DefinitelyNotLimewire
// PLACEHOLDER: for now this only reads the two config files, finds this
// machine's IP and prints the settings.
// The real peer (talking to the tracker, downloading chunks from other peers with threads,
// serving chunks) will be written here.

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "common.h"

namespace {

// Used when clientThreadConfig.cfg has no update-interval line
const int defaultUpdateIntervalSeconds = 900;  // 15 minutes

// Everything the peer reads from its two config files
struct PeerConfig {
    int trackerPort = 0;
    std::string trackerIp;
    int updateIntervalSeconds = defaultUpdateIntervalSeconds;
    int listenPort = 0;
    std::string sharedFolder;
};

// Resolves host (dotted IPv4 address or host name) to an IPv4 address.
bool resolveIPv4(const std::string& host, in_addr& address) {
    if (inet_pton(AF_INET, host.c_str(), &address) == 1) {
        return true;
    }
    addrinfo hints{};
    hints.ai_family = AF_INET;
    addrinfo* result = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || result == nullptr) {
        return false;
    }
    address = reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr;
    freeaddrinfo(result);
    return true;
}

// clientThreadConfig.cfg:
//   line 1    = tracker port
//   line 2    = tracker IP
//   last line = updatetracker interval in seconds (optional when the
//               file has only two lines; defaults to 900)
bool readClientConfig(PeerConfig& config) {
    const char path[] = "clientThreadConfig.cfg";
    const char usage[] =
        "  line 1:    tracker port (1-65535), e.g. 9000\n"
        "  line 2:    tracker IP address, e.g. 127.0.0.1\n"
        "  last line: updatetracker interval in seconds (optional, default 900)\n";

    std::vector<std::string> lines;
    if (!loadConfigFile("Peer", path, 2, usage, lines)) {
        return false;
    }

    if (!parsePort(lines[0], config.trackerPort)) {
        std::cerr << "Peer: " << path << " line 1: invalid tracker port '" << lines[0]
                  << "' (must be a number from 1 to 65535)\n";
        return false;
    }

    config.trackerIp = lines[1];
    in_addr unused{};
    if (!resolveIPv4(config.trackerIp, unused)) {
        std::cerr << "Peer: " << path << " line 2: invalid tracker IP '" << lines[1]
                  << "' (must be an IPv4 address such as 127.0.0.1 or a resolvable host name)\n";
        return false;
    }

    if (lines.size() >= 3) {
        if (!parseIntInRange(lines.back(), 1, 86400, config.updateIntervalSeconds)) {
            std::cerr << "Peer: " << path << " last line: invalid update interval '"
                      << lines.back() << "' (must be whole seconds from 1 to 86400)\n";
            return false;
        }
    } else {
        config.updateIntervalSeconds = defaultUpdateIntervalSeconds;
    }
    return true;
}

// serverThreadConfig.cfg:
//   line 1    = port this peer listens on for other peers
//   last line = shared folder name
bool readServerConfig(PeerConfig& config) {
    const char path[] = "serverThreadConfig.cfg";
    const char usage[] =
        "  line 1:    this peer's listen port (1-65535), e.g. 9001\n"
        "  last line: shared folder name, e.g. shared\n";

    std::vector<std::string> lines;
    if (!loadConfigFile("Peer", path, 2, usage, lines)) {
        return false;
    }

    if (!parsePort(lines[0], config.listenPort)) {
        std::cerr << "Peer: " << path << " line 1: invalid listen port '" << lines[0]
                  << "' (must be a number from 1 to 65535)\n";
        return false;
    }

    config.sharedFolder = lines.back();
    return true;
}

// Makes sure the shared folder exists. Returns false (error printed) if
// it can't be created or is not a directory.
bool prepareSharedFolder(const std::string& folder) {
    if (mkdir(folder.c_str(), 0755) != 0 && errno != EEXIST) {
        std::cerr << "Peer: could not create shared folder '" << folder
                  << "' (serverThreadConfig.cfg last line): " << std::strerror(errno) << "\n";
        return false;
    }
    struct stat info{};
    if (stat(folder.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) {
        std::cerr << "Peer: shared folder '" << folder
                  << "' (serverThreadConfig.cfg last line) is not a directory\n";
        return false;
    }
    return true;
}

// First non-loopback IPv4 address of this machine, or "" if none.
std::string firstInterfaceIPv4() {
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0) {
        return "";
    }
    std::string ip;
    for (ifaddrs* entry = interfaces; entry != nullptr; entry = entry->ifa_next) {
        if (entry->ifa_addr == nullptr || entry->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        if ((entry->ifa_flags & IFF_UP) == 0 || (entry->ifa_flags & IFF_LOOPBACK) != 0) {
            continue;
        }
        char text[INET_ADDRSTRLEN];
        const in_addr& address = reinterpret_cast<sockaddr_in*>(entry->ifa_addr)->sin_addr;
        if (inet_ntop(AF_INET, &address, text, sizeof(text)) != nullptr) {
            ip = text;
            break;
        }
    }
    freeifaddrs(interfaces);
    return ip;
}

// Finds the IP address of this machine that the tracker (and other
// peers) can reach us on. "Connecting" a UDP socket sends nothing; it
// only asks the OS which local address it would route to the tracker
// from. That makes a local tracker (127.0.0.1) give 127.0.0.1 and a
// remote tracker give the LAN address. Falls back to the first
// non-loopback interface, then to 127.0.0.1.
std::string getOwnIPAddress(const std::string& trackerHost, int trackerPort) {
    sockaddr_in tracker{};
    tracker.sin_family = AF_INET;
    tracker.sin_port = htons(static_cast<uint16_t>(trackerPort));
    if (resolveIPv4(trackerHost, tracker.sin_addr)) {
        int probe = socket(AF_INET, SOCK_DGRAM, 0);
        if (probe >= 0) {
            sockaddr_in local{};
            socklen_t length = sizeof(local);
            char text[INET_ADDRSTRLEN];
            bool found =
                connect(probe, reinterpret_cast<sockaddr*>(&tracker), sizeof(tracker)) == 0 &&
                getsockname(probe, reinterpret_cast<sockaddr*>(&local), &length) == 0 &&
                local.sin_addr.s_addr != htonl(INADDR_ANY) &&
                inet_ntop(AF_INET, &local.sin_addr, text, sizeof(text)) != nullptr;
            close(probe);
            if (found) {
                return text;
            }
        }
    }

    std::string ip = firstInterfaceIPv4();
    return ip.empty() ? "127.0.0.1" : ip;
}

}  // namespace

// Entry point for the Peer program.
int main() {
    PeerConfig config;
    if (!readClientConfig(config) || !readServerConfig(config)) {
        return 1;
    }
    if (!prepareSharedFolder(config.sharedFolder)) {
        return 1;
    }

    std::string ownIp = getOwnIPAddress(config.trackerIp, config.trackerPort);

    std::cout << "Peer: tracker at " << config.trackerIp << ":" << config.trackerPort
              << ", update every " << config.updateIntervalSeconds << "s\n";
    std::cout << "Peer: this peer is " << ownIp << ":" << config.listenPort
              << ", shared folder " << config.sharedFolder << "\n";
    std::cout << "Peer: not implemented yet\n";
    return 0;
}
