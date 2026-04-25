#ifndef __M_SERVER_H__
#define __M_SERVER_H__
#include <iostream>
#include <vector>
#include <string>
#include <cassert>
#include <cstring>
#include <ctime>
#include <functional>
#include <unordered_map>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <typeinfo>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include "../lse_engine/StealingEngine.h"

#define INF 0
#define DBG 1
#define ERR 2
#ifndef LOG_LEVEL
#define LOG_LEVEL DBG
#endif

#define LOG(level, format, ...) do{\
        if (level < LOG_LEVEL) break;\
        time_t t = time(NULL);\
        struct tm *ltm = localtime(&t);\
        char tmp[32] = {0};\
        strftime(tmp, 31, "%H:%M:%S", ltm);\
        fprintf(stdout, "[%p %s %s:%d] " format "\n", (void*)pthread_self(), tmp, __FILE__, __LINE__, ##__VA_ARGS__);\
    }while(0)

#define INF_LOG(format, ...) LOG(INF, format, ##__VA_ARGS__)
#define DBG_LOG(format, ...) LOG(DBG, format, ##__VA_ARGS__)
#define ERR_LOG(format, ...) LOG(ERR, format, ##__VA_ARGS__)

#define BUFFER_DEFAULT_SIZE 1024
class Buffer {
    private:
        std::vector<char> _buffer; //使用vector进行内存空间管理
        uint64_t _reader_idx; //读偏移
        uint64_t _writer_idx; //写偏移
    public:
        Buffer():_reader_idx(0), _writer_idx(0), _buffer(BUFFER_DEFAULT_SIZE){}
        char *Begin() { return &*_buffer.begin(); }
        //获取当前写入起始地址, _buffer的空间起始地址，加上写偏移量
        char *WritePosition() { return Begin() + _writer_idx; }
        //获取当前读取起始地址
        char *ReadPosition() { return Begin() + _reader_idx; }
        //获取缓冲区末尾空闲空间大小--写偏移之后的空闲空间, 总体空间大小减去写偏移
        uint64_t TailIdleSize() { return _buffer.size() - _writer_idx; }
        //获取缓冲区起始空闲空间大小--读偏移之前的空闲空间
        uint64_t HeadIdleSize() { return _reader_idx; }
        //获取可读数据大小 = 写偏移 - 读偏移
        uint64_t ReadAbleSize() { return _writer_idx - _reader_idx; }
        //将读偏移向后移动
        void MoveReadOffset(uint64_t len) { 
            if (len == 0) return; 
            //向后移动的大小，必须小于可读数据大小
            assert(len <= ReadAbleSize());
            _reader_idx += len;
        }
        //将写偏移向后移动 
        void MoveWriteOffset(uint64_t len) {
            //向后移动的大小，必须小于当前后边的空闲空间大小
            assert(len <= TailIdleSize());
            _writer_idx += len;
        }
        //确保可写空间足够（整体空闲空间够了就移动数据，否则就扩容）
        void EnsureWriteSpace(uint64_t len) {
            //如果末尾空闲空间大小足够，直接返回
            if (TailIdleSize() >= len) { return; }
            //末尾空闲空间不够，则判断加上起始位置的空闲空间大小是否足够, 够了就将数据移动到起始位置
            if (len <= TailIdleSize() + HeadIdleSize()) {
                //将数据移动到起始位置
                uint64_t rsz = ReadAbleSize();//把当前数据大小先保存起来
                std::copy(ReadPosition(), ReadPosition() + rsz, Begin());//把可读数据拷贝到起始位置
                _reader_idx = 0;    //将读偏移归0
                _writer_idx = rsz;  //将写位置置为可读数据大小， 因为当前的可读数据大小就是写偏移量
            }else {
                //总体空间不够，则需要扩容，不移动数据，直接给写偏移之后扩容足够空间即可
                DBG_LOG("RESIZE %ld", _writer_idx + len);
                _buffer.resize(_writer_idx + len);
            }
        } 
        //写入数据
        void Write(const void *data, uint64_t len) {
            //1. 保证有足够空间，2. 拷贝数据进去
            if (len == 0) return;
            EnsureWriteSpace(len);
            const char *d = (const char *)data;
            std::copy(d, d + len, WritePosition());
        }
        void WriteAndPush(const void *data, uint64_t len) {
            Write(data, len);
            MoveWriteOffset(len);
        }
        void WriteString(const std::string &data) {
            return Write(data.c_str(), data.size());
        }
        void WriteStringAndPush(const std::string &data) {
            WriteString(data);
            MoveWriteOffset(data.size());
        }
        void WriteBuffer(Buffer &data) {
            return Write(data.ReadPosition(), data.ReadAbleSize());
        }
        void WriteBufferAndPush(Buffer &data) { 
            WriteBuffer(data);
            MoveWriteOffset(data.ReadAbleSize());
        }
        //读取数据
        void Read(void *buf, uint64_t len) {
            //要求要获取的数据大小必须小于可读数据大小
            assert(len <= ReadAbleSize());
            std::copy(ReadPosition(), ReadPosition() + len, (char*)buf);
        }
        void ReadAndPop(void *buf, uint64_t len) {
            Read(buf, len);
            MoveReadOffset(len);
        }
        std::string ReadAsString(uint64_t len) {
            //要求要获取的数据大小必须小于可读数据大小
            assert(len <= ReadAbleSize());
            std::string str;
            str.resize(len);
            Read(&str[0], len);
            return str;
        }
        std::string ReadAsStringAndPop(uint64_t len) {
            assert(len <= ReadAbleSize());
            std::string str = ReadAsString(len);
            MoveReadOffset(len);
            return str;
        }
        char *FindCRLF() {
            char *res = (char*)memchr(ReadPosition(), '\n', ReadAbleSize());
            return res;
        }
        /*通常获取一行数据，这种情况针对是*/
        std::string GetLine() {
            char *pos = FindCRLF();
            if (pos == NULL) {
                return "";
            }
            // +1是为了把换行字符也取出来。
            return ReadAsString(pos - ReadPosition() + 1);
        }
        std::string GetLineAndPop() {
            std::string str = GetLine();
            MoveReadOffset(str.size());
            return str;
        }
        //清空缓冲区
        void Clear() {
            //只需要将偏移量归0即可
            _reader_idx = 0;
            _writer_idx = 0;
        }
};

