# Synchronization hardware resources

The synchronization resource catalog records resource domains, ID ranges, and
compiler reservation policy for `a2a3` and `a5`. Native callers use
`pypto::sync::FindResourceCatalog`; Python exposes the same queries through
`pypto.pypto_core.sync`. Queries do not allocate IDs, discover an ISA installation,
or change the active backend.

## Resource domains

`PoolKey` names a resource family, core scope, directed source/destination pipe
pair, and replica. Pipeline event banks are independent for each AIC/AIV executor
and direction. Intra-block signals canonicalize executor aliases to `CoreGroup`
and ignore pipe spelling. Non-pipeline resources use canonical `ALL/ALL` pipes.
Replica is a caller-supplied independent-instance identity; queries preserve it
while consulting the same replica-zero capability prototype.

An accepted key identifies a domain in the resource model. It does not certify
that each named pipe or instruction can execute on the selected core. The backend
remains responsible for instruction and core legality.

`PoolSpec` separates hardware legality from allocation policy:

| Field | Meaning |
| ----- | ------- |
| `capacity` | Exclusive numeric upper bound for hardware IDs |
| `legal_ids` | Restricted legal set; an empty list means `[0, capacity)` |
| `reserved_ids` | Hardware-legal IDs excluded by automatic allocation policy |
| `requires_contiguous_ids` | Contiguous IDs required for a future allocation |

The built-in rules follow the PTOAS resource model:

| Resource | A2/A3 capacity | A5 capacity |
| -------- | -------------- | ----------- |
| Pipeline event, per core and directed pipe pair | 8 | 8 |
| Intra-block signal, logical base IDs | 16 | 16 |
| Buffer credit | 0 | 32 |

On A5, the physical `base + 16` companion does not create an independently
allocatable signal base. A zero capacity records an unavailable resource family.
For `V -> S` and `S -> V`, ID 7 is compiler-reserved but remains hardware-legal
for an explicitly authored flag.

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

Unknown targets and invalid or unmodeled pool queries return `None` in Python
and null pointer / `std::nullopt` respectively in C++. Built-in catalogs live for
the process lifetime. Query results are independent snapshots; modifying a
returned list or key cannot change the catalog. Native construction validates
pool domains, ID sets, and duplicate canonical keys with `pypto::ValueError`.

## Native rules and ownership

Small C++ constants and initialization rules in `src/sync/resource_catalog.cpp`
construct the built-in catalogs. Queries validate and canonicalize a `PoolKey`,
look up the corresponding prototype, and return a `PoolSpec` snapshot with the
requested replica. The catalog owns the resource facts; callers own any
per-compilation occupancy state. Python exposes built-in queries and cannot
construct or register custom catalogs.

The capacity and reservation rules correspond to PTOAS's
`kTotalEventIdNum` and `kBlockSyncSetWaitEventIdNum` in
`include/PTO/Transforms/InsertSync/SyncEventIdAllocation.h`, the V/S reservation
in `lib/PTO/Transforms/InsertSync/SyncEventIdAllocation.cpp`, and
`ResourceModel::bufidCapacity` in
`include/PTO/Transforms/InsertSync/UnifiedSyncModel.h`.
Changes to these facts belong in the native rules and their catalog tests.

This component supplies facts for subsequent synchronization work. It does not
include a macro registry or resource allocator. Hidden ISA macro footprints,
event-ID allocation, and resource-pressure repair remain outside this interface.
The default automatic synchronization path continues to use PTOAS.
