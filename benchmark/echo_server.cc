// Minimal HTTP echo server for benchmarking.
// Baseline mode: message callback runs on EventLoop thread (locked task queue path).
// LSE mode:      message callback runs on StealingEngine worker (lock-free steal path).
//
// Usage:
//   ./echo_server [--port P] [--io N] [--lse W]
//     --port P   listen port (default 8500)
//     --io   N   IO thread count (default 4)
//     --lse  W   LSE worker count; 0 = baseline (default 0)

#include "../muduo_net/server.hpp"
#include <cstring>
#include <cstdlib>
#include <cstdio>

static const char RESP[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Length: 13\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "Hello, World!";

static void OnMessage(const PtrConnection& conn, Buffer* buf) {
    // Wait for complete HTTP headers before replying
    std::string data(buf->ReadPosition(), buf->ReadAbleSize());
    if (data.find("\r\n\r\n") == std::string::npos) return;
    buf->MoveReadOffset(buf->ReadAbleSize());
    conn->Send(RESP, sizeof(RESP) - 1);
}

int main(int argc, char* argv[]) {
    int port       = 8500;
    int io_threads = 4;
    int lse_workers = 0;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port        = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--io")   && i + 1 < argc) io_threads  = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--lse")  && i + 1 < argc) lse_workers = atoi(argv[++i]);
    }

    fprintf(stderr, "[bench] port=%d io_threads=%d lse_workers=%d\n",
            port, io_threads, lse_workers);

    TcpServer server(port);
    server.SetThreadCount(io_threads);
    server.EnableInactiveRelease(30);
    server.SetMessageCallback(OnMessage);
    if (lse_workers > 0) server.EnableLSE(lse_workers);
    server.Start();
    return 0;
}