#define MAX_LISTEN 1024
class Socket {
    private:
        int _sockfd;
    public:
        Socket():_sockfd(-1) {}
        Socket(int fd): _sockfd(fd) {}
        ~Socket() { Close(); }
        int Fd() { return _sockfd; }
        //创建套接字
        bool Create() {
            // int socket(int domain, int type, int protocol)
            _sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (_sockfd < 0) {
                ERR_LOG("CREATE SOCKET FAILED!!");
                return false;
            }
            return true;
        }
        //绑定地址信息
        bool Bind(const std::string &ip, uint16_t port) {
            struct sockaddr_in addr;
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            addr.sin_addr.s_addr = inet_addr(ip.c_str());
            socklen_t len = sizeof(struct sockaddr_in);
            // int bind(int sockfd, struct sockaddr*addr, socklen_t len);
            int ret = bind(_sockfd, (struct sockaddr*)&addr, len);
            if (ret < 0) {
                ERR_LOG("BIND ADDRESS FAILED!");
                return false;
            }
            return true;
        }
        //开始监听
        bool Listen(int backlog = MAX_LISTEN) {
            // int listen(int backlog)
            int ret = listen(_sockfd, backlog);
            if (ret < 0) {
                ERR_LOG("SOCKET LISTEN FAILED!");
                return false;
            }
            return true;
        }
        //向服务器发起连接
        bool Connect(const std::string &ip, uint16_t port) {
            struct sockaddr_in addr;
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            addr.sin_addr.s_addr = inet_addr(ip.c_str());
            socklen_t len = sizeof(struct sockaddr_in);
            // int connect(int sockfd, struct sockaddr*addr, socklen_t len);
            int ret = connect(_sockfd, (struct sockaddr*)&addr, len);
            if (ret < 0) {
                ERR_LOG("CONNECT SERVER FAILED!");
                return false;
            }
            return true;
        }
        //获取新连接
        int Accept() {
            // int accept(int sockfd, struct sockaddr *addr, socklen_t *len);
            int newfd = accept(_sockfd, NULL, NULL);
            if (newfd < 0) {
                ERR_LOG("SOCKET ACCEPT FAILED!");
                return -1;
            }
            return newfd;
        }
        //接收数据
        ssize_t Recv(void *buf, size_t len, int flag = 0) {
            // ssize_t recv(int sockfd, void *buf, size_t len, int flag);
            ssize_t ret = recv(_sockfd, buf, len, flag);
            if (ret <= 0) {
                //EAGAIN 当前socket的接收缓冲区中没有数据了，在非阻塞的情况下才会有这个错误
                //EINTR  表示当前socket的阻塞等待，被信号打断了，
                if (errno == EAGAIN || errno == EINTR) {
                    return 0;//表示这次接收没有接收到数据
                }
                ERR_LOG("SOCKET RECV FAILED!!");
                return -1;
            }
            return ret; //实际接收的数据长度
        }
        ssize_t NonBlockRecv(void *buf, size_t len) {
            return Recv(buf, len, MSG_DONTWAIT); // MSG_DONTWAIT 表示当前接收为非阻塞。
        }
        //发送数据
        ssize_t Send(const void *buf, size_t len, int flag = 0) {
            // ssize_t send(int sockfd, void *data, size_t len, int flag);
            ssize_t ret = send(_sockfd, buf, len, flag);
            if (ret < 0) {
                if (errno == EAGAIN || errno == EINTR) {
                    return 0;
                }
                ERR_LOG("SOCKET SEND FAILED!!");
                return -1;
            }
            return ret;//实际发送的数据长度
        }
        ssize_t NonBlockSend(void *buf, size_t len) {
            if (len == 0) return 0;
            return Send(buf, len, MSG_DONTWAIT); // MSG_DONTWAIT 表示当前发送为非阻塞。
        }
        //关闭套接字
        void Close() {
            if (_sockfd != -1) {
                close(_sockfd);
                _sockfd = -1;
            }
        }
        //创建一个服务端连接
        bool CreateServer(uint16_t port, const std::string &ip = "0.0.0.0", bool block_flag = false) {
            if (Create() == false) return false;
            ReuseAddress(); // 必须在 Bind 之前设置，否则 TIME_WAIT 状态下 bind 会失败
            if (block_flag) NonBlock();
            if (Bind(ip, port) == false) return false;
            if (Listen() == false) return false;
            return true;
        }
        //创建一个客户端连接
        bool CreateClient(uint16_t port, const std::string &ip) {
            //1. 创建套接字，2.指向连接服务器
            if (Create() == false) return false;
            if (Connect(ip, port) == false) return false;
            return true;
        }
        //设置套接字选项---开启地址端口重用
        void ReuseAddress() {
            // int setsockopt(int fd, int leve, int optname, void *val, int vallen)
            int val = 1;
            setsockopt(_sockfd, SOL_SOCKET, SO_REUSEADDR, (void*)&val, sizeof(int));
            val = 1;
            setsockopt(_sockfd, SOL_SOCKET, SO_REUSEPORT, (void*)&val, sizeof(int));
        }
        //设置套接字阻塞属性-- 设置为非阻塞
        void NonBlock() {
            //int fcntl(int fd, int cmd, ... /* arg */ );
            int flag = fcntl(_sockfd, F_GETFL, 0);
            fcntl(_sockfd, F_SETFL, flag | O_NONBLOCK);
        }
};

