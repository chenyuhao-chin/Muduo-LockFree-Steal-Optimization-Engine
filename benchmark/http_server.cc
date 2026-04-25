#include "../examples/http/http.hpp"
#include <cstring>
#include <cstdlib>
#include <cstdio>

int main(int argc, char* argv[]) {
    int port       = 8085;
    int io_threads = 4;
    int lse_workers = 0;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--port") && i+1 < argc) port        = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--io")  && i+1 < argc) io_threads  = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--lse") && i+1 < argc) lse_workers = atoi(argv[++i]);
    }

    fprintf(stderr, "[bench] port=%d io=%d lse=%d\n", port, io_threads, lse_workers);

    HttpServer server(port);
    server.SetThreadCount(io_threads);
    if (lse_workers > 0) server.EnableLSE(lse_workers);

    server.Get("/hello", [](const HttpRequest&, HttpResponse* rsp){
        rsp->SetContent("Hello, World!", "text/plain");
    });

    server.Listen();
    return 0;
}
