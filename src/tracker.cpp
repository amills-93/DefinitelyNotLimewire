// tracker.cpp
// Tracker server for DefinitelyNotLimewire
//
// Reads its port and shared folder from sconfig (line 1 = port,
// line 2 = folder), then serves TCP connections with one worker
// thread per connection: REQ LIST, GET, createtracker, updatetracker.
// The handlers are stubs for now; this file provides the socket setup,
// the worker threads, the command dispatch and the mutex the handlers
// will use.

#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "common.h"
#include "tracker_file.h"

namespace {

// How many connections the kernel may queue before accept() takes them
const int listenBacklog = 16;

// Longest request line we accept; a bigger one is answered with an
// error instead of growing our memory forever
const std::size_t maxRequestBytes = 4096;

// The reply for commands the tracker does not know. The doc does not
// define one, so this is our choice.
const char unknownCommandReply[] = "ERROR: unknown command\n";

// Default peer update interval in seconds (15 minutes, matching peer config)
const long long defaultUpdateIntervalSeconds = 900;

// Shared folder where .track files are stored (loaded from sconfig at startup)
std::string sharedFolder;

// Maximum age (in seconds) before a peer entry is considered dead
long long updateIntervalSeconds = defaultUpdateIntervalSeconds;

// One mutex around every tracker-file read/modify/write. The handlers
// (once implemented) hold this around their load -> modify -> save
// sequence, because TrackerFile itself is not thread-safe.
std::mutex trackerFilesMutex;

// sconfig: line 1 = port number, line 2 = shared folder (torrents).
// Returns false if the file is missing, too short, or has a bad port.
bool readConfig(int& port, std::string& folder) {
    std::vector<std::string> config = readConfigLines("sconfig");
    if (config.size() < 2) {
        std::cerr << "Tracker: could not read sconfig (need port and folder)\n";
        return false;
    }

    // The port must be a plain number between 1 and 65535
    errno = 0;
    char* end = nullptr;
    long portValue = std::strtol(config[0].c_str(), &end, 10);
    if (config[0].empty() || errno != 0 || *end != '\0' ||
        portValue < 1 || portValue > 65535) {
        std::cerr << "Tracker: bad port '" << config[0] << "' in sconfig\n";
        return false;
    }
    port = static_cast<int>(portValue);

    if (config[1].empty()) {
        std::cerr << "Tracker: empty shared folder in sconfig\n";
        return false;
    }
    folder = config[1];

    // Optional 3rd line in sconfig for update interval; otherwise fall back
    // to peer1/clientThreadConfig.cfg (line 3) or defaultUpdateIntervalSeconds
    if (config.size() >= 3) {
        errno = 0;
        end = nullptr;
        long long intervalValue = std::strtoll(config[2].c_str(), &end, 10);
        if (config[2].empty() || errno != 0 || *end != '\0' || intervalValue < 1) {
            std::cerr << "Tracker: bad update interval '" << config[2] << "' in sconfig\n";
            return false;
        }
        updateIntervalSeconds = intervalValue;
    } else {
        std::vector<std::string> clientCfg = readConfigLines("peer1/clientThreadConfig.cfg");
        if (clientCfg.size() >= 3) {
            errno = 0;
            end = nullptr;
            long long intervalValue = std::strtoll(clientCfg.back().c_str(), &end, 10);
            if (!clientCfg.back().empty() && errno == 0 && *end == '\0' && intervalValue >= 1) {
                updateIntervalSeconds = intervalValue;
            }
        }
    }
    return true;
}

// Makes sure the shared folder exists and starts with no tracker files.
// Leftover .track files from an earlier run are deleted on purpose, so
// every server start is a clean start (the demo needs an empty folder).
void prepareSharedFolder(const std::string& folder) {
    if (mkdir(folder.c_str(), 0755) != 0 && errno != EEXIST) {
        std::cerr << "Tracker: could not create folder '" << folder
                  << "': " << std::strerror(errno) << "\n";
        return;  // not fatal: the handlers will just find nothing there
    }

    DIR* dir = opendir(folder.c_str());
    if (dir == nullptr) {
        std::cerr << "Tracker: could not open folder '" << folder << "'\n";
        return;
    }
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        // only files ending in .track (and not the bare name ".track")
        if (name.size() > 6 && name.compare(name.size() - 6, 6, ".track") == 0) {
            std::string path = folder + "/" + name;
            if (unlink(path.c_str()) == 0) {
                std::cout << "Tracker: removed leftover '" << path << "'\n";
            } else {
                std::cerr << "Tracker: could not remove leftover '" << path
                          << "': " << std::strerror(errno) << "\n";
            }
        }
    }
    closedir(dir);
}

