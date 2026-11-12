// common.h
// Shared helper functions used by the tracker and the peer
 
#ifndef COMMON_H
#define COMMON_H
 
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
 
// Reads a config file and returns its non-empty lines, in order, with
// leading/trailing whitespace removed.
// Returns an empty list if the file can't be opened.
std::vector<std::string> readConfigLines(const std::string& path);
 
// Reads a config file into lines like readConfigLines, but tells a
// missing/unreadable file apart from a short one. On failure it prints a
// clear error that names the file and
// what it must contain (usage), and returns false.
bool loadConfigFile(const std::string& who, const std::string& path,
                    std::size_t minLines, const std::string& usage,
                    std::vector<std::string>& lines);
 
// Parses a whole-string decimal number in [minValue, maxValue].
// Returns false for empty text, junk characters, or out-of-range values.
bool parseIntInRange(const std::string& text, long minValue, long maxValue, int& value);
 
// Parses a TCP port number (1-65535).
bool parsePort(const std::string& text, int& port);
 
// ---------------------------------------------------------------------------
// Helpers used by the peer program (text, logging, sockets, MD5)
// ---------------------------------------------------------------------------
 
// Largest chunk of file data a peer may request or send (spec: 1024 bytes)
const std::uint64_t MAX_CHUNK = 1024;
 
// Prints one line to stdout. Thread-safe, so lines from different threads
// never get mixed together.
void logLine(const std::string& text);
 
// Lowercases ASCII letters.
std::string toLower(const std::string& text);
 
// Splits text at every sep character (empty fields are kept).
std::vector<std::string> splitChar(const std::string& text, char sep);
 
// Splits text on runs of whitespace (no empty words).
std::vector<std::string> splitWords(const std::string& text);
 
// Trims whitespace, then removes one leading '<' and one trailing '>'
// ("<REQ LIST>" -> "REQ LIST"). Text without brackets is returned trimmed.
std::string stripBrackets(const std::string& text);
 
// Parses a whole-string non-negative decimal number. False on junk/overflow.
bool parseU64(const std::string& text, std::uint64_t& value);
 
// True if name is safe to use as a file name inside a folder: not empty,
// no '/' or '\\', not "." or "..", no control characters.
bool isSafeName(const std::string& name);
 
// MD5 wrappers (see md5.h). md5File returns false if the file can't be read.
std::string md5String(const std::string& data);
bool md5File(const std::string& path, std::string& md5);
 
// Sends every byte (handles partial sends). False on error.
bool sendAll(int fd, const char* data, std::size_t length);
 
// Sends a string with sendAll.
bool sendStr(int fd, const std::string& text);
 
// Reads one line (up to '\n') into line, without the '\n' or a '\r'.
// False on error, timeout, closed connection with no data, or a line
// longer than 64 KB.
bool recvLine(int fd, std::string& line);
 
// Sets send and receive timeouts on a socket (seconds).
void setTimeouts(int fd, int seconds);
 
// Opens a TCP connection to ip:port, giving up after timeoutSeconds.
// Returns the socket, or -1 on failure.
int connectTo(const std::string& ip, int port, int timeoutSeconds);
 
// Creates a TCP socket listening on all interfaces at port.
// Returns the socket, or -1 on failure.
int listenOn(int port);
 
#endif
