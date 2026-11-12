// peer.cpp
// Peer program for DefinitelyNotLimewire
// Config Files:
//  clientThreadConfig.cfg: tracker port, tracker IP, update intervals, etc.
//  serverThreadConfig.cfg: listening port, shared folder name, etc.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
 
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
 
#include "common.h"
 
namespace fs = std::filesystem;
 
// Settings
struct Settings 
{
    // name shown in log lines (the folder name "peer1")
    std::string id;
    std::string trackerIp;
    int trackerPort = 0;
    // seconds between automatic updatetracker rounds
    long updateInterval = 900;
    int listenPort = 0;
    std::string sharedDir;
    // downloaded tracker files are kept here
    std::string cacheDir = "cache";
    // our own IP when talking to the tracker
    std::string myIp;
};
static Settings g;
 
// Print one log line tagged with this peer's name
static void say(const std::string& msg) { logLine("[" + g.id + "] " + msg); }
 
/* Finds the computer's own IP since it can't be in configs. It creates a UDP socket and calls connect()
on that socket to the tracker's IP and port. Then it calls getsockname() to read back which local address
the OS chose. Then it closes the socket and returns that address.
*/
static std::string findOwnIp(const std::string& trackerIp, int trackerPort) 
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return "127.0.0.1";
    sockaddr_in t;
    std::memset(&t, 0, sizeof(t));
    t.sin_family = AF_INET;
    t.sin_port = htons((uint16_t)trackerPort);
    std::string result = "127.0.0.1";
    if (inet_pton(AF_INET, trackerIp.c_str(), &t.sin_addr) == 1 && connect(s, (sockaddr*)&t, sizeof(t)) == 0) 
    {
        sockaddr_in me;
        socklen_t len = sizeof(me);
        char buf[INET_ADDRSTRLEN];
        if (getsockname(s, (sockaddr*)&me, &len) == 0 && inet_ntop(AF_INET, &me.sin_addr, buf, sizeof(buf)))
            result = buf;
    }
    close(s);
    return result;
}
 
// Issue #11 - Server thread serving file chunks
 
// Handles one request line and returns false if the connection should be closed
static bool handleChunkRequest(int fd, const std::string& line, const std::string& from, int& served, uint64_t& bytes) 
{
    std::vector<std::string> w = splitWords(stripBrackets(line));
    uint64_t start, end;
 
    // Must look exactly like: GET filename start end
    if (w.size() != 4 || toLower(w[0]) != "get" || !parseU64(w[2], start) || !parseU64(w[3], end) || end <= start) 
    {
        say("bad chunk request from " + from + ": " + line.substr(0, 60));
        return sendStr(fd, "<GET invalid>\n");
    }
 
    // Strict 1024-byte upper limit
    if (end - start > MAX_CHUNK) 
    {
        say("rejected chunk request from " + from + ": " + std::to_string(end - start) + " bytes is over the " + std::to_string(MAX_CHUNK) + "-byte limit");
        return sendStr(fd, "<GET invalid>\n");
    }
 
    // Only files inside the shared folder can be served
    if (!isSafeName(w[1])) 
    {
        say("rejected unsafe file name from " + from);
        return sendStr(fd, "<GET invalid>\n");
    }
    std::string path = g.sharedDir + "/" + w[1];
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) 
    {
        say("file " + w[1] + " requested by " + from + " but not shared here");
        return sendStr(fd, "<GET ferr>\n");
    }
    uint64_t fileSize = fs::file_size(path, ec);
    if (ec || end > fileSize) 
    {
        say("range " + std::to_string(start) + "-" + std::to_string(end) + " of " + w[1] + " is outside the file (" + std::to_string(fileSize) + " bytes)");
        return sendStr(fd, "<GET invalid>\n");
    }
 
    // Read exactly the requested bytes
    std::string data((size_t)(end - start), '\0');
    std::ifstream f(path, std::ios::binary);
    f.seekg((std::streamoff)start);
    f.read(&data[0], (std::streamsize)data.size());
    if ((uint64_t)f.gcount() != data.size()) return sendStr(fd, "<GET ferr>\n");
 
    if (served == 0)
        say("serving " + w[1] + " to " + from + " (first chunk " + std::to_string(start) + "-" + std::to_string(end) + ")");
    if (!sendStr(fd, "<REP GET CHUNK " + std::to_string(data.size()) + ">\n") || !sendAll(fd, data.data(), data.size()))
        return false;
    served++;
    bytes += data.size();
    return true;
}
 
