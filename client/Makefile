CXX = g++
CXXFLAGS = -std=c++17 -Wall -pthread
LIBS = -lssl -lcrypto

client: client.cpp
	$(CXX) $(CXXFLAGS) -o client client.cpp $(LIBS)

clean:
	rm -f client