class Poller;
class EventLoop;
class Channel {
    private:
        int _fd;
        EventLoop *_loop;
        uint32_t _events;  // 当前需要监控的事件
        uint32_t _revents; // 当前连接触发的事件
        using EventCallback = std::function<void()>;
        EventCallback _read_callback;   //可读事件被触发的回调函数
        EventCallback _write_callback;  //可写事件被触发的回调函数
        EventCallback _error_callback;  //错误事件被触发的回调函数
        EventCallback _close_callback;  //连接断开事件被触发的回调函数
        EventCallback _event_callback;  //任意事件被触发的回调函数
    public:
        Channel(EventLoop *loop, int fd):_fd(fd), _events(0), _revents(0), _loop(loop) {}
        int Fd() { return _fd; }
        uint32_t Events() { return _events; }//获取想要监控的事件
        void SetREvents(uint32_t events) { _revents = events; }//设置实际就绪的事件
        void SetReadCallback(const EventCallback &cb) { _read_callback = cb; }
        void SetWriteCallback(const EventCallback &cb) { _write_callback = cb; }
        void SetErrorCallback(const EventCallback &cb) { _error_callback = cb; }
        void SetCloseCallback(const EventCallback &cb) { _close_callback = cb; }
        void SetEventCallback(const EventCallback &cb) { _event_callback = cb; }
        //当前是否监控了可读
        bool ReadAble() { return (_events & EPOLLIN); } 
        //当前是否监控了可写
        bool WriteAble() { return (_events & EPOLLOUT); }
        //启动读事件监控
        void EnableRead() { _events |= EPOLLIN; Update(); }
        //启动写事件监控
        void EnableWrite() { _events |= EPOLLOUT; Update(); }
        //关闭读事件监控
        void DisableRead() { _events &= ~EPOLLIN; Update(); }
        //关闭写事件监控
        void DisableWrite() { _events &= ~EPOLLOUT; Update(); }
        //关闭所有事件监控
        void DisableAll() { _events = 0; Update(); }
        //移除监控
        void Remove();
        void Update();
        //事件处理，一旦连接触发了事件，就调用这个函数，自己触发了什么事件如何处理自己决定
        void HandleEvent() {
            if ((_revents & EPOLLIN) || (_revents & EPOLLRDHUP) || (_revents & EPOLLPRI)) {
                /*不管任何事件，都调用的回调函数*/
                if (_read_callback) _read_callback();
            }
            /*有可能会释放连接的操作事件，一次只处理一个*/
            if (_revents & EPOLLOUT) {
                if (_write_callback) _write_callback();
            }else if (_revents & EPOLLERR) {
                if (_error_callback) _error_callback();//一旦出错，就会释放连接，因此要放到前边调用任意回调
            }else if (_revents & EPOLLHUP) {
                if (_close_callback) _close_callback();
            }
            if (_event_callback) _event_callback();
        }
};
#define MAX_EPOLLEVENTS 1024
class Poller {
    private:
        int _epfd;
        struct epoll_event _evs[MAX_EPOLLEVENTS];
        std::unordered_map<int, Channel *> _channels;
    private:
        //对epoll的直接操作
        void Update(Channel *channel, int op) {
            // int epoll_ctl(int epfd, int op,  int fd,  struct epoll_event *ev);
            int fd = channel->Fd();
            struct epoll_event ev;
            ev.data.fd = fd;
            ev.events = channel->Events();
            int ret = epoll_ctl(_epfd, op, fd, &ev);
            if (ret < 0) {
                ERR_LOG("EPOLLCTL FAILED!");
            }
            return;
        }
        //判断一个Channel是否已经添加了事件监控
        bool HasChannel(Channel *channel) {
            auto it = _channels.find(channel->Fd());
            if (it == _channels.end()) {
                return false;
            }
            return true;
        }
    public:
        Poller() {
            _epfd = epoll_create(MAX_EPOLLEVENTS);
            if (_epfd < 0) {
                ERR_LOG("EPOLL CREATE FAILED!!");
                abort();//退出程序
            }
        }
        //添加或修改监控事件
        void UpdateEvent(Channel *channel) {
            bool ret = HasChannel(channel);
            if (ret == false) {
                //不存在则添加
                _channels.insert(std::make_pair(channel->Fd(), channel));
                return Update(channel, EPOLL_CTL_ADD);
            }
            return Update(channel, EPOLL_CTL_MOD);
        }
        //移除监控
        void RemoveEvent(Channel *channel) {
            auto it = _channels.find(channel->Fd());
            if (it != _channels.end()) {
                _channels.erase(it);
            }
            Update(channel, EPOLL_CTL_DEL);
        }
        //开始监控，返回活跃连接
        void Poll(std::vector<Channel*> *active) {
            // int epoll_wait(int epfd, struct epoll_event *evs, int maxevents, int timeout)
            int nfds = epoll_wait(_epfd, _evs, MAX_EPOLLEVENTS, -1);
            if (nfds < 0) {
                if (errno == EINTR) {
                    return ;
                }
                ERR_LOG("EPOLL WAIT ERROR:%s\n", strerror(errno));
                abort();//退出程序
            }
            for (int i = 0; i < nfds; i++) {
                auto it = _channels.find(_evs[i].data.fd);
                assert(it != _channels.end());
                it->second->SetREvents(_evs[i].events);//设置实际就绪的事件
                active->push_back(it->second);
            }
            return;
        }
};


using TaskFunc = std::function<void()>;
using ReleaseFunc = std::function<void()>;
class TimerTask{
    private:
        uint64_t _id;       // 定时器任务对象ID
        uint32_t _timeout;  //定时任务的超时时间
        bool _canceled;     // false-表示没有被取消， true-表示被取消
        TaskFunc _task_cb;  //定时器对象要执行的定时任务
        ReleaseFunc _release; //用于删除TimerWheel中保存的定时器对象信息
    public:
        TimerTask(uint64_t id, uint32_t delay, const TaskFunc &cb): 
            _id(id), _timeout(delay), _task_cb(cb), _canceled(false) {}
        ~TimerTask() { 
            if (_canceled == false) _task_cb(); 
            _release(); 
        }
        void Cancel() { _canceled = true; }
        void SetRelease(const ReleaseFunc &cb) { _release = cb; }
        uint32_t DelayTime() { return _timeout; }
};

class TimerWheel {
    private:
        using WeakTask = std::weak_ptr<TimerTask>;
        using PtrTask = std::shared_ptr<TimerTask>;
        int _tick;      //当前的秒针，走到哪里释放哪里，释放哪里，就相当于执行哪里的任务
        int _capacity;  //表盘最大数量---其实就是最大延迟时间
        std::vector<std::vector<PtrTask>> _wheel;
        std::unordered_map<uint64_t, WeakTask> _timers;

