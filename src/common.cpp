// common.cpp
// Code for the shared helper functions listed in common.h.
 
#include "common.h"
 
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
 
#include <mutex>
 
#include "md5.h"
 
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
 
namespace {
 
// Removes leading and trailing whitespace (including a Windows "\r")
std::string trim(const std::string& text) {
    std::size_t start = 0;
    std::size_t end = text.size();
    while (start < end && std::isspace(static_cast<unsigned char>(text[start]))) {
        start++;
    }
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        end--;
    }
    return text.substr(start, end - start);
}
 
// Reads the non-blank, trimmed lines of an already opened file
std::vector<std::string> readLines(std::ifstream& file) {
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    return lines;
}
 
}  // namespace
 
std::vector<std::string> readConfigLines(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        return {};
    }
    return readLines(file);
}
 
bool loadConfigFile(const std::string& who, const std::string& path,
                    std::size_t minLines, const std::string& usage,
                    std::vector<std::string>& lines) {
    std::ifstream file(path);
    if (!file) {
        std::cerr << who << ": cannot open config file '" << path << "': "
                  << std::strerror(errno) << "\n"
                  << who << ": it must be in the directory the program is run from and contain:\n"
                  << usage;
        return false;
    }
    lines = readLines(file);
    if (lines.size() < minLines) {
        std::cerr << who << ": config file '" << path << "' is incomplete (found "
                  << lines.size() << " non-blank line(s), need at least " << minLines
                  << "). Expected:\n"
                  << usage;
        return false;
    }
    return true;
}
 
bool parseIntInRange(const std::string& text, long minValue, long maxValue, int& value) {
    if (text.empty()) {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    long parsed = std::strtol(text.c_str(), &end, 10);
    if (errno != 0 || *end != '\0' || parsed < minValue || parsed > maxValue) {
        return false;
    }
    value = static_cast<int>(parsed);
    return true;
}
 
bool parsePort(const std::string& text, int& port) {
    return parseIntInRange(text, 1, 65535, port);
}
 
// ---------------------------------------------------------------------------
// Helpers used by the peer program
// ---------------------------------------------------------------------------
 
void logLine(const std::string& text) {
    static std::mutex outputMutex;
    std::lock_guard<std::mutex> lock(outputMutex);
    std::cout << text << std::endl;  // endl flushes, so lines appear at once
}
 
std::string toLower(const std::string& text) {
    std::string out = text;
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}
 
std::vector<std::string> splitChar(const std::string& text, char sep) {
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
 
std::vector<std::string> splitWords(const std::string& text) {
    std::vector<std::string> words;
    std::string word;
    for (char c : text) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!word.empty()) {
                words.push_back(word);
                word.clear();
            }
        } else {
            word.push_back(c);
        }
    }
    if (!word.empty()) {
        words.push_back(word);
    }
    return words;
}
 
std::string stripBrackets(const std::string& text) {
    std::string t = trim(text);
    if (!t.empty() && t.front() == '<') {
        t.erase(0, 1);
    }
    if (!t.empty() && t.back() == '>') {
        t.pop_back();
    }
    return t;
}
 
bool parseU64(const std::string& text, std::uint64_t& value) {
    // strtoull accepts a leading '-' or '+', so insist on digits only
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
    if (errno != 0 || *end != '\0') {
        return false;
    }
    value = static_cast<std::uint64_t>(parsed);
    return true;
}
 
bool isSafeName(const std::string& name) {
    if (name.empty() || name == "." || name == "..") {
        return false;
    }
    for (char c : name) {
        if (c == '/' || c == '\\' || std::iscntrl(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return true;
}
 
std::string md5String(const std::string& data) {
    return md5OfString(data);
}
 
bool md5File(const std::string& path, std::string& md5) {
    md5 = md5OfFile(path);
    return !md5.empty();
}
 
bool sendAll(int fd, const char* data, std::size_t length) {
    std::size_t sent = 0;
    while (sent < length) {
        ssize_t n = send(fd, data + sent, length - sent, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}
 
bool sendStr(int fd, const std::string& text) {
    return sendAll(fd, text.data(), text.size());
}
 
bool recvLine(int fd, std::string& line) {
    const std::size_t maxLine = 64 * 1024;
    line.clear();
    // One byte at a time so we never read past the end of the line: the
    // bytes after it (like chunk data) belong to the caller.
    while (line.size() < maxLine) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;  // error, timeout or closed
        }
        if (c == '\n') {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            return true;
        }
        line.push_back(c);
    }
    return false;
}
 
void setTimeouts(int fd, int seconds) {
    struct timeval tv;
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}
 
int connectTo(const std::string& ip, int port, int timeoutSeconds) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        close(fd);
        return -1;
    }
 
    // Connect without blocking so we can enforce our own timeout
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int result = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (result < 0 && errno == EINPROGRESS) {
        fd_set writeSet;
        FD_ZERO(&writeSet);
        FD_SET(fd, &writeSet);
        struct timeval tv;
        tv.tv_sec = timeoutSeconds;
        tv.tv_usec = 0;
        if (select(fd + 1, nullptr, &writeSet, nullptr, &tv) == 1) {
            int err = 0;
            socklen_t len = sizeof(err);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
            result = (err == 0) ? 0 : -1;
        }
    }
    if (result < 0) {
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, flags);  // back to blocking
    setTimeouts(fd, 15);
    return fd;
}
 
int listenOn(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(fd, 16) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}
