本项目采用 **One Loop Per Thread (Multi-Reactor)** 架构，并在此基础上引入了基于 **Chase-Lev Deque** 的无锁任务窃取引擎。

## 系统架构流转图

> **提示：** 本图描述了从端口接入到跨核负载均衡的完整路径。

```mermaid
graph TB
    %% --- 样式定义 (GitHub 兼容版) ---
    classDef network fill:#eef2ff,stroke:#4f46e5,stroke-width:2px;
    classDef compute fill:#f0fdf4,stroke:#16a34a,stroke-width:2px;
    classDef memory fill:#fff7ed,stroke:#ea580c,stroke-width:2px;
    classDef engine fill:#fef2f2,stroke:#dc2626,stroke-width:3px,stroke-dasharray:5 5;
    classDef client fill:#f5f3ff,stroke:#7c3aed,stroke-width:2px;
    classDef highlight stroke:#d32f2f,stroke-width:3px;

    %% --- 外部入口 ---
    Client((高并发客户端请求)):::client -->|① 建立 TCP 连接| Acceptor

    subgraph "网络接入层"
        Acceptor["Acceptor 接收线程<br/>分发新连接"]:::network
    end

    %% --- 负载分发 ---
    Acceptor -->|② 轮询分发| EP0
    Acceptor -->|② 轮询分发| EP1

    %% --- 核心 0：繁忙业务流 ---
    subgraph "CPU 核心 0 (高负载运行)"
        EP0["SubReactor 0<br/>监听 IO 事件"]:::network -->|③ 触发读取| Read0["零拷贝读取数据包"]:::network
        Read0 -->|④ pushBottom| Deque0[("Chase-Lev 无锁队列<br/>alignas 64 对齐")]:::memory
        Deque0 -->|⑤ popBottom| Exec0["执行业务回调"]:::compute
    end

    %% --- 核心 1：空闲与 Steal 逻辑 ---
    subgraph "CPU 核心 1 (空闲待命)"
        EP1["SubReactor 1"]:::network -.-> Check1{"检查队列"}
        Check1 -.->|空队列| LSE["任务窃取引擎 (LSE)"]:::engine
        
        %% 核心亮点：跨核窃取
        LSE == "⑥ popTop (CAS 窃取)" ===> Deque0:::highlight
        LSE -->|⑦ 执行任务| Exec1["跨核任务执行"]:::compute
    end

    %% --- 架构亮点批注 ---
    Note["架构亮点��<br/>1. 避免全局锁竞争<br/>2. LIFO 提升缓存局部性<br/>3. CAS 确保跨核安全<br/>4. 实现负载自平衡"]
    style Note fill:#f9fafb,stroke:#9ca3af,stroke-dasharray:4 4