        EventLoop *_loop;
        int _timerfd;//定时器描述符--可读事件回调就是读取计数器，执行定时任务
        std::unique_ptr<Channel> _timer_channel;
    private:
        void RemoveTimer(uint64_t id) {
            auto it = _timers.find(id);
            if (it != _timers.end()) {
                _timers.erase(it);
            }
        }
        static int CreateTimerfd() {
            int timerfd = timerfd_create(CLOCK_MONOTONIC, 0);
            if (timerfd < 0) {
                ERR_LOG("TIMERFD CREATE FAILED!");
                abort();
            }
            //int timerfd_settime(int fd, int flags, struct itimerspec *new, struct itimerspec *old);
            struct itimerspec itime;
            itime.it_value.tv_sec = 1;
            itime.it_value.tv_nsec = 0;//第一次超时时间为1s后
            itime.it_interval.tv_sec = 1; 
            itime.it_interval.tv_nsec = 0; //第一次超时后，每次超时的间隔时
            timerfd_settime(timerfd, 0, &itime, NULL);
            return timerfd;
        }
        int ReadTimefd() {
            uint64_t times;
            //有可能因为其他描述符的事件处理花费事件比较长，然后在处理定时器描述符事件的时候，有可能就已经超时了很多次
            //read读取到的数据times就是从上一次read之后超时的次数
            int ret = read(_timerfd, &times, 8);
            if (ret < 0) {
                ERR_LOG("READ TIMEFD FAILED!");
                abort();
            }
            return times;
        }
        //这个函数应该每秒钟被执行一次，相当于秒针向后走了一步
        void RunTimerTask() {
            _tick = (_tick + 1) % _capacity;
            _wheel[_tick].clear();//清空指定位置的数组，就会把数组中保存的所有管理定时器对象的shared_ptr释放掉
        }
        void OnTime() {
            //根据实际超时的次数，执行对应的超时任务
            int times = ReadTimefd();
            for (int i = 0; i < times; i++) {
                RunTimerTask();
            }
        }
        void TimerAddInLoop(uint64_t id, uint32_t delay, const TaskFunc &cb) {
            PtrTask pt(new TimerTask(id, delay, cb));
            pt->SetRelease(std::bind(&TimerWheel::RemoveTimer, this, id));
            int pos = (_tick + delay) % _capacity;
            _wheel[pos].push_back(pt);
            _timers[id] = WeakTask(pt);
        }
        void TimerRefreshInLoop(uint64_t id) {
            //通过保存的定时器对象的weak_ptr构造一个shared_ptr出来，添加到轮子中
            auto it = _timers.find(id);
            if (it == _timers.end()) {
                return;//没找着定时任务，没法刷新，没法延迟
            }
            PtrTask pt = it->second.lock();//lock获取weak_ptr管理的对象对应的shared_ptr
            int delay = pt->DelayTime();
            int pos = (_tick + delay) % _capacity;
            _wheel[pos].push_back(pt);
        }
        void TimerCancelInLoop(uint64_t id) {
            auto it = _timers.find(id);
            if (it == _timers.end()) {
                return;//没找着定时任务，没法刷新，没法延迟
            }
            PtrTask pt = it->second.lock();
            if (pt) pt->Cancel();
        }
    public:
        TimerWheel(EventLoop *loop):_capacity(60), _tick(0), _wheel(_capacity), _loop(loop), 
            _timerfd(CreateTimerfd()), _timer_channel(new Channel(_loop, _timerfd)) {
            _timer_channel->SetReadCallback(std::bind(&TimerWheel::OnTime, this));
            _timer_channel->EnableRead();//启动读事件监控
        }
        /*定时器中有个_timers成员，定时器信息的操作有可能在多线程中进行，因此需要考虑线程安全问题*/
        /*如果不想加锁，那就把对定期的所有操作，都放到一个线程中进行*/
        void TimerAdd(uint64_t id, uint32_t delay, const TaskFunc &cb);
        //刷新/延迟定时任务
        void TimerRefresh(uint64_t id);
        void TimerCancel(uint64_t id);
        /*这个接口存在线程安全问题--这个接口实际上不能被外界使用者调用，只能在模块内，在对应的EventLoop线程内执行*/
        bool HasTimer(uint64_t id) {
            auto it = _timers.find(id);
            if (it == _timers.end()) {
                return false;
            }
            return true;
        }
};

