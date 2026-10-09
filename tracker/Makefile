CXX = g++
CXXFLAGS = -std=c++17 -Wall -pthread
LIBS = -lssl -lcrypto

tracker: tracker.cpp
	$(CXX) $(CXXFLAGS) -o tracker tracker.cpp $(LIBS)

clean:
	rm -f tracker