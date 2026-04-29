#include "../examples/http/http.hpp"
#include "../examples/http/cpu_work.h"
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <random>

// 线程安全的单调递增 ID 生成器
static std::atomic<uint64_t> g_req_id{0};

int main(int argc, char* argv[]) {
    int port       = 8085;
    int io_threads = 4;
    int lse_workers = 0;
    int heavy_pct  = 0;  // 默认 0% = 纯轻量 /hello 模式

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--port") && i+1 < argc) port        = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--io")  && i+1 < argc) io_threads  = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--lse") && i+1 < argc) lse_workers = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--heavy-pct") && i+1 < argc) heavy_pct = atoi(argv[++i]);
    }

    fprintf(stderr, "[bench] port=%d io=%d lse=%d heavy_pct=%d%%\n",
            port, io_threads, lse_workers, heavy_pct);

    HttpServer server(port);
    server.SetThreadCount(io_threads);
    if (lse_workers > 0) server.EnableLSE(lse_workers);

    // /hello 纯轻量端点（对比基线，不做任何额外计算）
    server.Get("/hello", [](const HttpRequest&, HttpResponse* rsp){
        rsp->SetContent("Hello, World!", "text/plain");
    });

    // /mixed 混合负载端点
    // 以 heavy_pct% 概率执行约 50ms 的 CPU 密集计算（"西瓜任务"）
    // 剩余 (100-heavy_pct)% 概率直接返回
    if (heavy_pct > 0) {
        server.Get("/mixed", [heavy_pct](const HttpRequest&, HttpResponse* rsp){
            uint64_t id = g_req_id.fetch_add(1, std::memory_order_relaxed);

            // 使用确定性伪随机：基于 request ID 判断是否命中重任务
            // 避免每次请求都构造真的 std::mt19937（构造有开销）
            bool is_heavy = ((id * 1103515245ULL + 12345) % 100) < static_cast<uint64_t>(heavy_pct);

            if (is_heavy) {
                // 执行约 50ms CPU 密集计算（触发 cache miss 的整数 hash chain）
                cpu_work(50000); // 50ms
                rsp->SetContent("Heavy computation done!", "text/plain");
            } else {
                rsp->SetContent("Lightweight OK!", "text/plain");
            }
        });
    }

    server.Listen();
    return 0;
}
