CXX      = g++
CXXFLAGS = -O3 -std=c++17 -DLOG_LEVEL=3 \
           -I../muduo_net -I../lse_engine -I../examples/http \
           -lpthread

all: echo_server http_server

echo_server: echo_server.cc ../muduo_net/server.hpp \
             ../lse_engine/StealingEngine.h \
             ../lse_engine/WorkStealingDeque.h \
             ../lse_engine/PaddedAtomic.h
	$(CXX) $(CXXFLAGS) -o echo_server echo_server.cc

http_server: http_server.cc ../examples/http/http.hpp ../examples/http/cpu_work.h \
             ../muduo_net/server.hpp \
             ../lse_engine/StealingEngine.h
	$(CXX) $(CXXFLAGS) -o http_server http_server.cc

clean:
	rm -f echo_server http_server