// One thread per peer and it lives until the peer disconnects or goes idle
static void servePeer(int fd, std::string from) 
{
    setTimeouts(fd, 15);
    int served = 0;
    uint64_t bytes = 0;
    std::string line;
    while (recvLine(fd, line))
        if (!handleChunkRequest(fd, line, from, served, bytes)) break;
    close(fd);
    if (served > 0)
        say("finished serving " + from + ": " + std::to_string(served) + " chunks, " + std::to_string(bytes) + " bytes");
}
 
// The listening thread: accepts connections and gives each to servePeer()
static void serverThread() 
{
    int server = listenOn(g.listenPort);
    if (server < 0) 
    {
        say("ERROR: cannot listen on port " + std::to_string(g.listenPort) + " (already in use?)");
        std::exit(1);
    }
    say("server thread listening on port " + std::to_string(g.listenPort) + ", sharing folder " + g.sharedDir);
    while (true) 
    {
        sockaddr_in addr;
        socklen_t len = sizeof(addr);
        int fd = accept(server, (sockaddr*)&addr, &len);
        if (fd < 0) continue;
        char ip[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
        std::thread(servePeer, fd, std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port)))
            .detach();
    }
}
 
// Issue # 10 - tracker client with manual command mode
 
// Open a connection to the tracker
static int openTracker() 
{
    int fd = connectTo(g.trackerIp, g.trackerPort, 5);
    if (fd < 0)
        say("ERROR: cannot reach tracker at " + g.trackerIp + ":" + std::to_string(g.trackerPort));
    return fd;
}
 
// Send one message and read one reply line
static bool exchange(const std::string& msg, std::string& reply) 
{
    int fd = openTracker();
    if (fd < 0) return false;
    bool ok = sendStr(fd, "<" + msg + ">\n") && recvLine(fd, reply);
    close(fd);
    if (!ok) say("ERROR: tracker did not answer");
    return ok;
}
 
static std::string trackerFileName(const std::string& n) {
    const std::string ext = ".track";
    if (n.size() > ext.size() && n.compare(n.size() - ext.size(), ext.size(), ext) == 0) return n;
    return n + ext;
}
 
/* Gets the details about the file and puts them into one message. Then it sends that message to the tracker
and waits for the reply. Then it prints the outcome (succ: tracker file was created, ferr: tracker file for this
name already exists, fail: something went wrong with the tracker).
*/
static bool createTracker(const std::string& name, const std::string& size, const std::string& desc, const std::string& md5, const std::string& ip, const std::string& port) 
{
    say("createtracker " + name + " " + size + " " + desc + " " + md5 + " " + ip + " " + port);
    std::string reply;
    if (!exchange("createtracker " + name + " " + size + " " + desc + " " + md5 + " " + ip + " " + port, reply))
        return false;
    std::string r = stripBrackets(reply);
    if (r == "createtracker succ")      
        say("createtracker " + name + ": SUCCESS (tracker file created)");
    else if (r == "createtracker ferr") 
        say("createtracker " + name + ": tracker file already exists");
    else if (r == "createtracker fail") 
        say("createtracker " + name + ": FAILED at the tracker");
    else                                
        say("createtracker " + name + ": unexpected reply: " + reply);
    return r == "createtracker succ";
}
 
/* Gets the file name, start and end byte, IP, and port and puts them into one message. Then it sends that message to the tracker
and waits for the reply. Then it prints the outcome (succ: tracker recorded the check-in, ferr: tracker has no tracker file for
that name, fail: something went wrong at the tracker).
*/
static bool updateTracker(const std::string& name, const std::string& start, const std::string& end, const std::string& ip, const std::string& port) {
    say("updatetracker " + name + " " + start + " " + end + " " + ip + " " + port);
    std::string reply;
    if (!exchange("updatetracker " + name + " " + start + " " + end + " " + ip + " " + port, reply))
        return false;
    std::vector<std::string> w = splitWords(stripBrackets(reply));
    std::string result = (w.size() == 3 && w[0] == "updatetracker") ? w[2] : "?";
    if (result == "succ")      
        say("updatetracker " + name + ": SUCCESS");
    else if (result == "ferr") 
        say("updatetracker " + name + ": tracker has no file for it");
    else if (result == "fail") 
        say("updatetracker " + name + ": FAILED at the tracker");
    else                       
        say("updatetracker " + name + ": unexpected reply: " + reply);
    return result == "succ";
}
 
/* Connects to the tracker and sends <REQ LIST>. Then the tracker replies with a header saying how many files it has. 
Then it reads one line per file until it sees <REP LIST END> and prints each line on the screen.
*/
static bool listFiles() 
{
    say("REQ LIST");
    int fd = openTracker();
    if (fd < 0) return false;
    std::string line;
    bool ok = sendStr(fd, "<REQ LIST>\n") && recvLine(fd, line);
    std::vector<std::string> head = splitWords(stripBrackets(line));
    if (!ok || head.size() != 3 || head[0] != "REP" || head[1] != "LIST") 
    {
        say("ERROR: bad reply to LIST: " + line);
        close(fd);
        return false;
    }
    say("tracker has " + head[2] + " file(s):");
    while (recvLine(fd, line) && stripBrackets(line) != "REP LIST END")
        logLine("    " + stripBrackets(line));
    close(fd);
    return true;
}
 