class EventLoop {
    private:
        using Functor = std::function<void()>;
        std::thread::id _thread_id;//线程ID
        int _event_fd;//eventfd唤醒IO事件监控有可能导致的阻塞
        std::unique_ptr<Channel> _event_channel;
        Poller _poller;//进行所有描述符的事件监控
        std::vector<Functor> _tasks;//任务池
        std::mutex _mutex;//实现任务池操作的线程安全
        TimerWheel _timer_wheel;//定时器模块
    public:
        //执行任务池中的所有任务
        void RunAllTask() {
            std::vector<Functor> functor;
            {
                std::unique_lock<std::mutex> _lock(_mutex);
                _tasks.swap(functor);
            }
            for (auto &f : functor) {
                f();
            }
            return ;
        }
        static int CreateEventFd() {
            int efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
            if (efd < 0) {
                ERR_LOG("CREATE EVENTFD FAILED!!");
                abort();//让程序异常退出
            }
            return efd;
        }
        void ReadEventfd() {
            uint64_t res = 0;
            int ret = read(_event_fd, &res, sizeof(res));
            if (ret < 0) {
                //EINTR -- 被信号打断；   EAGAIN -- 表示无数据可读
                if (errno == EINTR || errno == EAGAIN) {
                    return;
                }
                ERR_LOG("READ EVENTFD FAILED!");
                abort();
            }
            return ;
        }
        void WeakUpEventFd() {
            uint64_t val = 1;
            int ret = write(_event_fd, &val, sizeof(val));
            if (ret < 0) {
                if (errno == EINTR) {
                    return;
                }
                ERR_LOG("READ EVENTFD FAILED!");
                abort();
            }
            return ;
        }
    public:
        EventLoop():_thread_id(std::this_thread::get_id()), 
                    _event_fd(CreateEventFd()), 
                    _event_channel(new Channel(this, _event_fd)),
                    _timer_wheel(this) {
            //给eventfd添加可读事件回调函数，读取eventfd事件通知次数
            _event_channel->SetReadCallback(std::bind(&EventLoop::ReadEventfd, this));
            //启动eventfd的读事件监控
            _event_channel->EnableRead();
        }
        //三步走--事件监控-》就绪事件处理-》执行任务
        void Start() {
            while(1) {
                //1. 事件监控， 
                std::vector<Channel *> actives;
                _poller.Poll(&actives);
                //2. 事件处理。 
                for (auto &channel : actives) {
                    channel->HandleEvent();
                }
                //3. 执行任务
                RunAllTask();
            }
        }
        //用于判断当前线程是否是EventLoop对应的线程；
        bool IsInLoop() {
            return (_thread_id == std::this_thread::get_id());
        }
        void AssertInLoop() {
            assert(_thread_id == std::this_thread::get_id());
        }
        //判断将要执行的任务是否处于当前线程中，如果是则执行，不是则压入队列。
        void RunInLoop(const Functor &cb) {
            if (IsInLoop()) {
                return cb();
            }
            return QueueInLoop(cb);
        }
        //将操作压入任务池
        void QueueInLoop(const Functor &cb) {
            {
                std::unique_lock<std::mutex> _lock(_mutex);
                _tasks.push_back(cb);
            }
            //唤醒有可能因为没有事件就绪，而导致的epoll阻塞；
            //其实就是给eventfd写入一个数据，eventfd就会触发可读事件
            WeakUpEventFd();
        }
        //添加/修改描述符的事件监控
        void UpdateEvent(Channel *channel) { return _poller.UpdateEvent(channel); }
        //移除描述符的监控
        void RemoveEvent(Channel *channel) { return _poller.RemoveEvent(channel); }
        void TimerAdd(uint64_t id, uint32_t delay, const TaskFunc &cb) { return _timer_wheel.TimerAdd(id, delay, cb); }
        void TimerRefresh(uint64_t id) { return _timer_wheel.TimerRefresh(id); }
        void TimerCancel(uint64_t id) { return _timer_wheel.TimerCancel(id); }
        bool HasTimer(uint64_t id) { return _timer_wheel.HasTimer(id); }
};
class LoopThread {
    private:
        /*用于实现_loop获取的同步关系，避免线程创建了，但是_loop还没有实例化之前去获取_loop*/
        std::mutex _mutex;          // 互斥锁
        std::condition_variable _cond;   // 条件变量
        EventLoop *_loop;       // EventLoop指针变量，这个对象需要在线程内实例化
        std::thread _thread;    // EventLoop对应的线程
    private:
        /*实例化 EventLoop 对象，唤醒_cond上有可能阻塞的线程，并且开始运行EventLoop模块的功能*/
        void ThreadEntry() {
            EventLoop loop;
            {
                std::unique_lock<std::mutex> lock(_mutex);//加锁
                _loop = &loop;
                _cond.notify_all();
            }
            loop.Start();
        }
    public:
        /*创建线程，设定线程入口函数*/
        LoopThread():_loop(NULL), _thread(std::thread(&LoopThread::ThreadEntry, this)) {}
        /*返回当前线程关联的EventLoop对象指针*/
        EventLoop *GetLoop() {
            EventLoop *loop = NULL;
            {
                std::unique_lock<std::mutex> lock(_mutex);//加锁
                _cond.wait(lock, [&](){ return _loop != NULL; });//loop为NULL就一直阻塞
                loop = _loop;
            }
            return loop;
        }
};

class LoopThreadPool {
    private:
        int _thread_count;
        int _next_idx;
        EventLoop *_baseloop;
        std::vector<LoopThread*> _threads;
        std::vector<EventLoop *> _loops;
    public:
        LoopThreadPool(EventLoop *baseloop):_thread_count(0), _next_idx(0), _baseloop(baseloop) {}
        void SetThreadCount(int count) { _thread_count = count; }
        void Create() {
            if (_thread_count > 0) {
                _threads.resize(_thread_count);
                _loops.resize(_thread_count);
                for (int i = 0; i < _thread_count; i++) {
                    _threads[i] = new LoopThread();
                    _loops[i] = _threads[i]->GetLoop();
                }
            }
            return ;
        }
        EventLoop *NextLoop() {
            if (_thread_count == 0) {
                return _baseloop;
            }
            _next_idx = (_next_idx + 1) % _thread_count;
            return _loops[_next_idx];
        }
};


class Any{
    private:
        class holder {
            public:
                virtual ~holder() {}
                virtual const std::type_info& type() = 0;
                virtual holder *clone() = 0;
        };
        template<class T>
        class placeholder: public holder {
            public:
                placeholder(const T &val): _val(val) {}
                // 获取子类对象保存的数据类型
                virtual const std::type_info& type() { return typeid(T); }
                // 针对当前的对象自身，克隆出一个新的子类对象
                virtual holder *clone() { return new placeholder(_val); }
            public:
                T _val;
        };
        holder *_content;
    public:
        Any():_content(NULL) {}
        template<class T>
        Any(const T &val):_content(new placeholder<T>(val)) {}
        Any(const Any &other):_content(other._content ? other._content->clone() : NULL) {}
        ~Any() { delete _content; }

        Any &swap(Any &other) {
            std::swap(_content, other._content);
            return *this;
        }

        // 返回子类对象保存的数据的指针
        template<class T>
        T *get() {
            //想要获取的数据类型，必须和保存的数据类型一致
            assert(typeid(T) == _content->type());
            return &((placeholder<T>*)_content)->_val;
        }
        //赋值运算符的重载函数
        template<class T>
        Any& operator=(const T &val) {
            //为val构造一个临时的通用容器，然后与当前容器自身进行指针交换，临时对象释放的时候，原先保存的数据也就被释放
            Any(val).swap(*this);
            return *this;
        }
        Any& operator=(const Any &other) {
            Any(other).swap(*this);
            return *this;
        }
};