// Creates the listening socket on port. Returns -1 on failure, with the
// error already printed.
int createListenSocket(int port) {
    int listenSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (listenSocket < 0) {
        std::cerr << "Tracker: socket failed: " << std::strerror(errno) << "\n";
        return -1;
    }

    // Without this, restarting the server right after stopping it fails
    // with "Address already in use" (the old port sits in TIME_WAIT)
    int reuse = 1;
    if (setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
        std::cerr << "Tracker: setsockopt failed: " << std::strerror(errno) << "\n";
        close(listenSocket);
        return -1;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);  // listen on every interface
    address.sin_port = htons(port);
    if (bind(listenSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        std::cerr << "Tracker: bind failed: " << std::strerror(errno) << "\n";
        close(listenSocket);
        return -1;
    }
    if (listen(listenSocket, listenBacklog) != 0) {
        std::cerr << "Tracker: listen failed: " << std::strerror(errno) << "\n";
        close(listenSocket);
        return -1;
    }
    return listenSocket;
}

// Reads one request line from the socket, looping until '\n' arrives
// (a single recv() can return only part of the line). A peer that
// closes without a newline still counts: its partial line is used.
// Returns false only when nothing at all could be read.
bool recvLine(int clientSocket, std::string& line) {
    line.clear();
    char buffer[512];
    while (line.find('\n') == std::string::npos) {
        if (line.size() >= maxRequestBytes) {
            return true;  // too long: stop reading, dispatch answers an error
        }
        ssize_t got = recv(clientSocket, buffer, sizeof(buffer), 0);
        if (got == 0) {
            break;  // peer closed the connection: use what we have so far
        }
        if (got < 0) {
            if (errno == EINTR) {
                continue;  // interrupted by a signal: try again
            }
            return false;  // real receive error
        }
        line.append(buffer, static_cast<std::size_t>(got));
    }
    return !line.empty();
}

// Sends the whole reply, looping because one send() may write only
// part of it. Returns false if the connection breaks.
bool sendAll(int clientSocket, const std::string& text) {
    std::size_t sent = 0;
    while (sent < text.size()) {
        ssize_t wrote = send(clientSocket, text.data() + sent, text.size() - sent, 0);
        if (wrote < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        sent += static_cast<std::size_t>(wrote);
    }
    return true;
}

// ---- request handlers (stubs: the real logic lands here later) ----

/// REQ LIST: will load every tracker in the shared folder and reply
/// with each file's name, size and MD5.
std::string handleList(const std::vector<std::string>& /*args*/) {
    return "LIST handler not implemented yet\n";
}

/// GET <filename>: will reply with the tracker file contents between
/// <REP GET BEGIN> and <REP GET END FileMD5>.
std::string handleGet(const std::vector<std::string>& /*args*/) {
    return "GET handler not implemented yet\n";
}

/// createtracker: will build a fresh tracker with TrackerFile::createSeed()
/// and save it as <filename>.track in the shared folder.
std::string handleCreateTracker(const std::vector<std::string>& /*args*/) {
    return "createtracker handler not implemented yet\n";
}

/// updatetracker: loads <filename>.track from the shared folder, removes dead
/// peers whose update interval has elapsed, adds or updates the calling peer's
/// byte range and timestamp, and saves the tracker file back to disk.
/// Wire format: <updatetracker filename start_bytes end_bytes ip-address port-number>\n
std::string handleUpdateTracker(const std::vector<std::string>& args) {
    if (args.empty()) {
        return "<updatetracker fail>\n";
    }

    const std::string& filename = args[0];

    if (args.size() != 5) {
        return "<updatetracker " + filename + " fail>\n";
    }

    const std::string& startBytesStr = args[1];
    const std::string& endBytesStr = args[2];
    const std::string& peerIp = args[3];
    const std::string& portStr = args[4];

    // Reject empty or path-traversing filenames
    if (filename.empty() || filename == "." || filename == ".." ||
        filename.find('/') != std::string::npos ||
        filename.find('\\') != std::string::npos) {
        return "<updatetracker " + filename + " fail>\n";
    }

    // Parse start_bytes and end_bytes (non-negative decimal integers, start <= end)
    errno = 0;
    char* end = nullptr;
    long long startByte = std::strtoll(startBytesStr.c_str(), &end, 10);
    if (startBytesStr.empty() || errno != 0 || *end != '\0' || startByte < 0) {
        return "<updatetracker " + filename + " fail>\n";
    }

    errno = 0;
    end = nullptr;
    long long endByte = std::strtoll(endBytesStr.c_str(), &end, 10);
    if (endBytesStr.empty() || errno != 0 || *end != '\0' || endByte < 0 ||
        endByte < startByte) {
        return "<updatetracker " + filename + " fail>\n";
    }

    // Parse port (1..65535)
    errno = 0;
    end = nullptr;
    long portValue = std::strtol(portStr.c_str(), &end, 10);
    if (portStr.empty() || errno != 0 || *end != '\0' ||
        portValue < 1 || portValue > 65535) {
        return "<updatetracker " + filename + " fail>\n";
    }
    int peerPort = static_cast<int>(portValue);

    if (peerIp.empty() || sharedFolder.empty()) {
        return "<updatetracker " + filename + " fail>\n";
    }

    // Validate IPv4 address format (also rejects colons that would break peer lines)
    in_addr parsedIp{};
    if (inet_pton(AF_INET, peerIp.c_str(), &parsedIp) != 1) {
        return "<updatetracker " + filename + " fail>\n";
    }

    std::string trackPath = sharedFolder + "/" + filename + ".track";

    std::lock_guard<std::mutex> lock(trackerFilesMutex);

    struct stat st{};
    if (stat(trackPath.c_str(), &st) != 0) {
        return "<updatetracker " + filename + " ferr>\n";
    }

    TrackerFile tracker;
    if (!tracker.load(trackPath)) {
        return "<updatetracker " + filename + " fail>\n";
    }

    if (endByte > tracker.filesize) {
        return "<updatetracker " + filename + " fail>\n";
    }

    tracker.pruneDead(updateIntervalSeconds);
    tracker.upsertPeer(peerIp, peerPort, startByte, endByte);

    if (!tracker.save(trackPath)) {
        return "<updatetracker " + filename + " fail>\n";
    }

    return "<updatetracker " + filename + " succ>\n";
}

// The words after the command word(s), passed to the handler as its
// arguments (empty for now, since every handler is a stub).
std::vector<std::string> argsAfter(const std::vector<std::string>& words, std::size_t skip) {
    std::vector<std::string> args;
    for (std::size_t i = skip; i < words.size(); i++) {
        args.push_back(words[i]);
    }
    return args;
}

// Turns one request line into a reply by routing it to its handler.
// Commands are accepted with or without the < > wrapper and with or
// without the trailing newline, e.g. "<REQ LIST>\n" and "REQ LIST".
std::string dispatch(const std::string& request) {
    std::string line = request;

    // drop the < > wrapper if it is there
    if (!line.empty() && line.front() == '<') {
        line.erase(0, 1);
    }
    // drop trailing whitespace (the newline) before looking for the
    // closing '>', otherwise "<REQ LIST>\n" keeps a "LIST>" word
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) {
        line.pop_back();
    }
    if (!line.empty() && line.back() == '>') {
        line.pop_back();
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) {
            line.pop_back();
        }
    }

    // split into words; the first word (or two) names the command
    std::vector<std::string> words;
    std::istringstream stream(line);
    std::string word;
    while (stream >> word) {
        words.push_back(word);
    }
    if (words.empty()) {
        return unknownCommandReply;
    }

    if (words[0] == "REQ" && words.size() > 1 && words[1] == "LIST") {
        return handleList(argsAfter(words, 2));
    }
    if (words[0] == "GET") {
        return handleGet(argsAfter(words, 1));
    }
    if (words[0] == "createtracker") {
        return handleCreateTracker(argsAfter(words, 1));
    }
    if (words[0] == "updatetracker") {
        return handleUpdateTracker(argsAfter(words, 1));
    }
    return unknownCommandReply;
}