/* Downloads a file's tracker file from the tracker, makes sure it's not corrupted, and saves it. It runs when you type GET <name>.track
It makes sure the name ends in .track, then sends <GET <name>.track> to the tracker. If the tracker doesn't have the file, it replies
<GET ferr> and prints "no such tracker file" and stops. If it does it sends <REP GET BEGIN>, then the contents line by line then 
<REP GET END md5>. Then it computes the md5 of what it received and compares it to the md5 the tracker sent. If they're different
the data was damaged somehow, so it discards it and errors. If they match, it saves the file into the cache folder and prints the contents.
*/
static bool getTrackerFile(const std::string& nameIn) 
{
    std::string name = trackerFileName(nameIn);
    say("GET " + name);
    int fd = openTracker();
    if (fd < 0) return false;
 
    std::string line;
    if (!sendStr(fd, "<GET " + name + ">\n") || !recvLine(fd, line)) 
    {
        say("ERROR: tracker did not answer the GET");
        close(fd);
        return false;
    }
    if (stripBrackets(line) != "REP GET BEGIN") 
    {
        say("GET " + name + ": tracker says " + line +
            (stripBrackets(line) == "GET ferr" ? " (no such tracker file)" : ""));
        close(fd);
        return false;
    }
 
    // Collect file content until the "<REP GET END md5>" line
    std::string content, md5Sent;
    bool gotEnd = false;
    while (recvLine(fd, line)) 
    {
        std::vector<std::string> w = splitWords(stripBrackets(line));
        if (w.size() == 4 && w[0] == "REP" && w[1] == "GET" && w[2] == "END") 
        {
            md5Sent = w[3];
            gotEnd = true;
            break;
        }
        content += line + "\n";
    }
    close(fd);
    if (!gotEnd) 
    { 
        say("ERROR: GET " + name + " ended early, discarding it"); 
        return false; 
    }
 
    // MD5 check: only keep the tracker file if the checksum matches
    if (md5String(content) != md5Sent) 
    {
        say("ERROR: GET " + name + ": MD5 mismatch, tracker data discarded");
        return false;
    }
    std::error_code ec;
    fs::create_directories(g.cacheDir, ec);
    std::ofstream out(g.cacheDir + "/" + name, std::ios::binary | std::ios::trunc);
    if (!out || !(out << content).flush()) 
    {
        say("ERROR: cannot save " + g.cacheDir + "/" + name);
        return false;
    }
    say("GET " + name + ": MD5 OK, saved to " + g.cacheDir + "/" + name);
    std::string indented;
    for (const std::string& l : splitChar(content.substr(0, content.size() - 1), '\n'))
        indented += "    " + l + "\n";
    std::cout << indented << std::flush;
    return true;
}
 
// Looks up a file in shared folder and finds its size and MD5.
static bool describeSharedFile(const std::string& name, uint64_t& size, std::string& md5) 
{
    std::string path = g.sharedDir + "/" + name;
    std::error_code ec;
    if (!isSafeName(name) || !fs::is_regular_file(path, ec)) 
        return false;
    size = fs::file_size(path, ec);
    return !ec && md5File(path, md5);
}
 
// Background thread that checks in with the tracker automatically
static void updaterThread() 
{
    while (true) 
    {
        std::this_thread::sleep_for(std::chrono::seconds(g.updateInterval));
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(g.sharedDir, ec)) 
        {
            if (!entry.is_regular_file()) continue;
            std::string name = entry.path().filename().string();
            if (name[0] == '.' || (name.size() > 5 && name.compare(name.size() - 5, 5, ".part") == 0))
                continue;
            uint64_t size = fs::file_size(entry.path(), ec);
            if (ec) 
                continue;
            updateTracker(name, "0", std::to_string(size), g.myIp, std::to_string(g.listenPort));
        }
    }
}
 
static void printHelp() 
{
    std::cout <<
        "Commands (names are not case sensitive):\n"
        "  REQ LIST                                  ask tracker for its file list\n"
        "  GET <name>.track                          download a tracker file (MD5 checked)\n"
        "  createtracker <file> <size> <desc> <md5> <ip> <port>\n"
        "  createtracker <file> [description]        short form: size/md5/ip/port filled in\n"
        "  updatetracker <file> <start> <end> <ip> <port>\n"
        "  updatetracker <file>                      short form: whole file, our ip/port\n"
        "  help | quit\n";
}
 