class Connection;
//DISCONECTED -- 连接关闭状态；   CONNECTING -- 连接建立成功-待处理状态
//CONNECTED -- 连接建立完成，各种设置已完成，可以通信的状态；  DISCONNECTING -- 待关闭状态
typedef enum { DISCONNECTED, CONNECTING, CONNECTED, DISCONNECTING}ConnStatu;
using PtrConnection = std::shared_ptr<Connection>;
class Connection : public std::enable_shared_from_this<Connection> {
    private:
        uint64_t _conn_id;  // 连接的唯一ID，便于连接的管理和查找
        //uint64_t _timer_id;   //定时器ID，必须是唯一的，这块为了简化操作使用conn_id作为定时器ID
        int _sockfd;        // 连接关联的文件描述符
        bool _enable_inactive_release;  // 连接是否启动非活跃销毁的判断标志，默认为false
        EventLoop *_loop;   // 连接所关联的一个EventLoop
        ConnStatu _statu;   // 连接状态
        Socket _socket;     // 套接字操作管理
        Channel _channel;   // 连接的事件管理
        Buffer _in_buffer;  // 输入缓冲区---存放从socket中读取到的数据
        Buffer _out_buffer; // 输出缓冲区---存放要发送给对端的数据
        Any _context;       // 请求的接收处理上下文