// Serves exactly one peer: read one request line, send the reply,
// close. Runs in its own thread, so nothing here may take the server
// down: every failure becomes a log line at most.
void serveConnection(int clientSocket, const std::string& peerAddress) {
    std::string request;
    if (!recvLine(clientSocket, request)) {
        std::cerr << "Tracker: " << peerAddress << " left before sending a request\n";
        close(clientSocket);
        return;
    }

    std::string reply = dispatch(request);

    if (!sendAll(clientSocket, reply)) {
        std::cerr << "Tracker: " << peerAddress << " left before the reply arrived\n";
    }
    close(clientSocket);
}

}  // namespace

int main() {
    int port = 0;
    std::string folder;
    if (!readConfig(port, folder)) {
        return 1;
    }

    sharedFolder = folder;
    prepareSharedFolder(folder);

    // A worker whose peer disappears mid-reply would otherwise take a
    // SIGPIPE and kill the whole server with it
    std::signal(SIGPIPE, SIG_IGN);

    int listenSocket = createListenSocket(port);
    if (listenSocket < 0) {
        return 1;
    }
    std::cout << "Tracker: listening on port " << port
              << " (shared folder: " << folder << ")\n";

    // The accept loop only accepts and hands off; all request handling
    // happens inside the worker threads it spawns
    while (true) {
        sockaddr_in peerAddress{};
        socklen_t peerLength = sizeof(peerAddress);
        int clientSocket = accept(listenSocket,
                                  reinterpret_cast<sockaddr*>(&peerAddress),
                                  &peerLength);
        if (clientSocket < 0) {
            if (errno == EINTR) {
                continue;  // interrupted by a signal: just accept again
            }
            std::cerr << "Tracker: accept failed: " << std::strerror(errno) << "\n";
            continue;  // one bad accept must not stop the server
        }

        char peerIp[INET_ADDRSTRLEN];
        const char* printed = inet_ntop(AF_INET, &peerAddress.sin_addr, peerIp, sizeof(peerIp));
        std::string peer = printed != nullptr
                               ? std::string(peerIp) + ":" + std::to_string(ntohs(peerAddress.sin_port))
                               : "unknown address";
        std::cout << "Tracker: connection from " << peer << "\n";

        // hand the connection to its worker right away
        try {
            std::thread worker(serveConnection, clientSocket, peer);
            worker.detach();
        } catch (const std::system_error& e) {
            // out of threads or similar: refuse this one connection
            std::cerr << "Tracker: could not start worker thread: " << e.what() << "\n";
            close(clientSocket);
        }
    }
}
