#include "http.hpp"

#define WWWROOT "./wwwroot/"

std::string RequestStr(const HttpRequest &req) {
    std::stringstream ss;
    ss << req._method << " " << req._path << " " << req._version << "\r\n";
    for (auto &it : req._params) {
        ss << it.first << ": " << it.second << "\r\n";
    }
    for (auto &it : req._headers) {
        ss << it.first << ": " << it.second << "\r\n";
    }
    ss << "\r\n";
    ss << req._body;
    return ss.str();
}
void Hello(const HttpRequest &req, HttpResponse *rsp) 
{
    rsp->SetContent(RequestStr(req), "text/plain");
}
void Login(const HttpRequest &req, HttpResponse *rsp) 
{
    rsp->SetContent(RequestStr(req), "text/plain");
}
void PutFile(const HttpRequest &req, HttpResponse *rsp) 
{
    std::string pathname = WWWROOT + req._path;
    Util::WriteFile(pathname, req._body);
}
void DelFile(const HttpRequest &req, HttpResponse *rsp) 
{
    rsp->SetContent(RequestStr(req), "text/plain");
}
int main()
{
    HttpServer server(8085);
    server.SetThreadCount(32);
    server.SetBaseDir(WWWROOT);//设置静态资源根目录，告诉服务器有静态资源请求到来，需要到哪里去找资源文件
    server.Get("/hello", Hello);
    server.Post("/login", Login);
    server.Put("/1234.txt", PutFile);
    server.Delete("/1234.txt", DelFile);
    server.Listen();
    return 0;
}