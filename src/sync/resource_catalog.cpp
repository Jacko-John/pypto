/*
 * Copyright (c) PyPTO Contributors.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 * -----------------------------------------------------------------------------------------------------------
 */

/**
 * @file resource_catalog.cpp
 * @brief Validate, canonicalize and query immutable target synchronization pools.
 *
 * Resource family, executor, direction and replica establish pool identity.
 * Compiler reservation policy remains separate from hardware legality. The
 * implementation owns no allocation state and reads no installed toolchain.
 * Native rules mirror PTOAS resource models; a bank query does not establish
 * that a particular pipe instruction is legal on the queried executor.
 */

#include "pypto/sync/resource_catalog.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "pypto/core/logging.h"
#include "pypto/ir/pipe.h"

namespace pypto::sync {
namespace {

// PTOAS include/PTO/Transforms/InsertSync/SyncEventIdAllocation.h defines
// kTotalEventIdNum=8 and kBlockSyncSetWaitEventIdNum=16. Signals count logical
// bases, not A5's base+16 physical companion: pto-isa include/pto/npu/a5/TPush.hpp
// uses VEC_CORE_ID_OFFSET for that subblock mapping during lowering.
constexpr uint32_t kEventIdCount = 8;
constexpr uint32_t kIntraBlockSignalBaseCount = 16;

// PTOAS include/PTO/Transforms/InsertSync/UnifiedSyncModel.h ResourceModel uses
// K=0 for A3 and K=32 for A5. Keep the existing combined a2a3 target disabled
// for buffer credits; zero means this compiler model does not offer that pool.
constexpr uint32_t kA2A3BufferCreditCount = 0;
constexpr uint32_t kA5BufferCreditCount = 32;

// PTOAS lib/PTO/Transforms/InsertSync/SyncEventIdAllocation.cpp uses
// reservedEventIdNum to withhold the last event in each V<->S pool from automatic
// allocation. This is compiler policy; ID7 remains in the hardware range for
// explicitly authored synchronization.
constexpr uint32_t kScalarVectorReservedEventId = kEventIdCount - 1;

/// Recognize all declared enum values before canonicalization can hide malformed aliases.
bool IsValidKey(const PoolKey& key) {
  const bool kind = key.kind == ResourceKind::PipelineEvent || key.kind == ResourceKind::IntraBlockSignal ||
                    key.kind == ResourceKind::BufferCredit;
  const bool core =
      key.core == CoreScope::AIC || key.core == CoreScope::AIV || key.core == CoreScope::CoreGroup;
  const auto valid_pipe = [](ir::PipeType pipe) {
    return pipe >= ir::PipeType::MTE1 && pipe <= ir::PipeType::ALL;
  };
  if (!kind || !core || !valid_pipe(key.src) || !valid_pipe(key.dst)) return false;
  // A same-pipe barrier needs no event bank, and CoreGroup is not a pipeline executor.
  return key.kind != ResourceKind::PipelineEvent ||
         (key.core != CoreScope::CoreGroup && key.src != key.dst && key.src != ir::PipeType::ALL &&
          key.dst != ir::PipeType::ALL);
}

}  // namespace

/// Keep raw key equality independent of explicit alias normalization at catalog boundaries.
bool PoolKey::operator==(const PoolKey& other) const {
  return std::tie(kind, core, src, dst, replica) ==
         std::tie(other.kind, other.core, other.src, other.dst, other.replica);
}

/// Use the same raw identity in ordered maps, including distinct caller replicas.
bool PoolKey::operator<(const PoolKey& other) const {
  return std::tie(kind, core, src, dst, replica) <
         std::tie(other.kind, other.core, other.src, other.dst, other.replica);
}

/// Normalize API spellings into conflict domains without changing the caller's object or ID space.
PoolKey CanonicalizePoolKey(PoolKey key) {
  // Signal APIs spell the participating executor/pipe, but these are aliases of one core-group pool.
  if (key.kind == ResourceKind::IntraBlockSignal) key.core = CoreScope::CoreGroup;
  if (key.kind != ResourceKind::PipelineEvent) key.src = key.dst = ir::PipeType::ALL;
  return key;
}

/// Format a validated domain in canonical form; invalid pipe enums are not accepted by this helper.
std::string DescribePoolKey(const PoolKey& key) {
  const auto canonical = CanonicalizePoolKey(key);
  const char* family = canonical.kind == ResourceKind::PipelineEvent      ? "pipeline-event"
                       : canonical.kind == ResourceKind::IntraBlockSignal ? "intra-block"
                                                                          : "buffer-credit";
  const char* core = canonical.core == CoreScope::AIC   ? "aic"
                     : canonical.core == CoreScope::AIV ? "aiv"
                                                        : "group";
  return std::string(family) + ":" + core + ":" + ir::PipeTypeToString(canonical.src) + "->" +
         ir::PipeTypeToString(canonical.dst) + ":" + std::to_string(canonical.replica);
}

/// Validate caller-owned facts before exposing the catalog; input vectors are consumed by value.
ResourceCatalog::ResourceCatalog(std::string target, std::vector<PoolSpec> pools)
    : target_(std::move(target)) {
  CHECK(!target_.empty()) << "Synchronization resource catalog requires a target name";
  for (auto& pool : pools) {
    CHECK(IsValidKey(pool.key)) << "Invalid synchronization resource pool key";
    CHECK(pool.key.replica == 0) << "Catalog pool specifications describe replica zero";
    // Validate the raw spelling first, then detect collisions after canonicalization. Independent
    // aliases must not register contradictory capacities or policy for the same hardware domain.
    pool.key = CanonicalizePoolKey(pool.key);
    std::set<uint32_t> legal;
    for (auto id : pool.legal_ids) {
      CHECK(id < pool.capacity && legal.insert(id).second)
          << "Invalid or duplicate legal ID " << id << " in " << DescribePoolKey(pool.key);
    }
    // Policy exclusions are a subset of hardware legality. They never shrink capacity or change the
    // sparse set returned to explicit source-authored users of the same physical pool.
    std::set<uint32_t> reserved;
    for (auto id : pool.reserved_ids) {
      CHECK(id < pool.capacity && (legal.empty() || legal.count(id)) && reserved.insert(id).second)
          << "Invalid or duplicate reserved ID " << id << " in " << DescribePoolKey(pool.key);
    }
    CHECK(pools_.emplace(pool.key, std::move(pool)).second) << "Duplicate synchronization resource pool";
  }
}

/// Return independent facts, binding the queried replica to its shared capability prototype.
std::optional<PoolSpec> ResourceCatalog::FindPool(const PoolKey& key) const {
  if (!IsValidKey(key)) return std::nullopt;
  auto canonical = CanonicalizePoolKey(key);
  auto prototype = canonical;
  prototype.replica = 0;
  const auto it = pools_.find(prototype);
  if (it == pools_.end()) return std::nullopt;
  // Copying prevents callers from changing a later query through a returned pool or vector.
  auto result = it->second;
  result.key = canonical;
  return result;
}

namespace {

/// Build one target's resource model without inferring executor instruction legality.
ResourceCatalog MakeBuiltinResourceCatalog(std::string target, uint32_t buffer_credit_count) {
  constexpr std::array cores{CoreScope::AIC, CoreScope::AIV};
  constexpr std::array pipes{ir::PipeType::MTE1, ir::PipeType::MTE2, ir::PipeType::MTE3, ir::PipeType::M,
                             ir::PipeType::V,    ir::PipeType::S,    ir::PipeType::FIX};
  std::vector<PoolSpec> pools;
  pools.reserve(cores.size() * pipes.size() * (pipes.size() - 1) + 2);

  // PTOAS SyncEventIdAllocation::ScopePair keys events by ordered pipe pair.
  // Both executors receive the same capability prototype; backend validation,
  // not the existence of this entry, decides which instructions each can run.
  for (const auto core : cores) {
    for (const auto src : pipes) {
      for (const auto dst : pipes) {
        if (src == dst) continue;
        std::vector<uint32_t> reserved;
        if ((src == ir::PipeType::V && dst == ir::PipeType::S) ||
            (src == ir::PipeType::S && dst == ir::PipeType::V)) {
          reserved.push_back(kScalarVectorReservedEventId);
        }
        pools.push_back(
            {{ResourceKind::PipelineEvent, core, src, dst}, kEventIdCount, {}, std::move(reserved), false});
      }
    }
  }

  // Block set/wait uses one shared ScopePair in PTOAS, independent of pipe.
  // Block-all's conditional use of IDs14/15 belongs to per-compilation policy,
  // so the catalog reports the full sixteen-base domain without reserving them.
  pools.push_back(
      {{ResourceKind::IntraBlockSignal, CoreScope::CoreGroup}, kIntraBlockSignalBaseCount, {}, {}, false});
  // Buffer credits use the existing AIV-owned model, shared across directions.
  // Preserve a registered zero-capacity pool to distinguish unavailable from unknown.
  pools.push_back({{ResourceKind::BufferCredit, CoreScope::AIV}, buffer_credit_count, {}, {}, false});
  return ResourceCatalog(std::move(target), std::move(pools));
}

/// Construct the two supported target models through the same validation used by native callers.
std::array<ResourceCatalog, 2> MakeBuiltinResourceCatalogs() {
  return {MakeBuiltinResourceCatalog("a2a3", kA2A3BufferCreditCount),
          MakeBuiltinResourceCatalog("a5", kA5BufferCreditCount)};
}

}  // namespace

/// Initialize built-ins once and borrow their process-lifetime facts; unknown names return null.
const ResourceCatalog* FindResourceCatalog(const std::string& target) {
  static const auto catalogs = MakeBuiltinResourceCatalogs();
  const auto it = std::find_if(catalogs.begin(), catalogs.end(),
                               [&](const auto& catalog) { return catalog.GetTarget() == target; });
  return it == catalogs.end() ? nullptr : &*it;
}

}  // namespace pypto::sync
