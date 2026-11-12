# CoroReplay v0.1 设计说明

## 1. 闭世界模型

CoroReplay 是被测程序显式接入的 C++20 单线程协程模拟器，不是操作系统线程调度器，也不是任意协程库的透明适配层。一个 scenario 的 setup 在全新 `Runtime` 中创建根任务与资源；每次 exploration run 都只使用该 Runtime 内的 `task`、`yield`、timer、`async_mutex` 和 `manual_event`。

所有协程切换都是协作式的。没有挂起点的业务死循环无法被 `max_decisions` 抢占。模拟器可以发现特定交错下的逻辑错误，但不模拟真实 data race、硬件内存序、真实 I/O 或 OS 调度。随机探索不是完备模型检查，`max_decisions` 耗尽只表示 `limit_exceeded`，不构活性证明。

## 2. task 状态机与所有权

### 2.1 未 attach

`task<T>` 为惰性、move-only，`initial_suspend` 为 `suspend_always`。此时 task 对象持有 coroutine handle；析构未启动或被移动后的 task 不会执行函数体。空 task 不能 attach，`task<T&>` 不支持。

### 2.2 attach 后状态

根任务由 `Runtime::spawn` attach，嵌套任务只能经右值 `co_await` attach。attach 时分配单调递增、不复用的 Operation ID，并记录 `operation_create`；子任务额外记录 `child_attach`。

状态转换如下：

```text
created -> ready -> running -> completed
                  ^  |          ^
                  |  | yield    | final_suspend 后由 Scheduler 完成
                  |  +----------+
                  |
                  +---- waiting ----> ready（资源唤醒，仍不直接 resume）
```

- `ready`：在按 Operation ID 排序的 Ready 集合中；只有 Scheduler 能选择并恢复。
- `running`：当前唯一正在执行 `resume()` 的 Operation。
- `waiting`：具有 `WaitKind::{child,mutex,event,timer}` 及稳定 wait-for 信息。
- `completed`：`resume()` 返回且 handle 已完成，Scheduler 读取结果/异常后统一完成收尾。

`final_suspend` 不分配、不销毁 frame，也不恢复 continuation。父任务仅保存 child Operation ID；child 完成后 Scheduler 把 parent 放入 Ready。`resume()` 期间 child attach 可能使 Operation vector 扩容，因此 Scheduler 返回后按 ID 重新取得 Operation，不跨 `resume()` 保留 vector 元素引用。

所有已 attach frame 保存到本次 Runtime 结束。`run()` 返回后禁止继续 spawn、创建资源、set/reset event 或追加 invariant，防止终止事件之后出现不可回放的 mutation。结果保留在各自 promise 的 `optional<T>` 中，通过类型擦除的地址回调取出并移动，因此支持 move-only、非默认构造 `T`，不经过要求可复制类型的 `std::any`。结果只能消费一次。异常由 child awaiter 重新抛出，parent 可捕获；未捕获 root 异常形成结构化终止。

Runtime teardown 先销毁全部 frame，再销毁 mutex/event 资源，覆盖 blocked task 的局部对象析构。该设计避免 final continuation 直接恢复造成的 UAF/double destroy。

## 3. 确定性调度规则

### 3.1 ID 与 Ready

Operation、Resource、Timer ID 均从 1 开始且不复用。语义记录不包含对象地址、`typeid.name()`、异常文本、源路径或无序容器遍历顺序。

Resume 决策前把 Ready 复制为升序 ID 列表。random policy 使用该有序列表做 bounded selection；即使列表只有一项，也生成显式决策并执行一次固定 RNG bounded selection。FIFO policy 是辅助测试策略，按每次入 Ready 的单调序号选择最早已就绪任务，但记录中的 Ready 列表仍按 ID 排序。

所有唤醒只改变状态并加入 Ready，不直接调用 `resume()`。`yield` 总是挂起并重新入队。

### 3.2 RNG 与 run seed

RNG 为 `std::mt19937_64`，版本字符串为 `mt19937_64-rejection-v1`。bounded selection 使用无符号 rejection sampling：

```text
threshold = (-bound mod 2^64) mod bound
重复 x = next_u64()，直到 x >= threshold
结果 = x mod bound
```

不使用实现可变的 `std::uniform_int_distribution`。run index 从 0 开始，effective seed 使用项目固定 SplitMix64 finalizer：

```text
x = base_seed + 0x9e3779b97f4a7c15 * (run_index + 1)  // uint64 wrap
x = (x xor (x >> 30)) * 0xbf58476d1ce4e5b9
x = (x xor (x >> 27)) * 0x94d049bb133111eb
seed = x xor (x >> 31)
```

测试包含 RNG、effective seed 的 golden vectors。Replay 不重新抽随机数。

### 3.3 虚拟时间

时钟为从 0 开始的 `uint64_t` 纳秒。timer 按 `(deadline, timer_id)` 排序；同一 deadline 的 timer 全按 ID 顺序 fire。只有 Ready 为空才产生 `AdvanceClock`，并推进到最早 deadline。零时长 sleep 也会挂起并产生显式零长度 AdvanceClock；过去 deadline 被钳到当前时间。

加法溢出形成 `virtual_time_overflow`，时钟不 wrap。负 duration 是 `UsageError`。

### 3.4 同步原语

