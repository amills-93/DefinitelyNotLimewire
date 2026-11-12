# makefile for DefinitelyNotLimewire 
#
# make -f makefile --> builds the tracker and the three peers
# make clean --> deletes everything the build created

# Which compiler to use and its settings
CXX      = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -pthread

# Libraries to link: OpenSSL's crypto library (for MD5)
LDLIBS   = -lcrypto

# Source files for each program
TRACKER_SRC = src/tracker.cpp src/common.cpp src/tracker_file.cpp src/md5.cpp
PEER_SRC    = src/peer.cpp src/common.cpp src/tracker_file.cpp src/md5.cpp
HEADERS     = src/common.h src/tracker_file.h src/md5.h

# The three simulated peer folders (spec: "How To Run the Peer Program")
PEERS = peer1 peer2 peer3

# Default rule: running "make" builds both the tracker and the peers
all: tracker peers

# Build the tracker executable in the main folder.
# Also make sure its shared folder (torrents) exists.
tracker: $(TRACKER_SRC) $(HEADERS)
	mkdir -p torrents
	$(CXX) $(CXXFLAGS) $(TRACKER_SRC) -o tracker $(LDLIBS)

# Build the peer once, copy it into peer1/, peer2/, peer3/,
# make each peer's shared folder, then delete the extra copy.
peers: $(PEER_SRC) $(HEADERS)
	$(CXX) $(CXXFLAGS) $(PEER_SRC) -o peer $(LDLIBS)
	for p in $(PEERS); do mkdir -p $$p/shared; cp peer $$p/peer; done
	rm -f peer

# Delete the compiled programs (so none get submitted)
clean:
	rm -f tracker peer
	for p in $(PEERS); do rm -f $$p/peer; done

# These rule names are not real files
.PHONY: all peers clean