// Command Prompt: lets you type commands into the peer> prompt and runs the matching function.
static void manualMode() 
{
    printHelp();
    std::string line;
    while (true) 
    {
        std::cout << g.id << "> " << std::flush;
        if (!std::getline(std::cin, line)) 
        {
            // stdin closed: keep serving in the background
            while (true) std::this_thread::sleep_for(std::chrono::hours(1));
        }
        std::vector<std::string> w = splitWords(line);
        if (w.empty()) continue;
        std::string cmd = toLower(w[0]);
 
        if (cmd == "quit" || cmd == "exit") 
        {
            say("terminated");
            return;
        } 
        else if (cmd == "help" || cmd == "?") 
        {
            printHelp();
        } 
        else if ((cmd == "req" && w.size() == 2 && toLower(w[1]) == "list") || (cmd == "list" && w.size() == 1)) 
        {
            listFiles();
        } 
        else if (cmd == "get" && w.size() == 2) 
        {
            getTrackerFile(w[1]);
        } 
        else if (cmd == "createtracker" && w.size() == 7) 
        {
            createTracker(w[1], w[2], w[3], w[4], w[5], w[6]);
        } 
        else if (cmd == "createtracker" && w.size() >= 2) 
        {
            uint64_t size; std::string md5;
            if (!describeSharedFile(w[1], size, md5)) { say("file " + w[1] + " not found in " + g.sharedDir); continue; }
            std::string desc;
            for (size_t i = 2; i < w.size(); i++) desc += (i > 2 ? "_" : "") + w[i];
            if (desc.empty()) desc = "no_description";
            createTracker(w[1], std::to_string(size), desc, md5, g.myIp, std::to_string(g.listenPort));
        } 
        else if (cmd == "updatetracker" && w.size() == 6) 
        {
            updateTracker(w[1], w[2], w[3], w[4], w[5]);
        } 
        else if (cmd == "updatetracker" && w.size() == 2) 
        {
            uint64_t size; std::string md5;
            if (!describeSharedFile(w[1], size, md5)) { say("file " + w[1] + " not found in " + g.sharedDir); continue; }
            updateTracker(w[1], "0", std::to_string(size), g.myIp, std::to_string(g.listenPort));
        } 
        else 
        {
            std::cout << "Unknown or incomplete command. Type 'help'.\n";
        }
    }
}
 
/* Reads the two config files. Checks the numbers: ports have to be 1-65535 and the interval must be over 0. Sets up its name
and folders: uses current folder's name to label the log lines and creates the shared and cache folder if they're missing. 
Finds computer's IP with findOwnIP(), so the tracker knows where other peers can reach it. Prints startup line. Starts the
server thread in the background, which listens for other peers and server chunks then waits briefly to let it bind. Starts the
updater thread in the background then calls manualMode() to let the user input commands. Type "quit" to end.
*/
int main() 
{
    // clientThreadConfig.cfg: tracker port, tracker IP, update interval (seconds)
    std::vector<std::string> client = readConfigLines("clientThreadConfig.cfg");
 
    // serverThreadConfig.cfg: this peer's listening port, shared folder name
    std::vector<std::string> server = readConfigLines("serverThreadConfig.cfg");
 
    // Stop with an error if either file is missing/incomplete
    if (client.size() < 3 || server.size() < 2) 
    {
        std::cerr << "Peer: could not read config files\n";
        return 1;
    }
 
    uint64_t interval;
    if (!parsePort(client[0], g.trackerPort) || !parsePort(server[0], g.listenPort) || !parseU64(client.back(), interval) || interval == 0) 
    {
        std::cerr << "Peer: bad number in a config file (ports 1-65535, interval > 0)\n";
        return 1;
    }
    g.trackerIp = client[1];
    g.updateInterval = (long)interval;
    g.sharedDir = server.back();
 
    // Use the current folder's name to label log lines
    std::error_code ec;
    g.id = fs::current_path(ec).filename().string();
    if (g.id.empty()) 
        g.id = "peer";
    fs::create_directories(g.sharedDir, ec);
    fs::create_directories(g.cacheDir, ec);
    if (!fs::is_directory(g.sharedDir)) 
    {
        std::cerr << "Peer: cannot use shared folder " << g.sharedDir << "\n";
        return 1;
    }
    g.myIp = findOwnIp(g.trackerIp, g.trackerPort);
 
    signal(SIGPIPE, SIG_IGN);
    say("tracker " + g.trackerIp + ":" + std::to_string(g.trackerPort) + ", my address " + g.myIp +
        ":" + std::to_string(g.listenPort) + ", update every " + std::to_string(g.updateInterval) + "s");
 
    std::thread(serverThread).detach();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::thread(updaterThread).detach();
    manualMode();
    return 0;
}