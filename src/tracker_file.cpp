// tracker_file.cpp
// Code for the TrackerFile class (see tracker_file.h)

#include "tracker_file.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace {

// Strips spaces, tabs and line endings from both ends of text.
std::string trim(const std::string& text) {
    const char* blanks = " \t\r\n";
    std::size_t start = text.find_first_not_of(blanks);
    if (start == std::string::npos) {
        return "";
    }
    std::size_t end = text.find_last_not_of(blanks);
    return text.substr(start, end - start + 1);
}

// Splits text at every occurrence of sep (empty fields are kept).
std::vector<std::string> split(const std::string& text, char sep) {
    std::vector<std::string> parts;
    std::string part;
    for (char c : text) {
        if (c == sep) {
            parts.push_back(part);
            part.clear();
        } else {
            part.push_back(c);
        }
    }
    parts.push_back(part);
    return parts;
}

// Parses text as one plain decimal number with no trailing junk.
// strtoll never throws, so load() can stay exception-free.
bool parseNumber(const std::string& text, long long& out) {
    if (text.empty()) {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    long long value = std::strtoll(text.c_str(), &end, 10);
    if (errno != 0 || end != text.c_str() + text.size()) {
        return false;
    }
    out = value;
    return true;
}

// If line is a "Key: value" header line for key, fills value with the
// trimmed part after the colon and returns true.
bool headerValue(const std::string& line, const std::string& key, std::string& value) {
    if (line.size() <= key.size() || line.compare(0, key.size(), key) != 0 ||
        line[key.size()] != ':') {
        return false;
    }
    value = trim(line.substr(key.size() + 1));
    return true;
}

// Parses one peer line: <ip>:<port>:<start byte>:<end byte>:<time stamp>
bool parsePeerLine(const std::string& line, PeerEntry& entry) {
    std::vector<std::string> parts = split(line, ':');
    if (parts.size() != 5) {
        return false;
    }
    for (std::string& part : parts) {
        part = trim(part);
    }

    long long port = 0, startByte = 0, endByte = 0, timestamp = 0;
    if (!parseNumber(parts[1], port) || !parseNumber(parts[2], startByte) ||
        !parseNumber(parts[3], endByte) || !parseNumber(parts[4], timestamp)) {
        return false;
    }
    if (parts[0].empty() || port < 1 || port > 65535 || startByte < 0 ||
        endByte < 0 || timestamp < 0) {
        return false;
    }

    entry.ip = parts[0];
    entry.port = static_cast<int>(port);
    entry.startByte = startByte;
    entry.endByte = endByte;
    entry.timestamp = static_cast<std::time_t>(timestamp);
    return true;
}

}  // namespace

bool TrackerFile::load(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }

    // Parse into a fresh object and only swap it in when the whole file
    // was valid, so a failed load never leaves half-parsed data behind.
    TrackerFile parsed;
    bool haveFilesize = false;
    std::string line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }

        std::string value;
        if (headerValue(line, "Filename", value)) {
            parsed.filename = value;
        } else if (headerValue(line, "Filesize", value)) {
            if (!parseNumber(value, parsed.filesize) || parsed.filesize < 0) {
                return false;
            }
            haveFilesize = true;
        } else if (headerValue(line, "Description", value)) {
            parsed.description = value;
        } else if (headerValue(line, "MD5", value)) {
            parsed.md5 = value;
        } else {
            PeerEntry entry;
            if (!parsePeerLine(line, entry)) {
                return false;
            }
            parsed.peers.push_back(entry);
        }
    }

    // Filename, Filesize and MD5 must be present (Description is optional)
    if (parsed.filename.empty() || !haveFilesize || parsed.md5.empty()) {
        return false;
    }

    *this = std::move(parsed);
    return true;
}

bool TrackerFile::save(const std::string& path) const {
    // Write to a temp file beside the target and rename it over the real
    // one, so a reader never sees a half-written tracker file.
    std::string tmpPath = path + ".tmp";
    {
        std::ofstream file(tmpPath, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            return false;
        }
        file << toString();
        file.flush();
        if (!file.good()) {
            std::remove(tmpPath.c_str());
            return false;
        }
    }  // the file closes here, before the rename

    if (std::rename(tmpPath.c_str(), path.c_str()) != 0) {
        std::remove(tmpPath.c_str());  // drop the temp file we can't move
        return false;
    }
    return true;
}

void TrackerFile::upsertPeer(const std::string& ip, int port,
                             long long startByte, long long endByte) {
    std::time_t now = std::time(nullptr);

    // Peers are identified by ip and port: refresh the range and time
    // stamp of the matching entry instead of appending a duplicate.
    for (PeerEntry& peer : peers) {
        if (peer.ip == ip && peer.port == port) {
            peer.startByte = startByte;
            peer.endByte = endByte;
            peer.timestamp = now;
            return;
        }
    }
    peers.push_back(PeerEntry{ip, port, startByte, endByte, now});
}

std::size_t TrackerFile::pruneDead(long long maxAgeSeconds) {
    std::time_t now = std::time(nullptr);
    std::size_t before = peers.size();

    // "now - timestamp > maxAge" drops only entries older than the
    // cutoff; a time stamp briefly in the future (clock jitter) survives.
    peers.erase(std::remove_if(peers.begin(), peers.end(),
                               [now, maxAgeSeconds](const PeerEntry& peer) {
                                   long long age = static_cast<long long>(now) -
                                                   static_cast<long long>(peer.timestamp);
                                   return age > maxAgeSeconds;
                               }),
                peers.end());
    return before - peers.size();
}

std::string TrackerFile::toString() const {
    std::ostringstream out;
    out << "Filename: " << filename << "\n";
    out << "Filesize: " << filesize << "\n";
    out << "Description:";
    if (!description.empty()) {
        out << " " << description;
    }
    out << "\n";
    out << "MD5: " << md5 << "\n";
    out << "#list of peers follows next\n";
    for (const PeerEntry& peer : peers) {
        out << peer.ip << ":" << peer.port << ":" << peer.startByte << ":"
            << peer.endByte << ":" << peer.timestamp << "\n";
    }
    return out.str();
}

std::vector<PeerEntry> TrackerFile::peersByNewest() const {
    std::vector<PeerEntry> sorted = peers;
    // stable sort: peers with the same time stamp keep their file order
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const PeerEntry& a, const PeerEntry& b) {
                         return a.timestamp > b.timestamp;
                     });
    return sorted;
}

TrackerFile TrackerFile::createSeed(const std::string& filename, long long filesize,
                                    const std::string& md5, const std::string& description,
                                    const std::string& seedIp, int seedPort) {
    TrackerFile tracker;
    tracker.filename = filename;
    tracker.filesize = filesize;
    tracker.md5 = md5;
    tracker.description = description;

    // The creating peer is the seed: it holds the whole file (0..filesize)
    PeerEntry seed{seedIp, seedPort, 0, filesize, std::time(nullptr)};
    tracker.peers.push_back(seed);
    return tracker;
}
