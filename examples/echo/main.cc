#include "echo.hpp"
#include <cstdlib>
int main(int argc, char* argv[]) {
    int t = argc > 1 ? std::atoi(argv[1]) : 1;
    EchoServer server(8500, t);
    server.Start();
    return 0;
}
