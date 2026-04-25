# 无锁任务窃取：时序与竞态分析

本图详细描述了 `lse_engine` 在高并发场景下，如何通过 **CAS (Compare-And-Swap)** 解决本地线程 (Owner) 与窃取线程 (Stealer) 之间的任务争抢。

```mermaid
sequenceDiagram
    autonumber
    
    %% 定义参与者
    participant Owner as 繁忙线程 (Owner)
    participant Deque as Chase-Lev 队列
    participant Stealer as 闲置线程 (Stealer)

    %% 顶部概要
    Note left of Owner: **生命周期概要**<br/>1. pushBottom: 任务入队<br/>2. popBottom: 本地执行<br/>3. Steal: 空核窃取

    %% 阶段一
    rect rgb(240, 249, 255)
    Note over Owner, Deque: 🟢 阶段一｜任务入队与本地执行（无竞争）
    Owner->>Deque: pushBottom() : 压入新任务
    Owner->>Deque: popBottom() : LIFO 弹出并执行
    Note right of Owner: 命中 L1 Cache，执行速度极快
    end

    %% 阶段二
    rect rgb(255, 241, 242)
    Note over Stealer, Deque: 🔴 阶段二｜任务窃取触发（潜在竞争）
    Stealer->>Deque: 读取 Top 索引 (Acquire 语义)
    Owner->>Deque: 操作 Bottom 索引 (Release 语义)

    alt 任务充足 (Top < Bottom)
        Stealer->>Deque: CAS 更新 Top 成功
        Deque-->>Stealer: 返回任务引用
        Stealer->>Stealer: 执行窃取任务 (Steal Success)
    else 仅剩最后一个任务 (Top == Bottom)
        Note over Owner, Stealer: 核心竞态区 (Owner 与 Stealer 同时发起原子抢夺)
        Stealer->>Deque: CAS 尝试更新 Top
        Owner->>Deque: CAS 尝试更新 Bottom
        Note over Deque: 硬件级仲裁：同一时刻仅一方 CAS 成功
        
        alt Stealer 抢到任务
            Deque-->>Stealer: 返回任务
            Deque-->>Owner: 返回 Empty
        else Owner 抢到任务
            Deque-->>Owner: 返回任务
            Deque-->>Stealer: 返回 Empty
        end
    end
    end

    %% 总结
    Note over Owner, Stealer: 最终一致性：所有任务仅执行一次，无重复、无丢失
