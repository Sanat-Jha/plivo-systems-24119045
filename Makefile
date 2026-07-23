CXX ?= g++
CXXFLAGS ?= -O2 -Wall -std=c++17

all: sender receiver

sender: sender.cpp
	$(CXX) $(CXXFLAGS) -o sender sender.cpp

receiver: receiver.cpp
	$(CXX) $(CXXFLAGS) -o receiver receiver.cpp

clean:
	rm -f sender receiver