`async_mutex` 非递归、单 owner、FIFO handoff。unlock 时若存在 waiter，队首立即成为逻辑 owner 并进入 Ready，同时更新其余 waiter 的 wait-for 为新 owner；它尚未 resume 时，其他 fast-path 也不能插队。自锁、非 owner unlock、跨 Runtime 使用抛出 `UsageError`。首版有意不提供跨挂起点的 RAII guard、取消或超时。

`manual_event` 是 manual-reset。set 放行当时全部 waiter；已发生的 Ready 不被之后的 reset 撤销。set-before-wait 走 fast path。generation 和 waiter ID 使用稳定语义记录。

### 3.5 终止与限制

Ready/timer 均为空且仍有 Waiting Operation 时生成等待现场。沿 child 和 mutex owner 的 wait-for 边检查环：有环为 `deadlock/wait_cycle`，无环为 `orphan_wait/orphan_wait`。event/timer 等待本身不虚构等待环。

`max_decisions`、`max_operations`、`max_trace_events` 分别限制调度决策、attach 数和事件缓冲。耗尽为 `limit_exceeded`，并记录具体限制 code，不构成活性证明。`trace_enabled=false` 或 `schedule_enabled=false` 仅用于执行/开销 benchmark；默认开启，artifact 与 strict replay 始终使用完整记录。

## 4. Schedule 与语义事件

每个 Resume decision 包含：

- `index`、`kind=resume`、当前/结束虚拟时间；
- selected Operation 和完整排序 Ready；
- 对应事件左闭右开范围 `[event_begin,event_end)`；
- 空 wakes 列表。

AdvanceClock decision 包含起止虚拟时间及 `{timer,operation}` wakes。setup 事件和 decision 外的终止事件用 `uint64_t` 最大值作为 `decision_index` sentinel。

事件统一字段为：`index,decision_index,time_ns,kind,operation,related_operation,resource,timer,value,value2,text`。kind 覆盖 create/resume/suspend/complete、child attach、ready、wait start/finish、mutex acquire/handoff/release、event set/reset、timer arm/fire、clock advance、invariant、failure。

`value/value2` 仅用于明确稳定的整数，例如 mutex owner 或 invariant 观测值；`text` 仅用于固定枚举名、显式 invariant label 和稳定 failure code。普通业务对象、异常 what、源路径不进入语义数据。

## 5. Artifact schema v1

`artifact.json` 顶层字段：

```text
schema_version
scheduler_semantics_version
scenario { id, semantic_version }
seed { base, effective, run_index }
rng_version
limits { max_decisions, max_operations, max_trace_events }
decisions[]
events[]
terminal { kind, code }
waiting[]
```

`waiting[]` 按 Operation ID 排列，保存 `operation,wait_kind,wait_for,resource,timer`。schema/scheduler semantics/scenario semantic version 共同决定兼容性。JSON 只存在于私有实现，公共头文件不依赖 nlohmann_json。

每次失败写独立目录，`artifact.json` 与 `trace.json` 来自同一 RunRecord。Explore 默认首个失败即停止；成功 run 不自动写 artifact，但公共类型允许保存成功记录用于 replay 测试。

## 6. 严格 Replay 与第一处分歧

Replay 先校验 schema、scheduler semantics、scenario ID/version、RNG version，然后用 artifact 的 effective seed 和 limits 创建新 Runtime，但不进行随机选择。

每个决策校验类型、index、Ready、selected、虚拟时间、timer wakes 和事件范围；Resume selected 还必须属于实际 Ready，并与对应的首个 `operation_resume` 一致。AdvanceClock 的 wakes 由实际 timer 状态重新生成，而不是照抄 expected。

事件生成时逐项比较稳定字段。发现差异立即停止调度，报告 phase、decision/event index、field、expected/actual、当前 Operation 状态、Resource owner/waiter 状态，以及最近最多 8 条共同事件。运行结束还检查缺失/额外 decisions、缺失/额外 events、terminal kind/code 和最终等待现场，因此“前缀相同但尾部不同”也不会被误判为成功。

Replay 返回 `ReplayReport`，不以打印文本代替结构化结果，也不写回或覆盖原 artifact。测试包含 selected/Ready、删除事件、mutex owner、截断/追加、terminal、版本和 waiting mutation，以及 checked-in deadlock golden artifact 的 100 次重复回放。

## 7. Chrome Trace / Perfetto

`trace.json` 直接遍历同一事件缓冲，为每条语义事件生成 Chrome Trace instant，并为 Operation 生成稳定 thread metadata。`ts` 使用 Chrome 所需的微秒数值，显示单位为 ns；artifact 中原始纳秒整数仍是严格比较的权威数据。没有第二套插桩或独立跟踪开关逻辑。CTest 使用 JSON 解析器检查结构，发布前还应在 Perfetto UI 人工导入检查实际展示。

## 8. Scenario 约束

setup 必须确定性且不能依赖 run 后已销毁的局部状态。需要跨 setup 生命周期共享的业务状态应由 coroutine frame 持有（例如按值捕获 `shared_ptr`），或放在其他保证覆盖整个 run 的所有权域中。惰性 task 的参数引用和捕获型 coroutine lambda 的闭包也必须活到 run/teardown 结束；临时字符串引用、临时捕获 lambda 都是未定义行为，具名协程配合按值参数更安全。资源必须属于当前 Runtime。跨 Runtime 调用通过 owning Runtime 和当前活动 Scheduler 双重检查。

随机 exploration 的覆盖能力依赖 scenario 暴露的挂起点；业务代码若自行引入系统时间、地址迭代、未固定 hash 顺序或外部非确定输入，就无法由 CoroReplay 保证跨平台语义一致。
