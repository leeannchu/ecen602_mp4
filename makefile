CXX      = g++
CXXFLAGS = -Wall -Wextra -std=c++17 -g

all: proxy client

proxy: proxy.o http.o cache.o
	$(CXX) $(CXXFLAGS) -o $@ $^

client: client.o
	$(CXX) $(CXXFLAGS) -o $@ $^

test_unit: test_unit.o http.o cache.o
	$(CXX) $(CXXFLAGS) -o $@ $^

test: test_unit
	./test_unit

%.o: %.cpp http.h cache.h
	$(CXX) $(CXXFLAGS) -c $<

clean:
	rm -f *.o proxy client test_unit

.PHONY: all test clean