        /*这四个回调函数，是让服务器模块来设置的（其实服务器模块的处理回调也是组件使用者设置的）*/
        /*换句话说，这几个回调都是组件使用者使用的*/
        using ConnectedCallback = std::function<void(const PtrConnection&)>;
        using MessageCallback = std::function<void(const PtrConnection&, Buffer *)>;
        using ClosedCallback = std::function<void(const PtrConnection&)>;
        using AnyEventCallback = std::function<void(const PtrConnection&)>;
        ConnectedCallback _connected_callback;
        MessageCallback _message_callback;
        ClosedCallback _closed_callback;
        AnyEventCallback _event_callback;
        /*组件内的连接关闭回调--组件内设置的，因为服务器组件内会把所有的连接管理起来，一旦某个连接要关闭*/
        /*就应该从管理的地方移除掉自己的信息*/
        ClosedCallback _server_closed_callback;
        lse::StealingEngine* _lse_engine = nullptr; // non-owning; set by TcpServer when LSE is enabled
    private:
        /*五个channel的事件回调函数*/
        //描述符可读事件触发后调用的函数，接收socket数据放到接收缓冲区中，然后调用_message_callback
        void HandleRead() {
            //1. 接收socket的数据，放到缓冲区
            char buf[65536];
            ssize_t ret = _socket.NonBlockRecv(buf, 65535);
            if (ret < 0) {
                //出错了,不能直接关闭连接
                return ShutdownInLoop();
            }
            //这里的等于0表示的是没有读取到数据，而并不是连接断开了，连接断开返回的是-1
            //将数据放入输入缓冲区,写入之后顺便将写偏移向后移动
            _in_buffer.WriteAndPush(buf, ret);
            //2. 调用message_callback进行业务处理
            if (_in_buffer.ReadAbleSize() > 0) {
                if (_lse_engine) {
                    // LSE 路径：把缓冲区快照移交给 StealingEngine worker，
                    // EventLoop 线程立即释放，不再持有 _in_buffer 中的这批数据
                    Buffer snap;
                    snap.WriteAndPush(_in_buffer.ReadPosition(), _in_buffer.ReadAbleSize());
                    _in_buffer.MoveReadOffset(_in_buffer.ReadAbleSize());
                    auto conn = shared_from_this();
                    _lse_engine->submit([this, conn, snap = std::move(snap)]() mutable {
                        if (_message_callback) _message_callback(conn, &snap);
                    });
                } else {
                    //shared_from_this--从当前对象自身获取自身的shared_ptr管理对象
                    return _message_callback(shared_from_this(), &_in_buffer);
                }
            }
        }
        //描述符可写事件触发后调用的函数，将发送缓冲区中的数据进行发送
        void HandleWrite() {
            //_out_buffer中保存的数据就是要发送的数据
            ssize_t ret = _socket.NonBlockSend(_out_buffer.ReadPosition(), _out_buffer.ReadAbleSize());
            if (ret < 0) {
                //发送错误就该关闭连接了，
                if (_in_buffer.ReadAbleSize() > 0) {
                    _message_callback(shared_from_this(), &_in_buffer);
                }
                return Release();//这时候就是实际的关闭释放操作了。
            }
            _out_buffer.MoveReadOffset(ret);//千万不要忘了，将读偏移向后移动
            if (_out_buffer.ReadAbleSize() == 0) {
                _channel.DisableWrite();// 没有数据待发送了，关闭写事件监控
                //如果当前是连接待关闭状态，则有数据，发送完数据释放连接，没有数据则直接释放
                if (_statu == DISCONNECTING) {
                    return Release();
                }
            }
            return;
        }
        //描述符触发挂断事件
        void HandleClose() {
            /*一旦连接挂断了，套接字就什么都干不了了，因此有数据待处理就处理一下，完毕关闭连接*/
            if (_in_buffer.ReadAbleSize() > 0) {
                _message_callback(shared_from_this(), &_in_buffer);
            }
            return Release();
        }
        //描述符触发出错事件
        void HandleError() {
            return HandleClose();
        }
        //描述符触发任意事件: 1. 刷新连接的活跃度--延迟定时销毁任务；  2. 调用组件使用者的任意事件回调
        void HandleEvent() {
            if (_enable_inactive_release == true)  {  _loop->TimerRefresh(_conn_id); }
            if (_event_callback)  {  _event_callback(shared_from_this()); }
        }
        //连接获取之后，所处的状态下要进行各种设置（启动读监控,调用回调函数）
        void EstablishedInLoop() {
            // 1. 修改连接状态；  2. 启动读事件监控；  3. 调用回调函数
            assert(_statu == CONNECTING);//当前的状态必须一定是上层的半连接状态
            _statu = CONNECTED;//当前函数执行完毕，则连接进入已完成连接状态
            // 一旦启动读事件监控就有可能会立即触发读事件，如果这时候启动了非活跃连接销毁
            _channel.EnableRead();
            if (_connected_callback) _connected_callback(shared_from_this());
        }
        //这个接口才是实际的释放接口
        void ReleaseInLoop() {
            //1. 修改连接状态，将其置为DISCONNECTED
            _statu = DISCONNECTED;
            //2. 移除连接的事件监控
            _channel.Remove();
            //3. 关闭描述符
            _socket.Close();
            //4. 如果当前定时器队列中还有定时销毁任务，则取消任务
            if (_loop->HasTimer(_conn_id)) CancelInactiveReleaseInLoop();
            //5. 调用关闭回调函数，避免先移除服务器管理的连接信息导致Connection被释放，再去处理会出错，因此先调用用户的回调函数
            if (_closed_callback) _closed_callback(shared_from_this());
            //移除服务器内部管理的连接信息
            if (_server_closed_callback) _server_closed_callback(shared_from_this());
        }
        //这个接口并不是实际的发送接口，而只是把数据放到了发送缓冲区，启动了可写事件监控
        void SendInLoop(Buffer &buf) {
            if (_statu == DISCONNECTED) return ;
            _out_buffer.WriteBufferAndPush(buf);
            if (_channel.WriteAble() == false) {
                _channel.EnableWrite();
            }
        }
        //这个关闭操作并非实际的连接释放操作，需要判断还有没有数据待处理，待发送
        void ShutdownInLoop() {
            _statu = DISCONNECTING;// 设置连接为半关闭状态
            if (_in_buffer.ReadAbleSize() > 0) {
                if (_message_callback) _message_callback(shared_from_this(), &_in_buffer);
            }
            //要么就是写入数据的时候出错关闭，要么就是没有待发送数据，直接关闭
            if (_out_buffer.ReadAbleSize() > 0) {
                if (_channel.WriteAble() == false) {
                    _channel.EnableWrite();
                }
            }
            if (_out_buffer.ReadAbleSize() == 0) {
                Release();
            }
        }
        //启动非活跃连接超时释放规则
        void EnableInactiveReleaseInLoop(int sec) {
            //1. 将判断标志 _enable_inactive_release 置为true
            _enable_inactive_release = true;
            //2. 如果当前定时销毁任务已经存在，那就刷新延迟一下即可
            if (_loop->HasTimer(_conn_id)) {
                return _loop->TimerRefresh(_conn_id);
            }
            //3. 如果不存在定时销毁任务，则新增
            _loop->TimerAdd(_conn_id, sec, std::bind(&Connection::Release, shared_from_this()));
        }
        void CancelInactiveReleaseInLoop() {
            _enable_inactive_release = false;
            if (_loop->HasTimer(_conn_id)) { 
                _loop->TimerCancel(_conn_id); 
            }
        }
        void UpgradeInLoop(const Any &context, 
                    const ConnectedCallback &conn, 
                    const MessageCallback &msg, 
                    const ClosedCallback &closed, 
                    const AnyEventCallback &event) {
            _context = context;
            _connected_callback = conn;
            _message_callback = msg;
            _closed_callback = closed;
            _event_callback = event;
        }
    public:
        Connection(EventLoop *loop, uint64_t conn_id, int sockfd):_conn_id(conn_id), _sockfd(sockfd),
            _enable_inactive_release(false), _loop(loop), _statu(CONNECTING), _socket(_sockfd),
            _channel(loop, _sockfd) {
            _channel.SetCloseCallback(std::bind(&Connection::HandleClose, this));
            _channel.SetEventCallback(std::bind(&Connection::HandleEvent, this));
            _channel.SetReadCallback(std::bind(&Connection::HandleRead, this));
            _channel.SetWriteCallback(std::bind(&Connection::HandleWrite, this));
            _channel.SetErrorCallback(std::bind(&Connection::HandleError, this));
        }
        ~Connection() { DBG_LOG("RELEASE CONNECTION:%p", this); }
        //获取管理的文件描述符
        int Fd() { return _sockfd; }
        //获取连接ID
        int Id() { return _conn_id; }
        //是否处于CONNECTED状态
        bool Connected() { return (_statu == CONNECTED); }
        //设置上下文--连接建立完成时进行调用
        void SetContext(const Any &context) { _context = context; }
        //获取上下文，返回的是指针
        Any *GetContext() { return &_context; }
        void SetConnectedCallback(const ConnectedCallback&cb) { _connected_callback = cb; }
        void SetMessageCallback(const MessageCallback&cb) { _message_callback = cb; }
        void SetClosedCallback(const ClosedCallback&cb) { _closed_callback = cb; }
        void SetAnyEventCallback(const AnyEventCallback&cb) { _event_callback = cb; }
        void SetSrvClosedCallback(const ClosedCallback&cb) { _server_closed_callback = cb; }
        void SetLSE(lse::StealingEngine* lse) { _lse_engine = lse; }
        //连接建立就绪后，进行channel回调设置，启动读监控，调用_connected_callback
        void Established() {
            _loop->RunInLoop(std::bind(&Connection::EstablishedInLoop, this));
        }
        //发送数据，将数据放到发送缓冲区，启动写事件监控
        void Send(const char *data, size_t len) {
            Buffer buf;
            buf.WriteAndPush(data, len);
            _loop->RunInLoop(std::bind(&Connection::SendInLoop, shared_from_this(), std::move(buf)));
        }
        void Shutdown() {
            _loop->RunInLoop(std::bind(&Connection::ShutdownInLoop, shared_from_this()));
        }
        void Release() {
            _loop->QueueInLoop(std::bind(&Connection::ReleaseInLoop, shared_from_this()));
        }
        //启动非活跃销毁，并定义多长时间无通信就是非活跃，添加定时任务
        void EnableInactiveRelease(int sec) {
            _loop->RunInLoop(std::bind(&Connection::EnableInactiveReleaseInLoop, this, sec));
        }
        //取消非活跃销毁
        void CancelInactiveRelease() {
            _loop->RunInLoop(std::bind(&Connection::CancelInactiveReleaseInLoop, this));
        }
        //切换协议---重置上下文以及阶段性回调处理函数 -- 而是这个接口必须在EventLoop线程中立即执行
        //防备新的事件触发后，处理的时候，切换任务还没有被执行--会导致数据使用原协议处理了。
        void Upgrade(const Any &context, const ConnectedCallback &conn, const MessageCallback &msg, 
                     const ClosedCallback &closed, const AnyEventCallback &event) {
            _loop->AssertInLoop();
            _loop->RunInLoop(std::bind(&Connection::UpgradeInLoop, this, context, conn, msg, closed, event));
        }
};

class Acceptor {
    private:
        Socket _socket;//用于创建监听套接字
        EventLoop *_loop; //用于对监听套接字进行事件监控
        Channel _channel; //用于对监听套接字进行事件管理

