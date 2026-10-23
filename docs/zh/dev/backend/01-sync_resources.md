# 同步硬件资源

同步资源目录（resource catalog）记录 `a2a3` 和 `a5` 的资源域、ID 范围及编译器预留策略。
C++ 调用方使用 `pypto::sync::FindResourceCatalog`，Python 通过
`pypto.pypto_core.sync` 提供相同的查询。查询不会分配 ID、查找 ISA 安装目录或改变当前后端。

## 资源域

`PoolKey` 描述资源种类、核心作用域、有向源/目标流水线对和副本（replica）。
流水线事件池按 AIC/AIV 执行核心和方向相互独立。块内信号将执行核心别名规范化为
`CoreGroup`，并忽略流水线的具体写法。非流水线资源统一使用 `ALL/ALL`。
replica 是调用方提供的独立实例标识；查询同一个 replica 为零的能力原型，同时在结果中保留所请求的 replica。

查询接受某个 key，表示它对应资源模型中的一个域；这并不保证所指定的流水线或指令
都能在选定核心上执行。指令与核心的执行合法性仍由后端校验。

`PoolSpec` 区分硬件合法性与分配策略：

| 字段 | 含义 |
| ---- | ---- |
| `capacity` | 硬件 ID 的数值上界，不包含该值 |
| `legal_ids` | 受限合法集合；空列表表示 `[0, capacity)` |
| `reserved_ids` | 硬件合法、但自动分配策略应排除的 ID |
| `requires_contiguous_ids` | 目录对后续分配器提出的额外连续性要求 |

内置规则沿用 PTOAS 的资源模型：

| 资源 | A2/A3 容量 | A5 容量 |
| ---- | ---------- | ------- |
| 流水线事件，按核心与有向流水线对划分 | 8 | 8 |
| 块内信号，逻辑基 ID | 16 | 16 |
| 缓冲区信用（buffer credit） | 0 | 32 |

A5 的物理 `base + 16` 配对信号不会增加可独立分配的信号基 ID。容量为零表示该资源种类不可用。
`V -> S` 和 `S -> V` 的 ID 7 是编译器预留值，但对显式编写的 flag 仍然属于合法硬件 ID。

```python
from pypto.pypto_core import ir, sync

catalog = sync.find_resource_catalog("a5")
assert catalog is not None
pool = catalog.find_pool(
    sync.PoolKey(core=sync.CoreScope.AIV, src=ir.PipeType.V, dst=ir.PipeType.S)
)
assert pool is not None
assert pool.capacity == 8
assert pool.legal_ids == []
assert pool.reserved_ids == [7]
```

未知目标以及非法或未建模的资源池查询在 Python 中返回 `None`，在 C++ 中分别返回空指针或
`std::nullopt`。内置目录具有进程级生命周期。查询结果是独立快照，修改返回的列表或 key
不会改变目录。C++ 构造函数使用 `pypto::ValueError` 校验资源域、ID 集合及重复的规范化 key。

## 原生规则与所有权

`src/sync/resource_catalog.cpp` 中的少量 C++ 常量和初始化规则构造内置目录。
查询会校验并规范化 `PoolKey`，查找对应原型，然后返回保留所请求 replica 的 `PoolSpec` 快照。
目录拥有资源事实；每次编译的资源占用状态由调用方维护。Python 仅提供内置目录的查询，
不能构造或注册自定义目录。

容量与预留规则对应 PTOAS 中
`include/PTO/Transforms/InsertSync/SyncEventIdAllocation.h` 的
`kTotalEventIdNum`、`kBlockSyncSetWaitEventIdNum`，
`lib/PTO/Transforms/InsertSync/SyncEventIdAllocation.cpp` 的 V/S 预留规则，以及
`include/PTO/Transforms/InsertSync/UnifiedSyncModel.h` 的 `ResourceModel::bufidCapacity`。
修改这些资源事实时，应同步更新原生规则和目录测试。

此组件为后续同步工作提供资源事实，尚未包含宏注册表或资源分配器。
ISA 宏的隐藏占用、事件 ID 分配和资源压力修复均不属于当前接口的职责。
默认自动插同步路径继续使用 PTOAS。