        using AcceptCallback = std::function<void(int)>;
        AcceptCallback _accept_callback;
    private:
        /*监听套接字的读事件回调处理函数---获取新连接，调用_accept_callback函数进行新连接处理*/
        void HandleRead() {
            int newfd = _socket.Accept();
            if (newfd < 0) {
                return ;
            }
            if (_accept_callback) _accept_callback(newfd);
        }
        int CreateServer(int port) {
            bool ret = _socket.CreateServer(port);
            assert(ret == true);
            return _socket.Fd();
        }
    public:
        /*不能将启动读事件监控，放到构造函数中，必须在设置回调函数后，再去启动*/
        /*否则有可能造成启动监控后，立即有事件，处理的时候，回调函数还没设置：新连接得不到处理，且资源泄漏*/
        Acceptor(EventLoop *loop, int port): _socket(CreateServer(port)), _loop(loop), 
            _channel(loop, _socket.Fd()) {
            _channel.SetReadCallback(std::bind(&Acceptor::HandleRead, this));
        }
        void SetAcceptCallback(const AcceptCallback &cb) { _accept_callback = cb; }
        void Listen() { _channel.EnableRead(); }
};

class TcpServer {
    private:
        uint64_t _next_id;      //这是一个自动增长的连接ID，
        int _port;
        int _timeout;           //这是非活跃连接的统计时间---多长时间无通信就是非活跃连接
        bool _enable_inactive_release;//是否启动了非活跃连接超时销毁的判断标志
        EventLoop _baseloop;    //这是主线程的EventLoop对象，负责监听事件的处理
        Acceptor _acceptor;    //这是监听套接字的管理对象
        LoopThreadPool _pool;   //这是从属EventLoop线程池
        std::unordered_map<uint64_t, PtrConnection> _conns;//保存管理所有连接对应的shared_ptr对象
        std::unique_ptr<lse::StealingEngine> _lse_engine; // optional; null = baseline mode

        using ConnectedCallback = std::function<void(const PtrConnection&)>;
        using MessageCallback = std::function<void(const PtrConnection&, Buffer *)>;
        using ClosedCallback = std::function<void(const PtrConnection&)>;
        using AnyEventCallback = std::function<void(const PtrConnection&)>;
        using Functor = std::function<void()>;
        ConnectedCallback _connected_callback;
        MessageCallback _message_callback;
        ClosedCallback _closed_callback;
        AnyEventCallback _event_callback;
    private:
        void RunAfterInLoop(const Functor &task, int delay) {
            _next_id++;
            _baseloop.TimerAdd(_next_id, delay, task);
        }
        //为新连接构造一个Connection进行管理
        void NewConnection(int fd) {
            _next_id++;
            PtrConnection conn(new Connection(_pool.NextLoop(), _next_id, fd));
            conn->SetMessageCallback(_message_callback);
            conn->SetClosedCallback(_closed_callback);
            conn->SetConnectedCallback(_connected_callback);
            conn->SetAnyEventCallback(_event_callback);
            conn->SetSrvClosedCallback(std::bind(&TcpServer::RemoveConnection, this, std::placeholders::_1));
            if (_lse_engine) conn->SetLSE(_lse_engine.get());
            if (_enable_inactive_release) conn->EnableInactiveRelease(_timeout);//启动非活跃超时销毁
            conn->Established();//就绪初始化
            _conns.insert(std::make_pair(_next_id, conn));
        }
        void RemoveConnectionInLoop(const PtrConnection &conn) {
            int id = conn->Id();
            auto it = _conns.find(id);
            if (it != _conns.end()) {
                _conns.erase(it);
            }
        }
        //从管理Connection的_conns中移除连接信息
        void RemoveConnection(const PtrConnection &conn) {
            _baseloop.RunInLoop(std::bind(&TcpServer::RemoveConnectionInLoop, this, conn));
        }
    public:
        TcpServer(int port):
            _port(port), 
            _next_id(0), 
            _enable_inactive_release(false), 
            _acceptor(&_baseloop, port),
            _pool(&_baseloop) {
            _acceptor.SetAcceptCallback(std::bind(&TcpServer::NewConnection, this, std::placeholders::_1));
            _acceptor.Listen();//将监听套接字挂到baseloop上
        }
        void SetThreadCount(int count) { return _pool.SetThreadCount(count); }
        void SetConnectedCallback(const ConnectedCallback&cb) { _connected_callback = cb; }
        void SetMessageCallback(const MessageCallback&cb) { _message_callback = cb; }
        void SetClosedCallback(const ClosedCallback&cb) { _closed_callback = cb; }
        void SetAnyEventCallback(const AnyEventCallback&cb) { _event_callback = cb; }
        void EnableInactiveRelease(int timeout) { _timeout = timeout; _enable_inactive_release = true; }
        //用于添加一个定时任务
        void RunAfter(const Functor &task, int delay) {
            _baseloop.RunInLoop(std::bind(&TcpServer::RunAfterInLoop, this, task, delay));
        }
        // 启用 LSE：message callback 将在 StealingEngine worker 线程中执行，而非 EventLoop 线程
        void EnableLSE(int workers) {
            _lse_engine = std::make_unique<lse::StealingEngine>(workers);
        }
        void Start() { _pool.Create(); if (_lse_engine) _lse_engine->start(); _baseloop.Start(); }
};


void Channel::Remove() { return _loop->RemoveEvent(this); }
void Channel::Update() { return _loop->UpdateEvent(this); }
void TimerWheel::TimerAdd(uint64_t id, uint32_t delay, const TaskFunc &cb) {
    _loop->RunInLoop(std::bind(&TimerWheel::TimerAddInLoop, this, id, delay, cb));
}
//刷新/延迟定时任务
void TimerWheel::TimerRefresh(uint64_t id) {
    _loop->RunInLoop(std::bind(&TimerWheel::TimerRefreshInLoop, this, id));
}
void TimerWheel::TimerCancel(uint64_t id) {
    _loop->RunInLoop(std::bind(&TimerWheel::TimerCancelInLoop, this, id));
}


class NetWork {
    public:
        NetWork() {
            DBG_LOG("SIGPIPE INIT");
            signal(SIGPIPE, SIG_IGN);
        }
};
static NetWork nw;
#endif