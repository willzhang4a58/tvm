/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
#ifndef TVM_TIR_SCHEDULE_UTILS_H_
#define TVM_TIR_SCHEDULE_UTILS_H_

#include <tvm/arith/analyzer.h>
#include <tvm/arith/int_set.h>
#include <tvm/arith/iter_affine_map.h>
#include <tvm/tir/analysis.h>
#include <tvm/tir/function.h>
#include <tvm/tir/op.h>
#include <tvm/tir/schedule/instruction.h>
#include <tvm/tir/schedule/schedule.h>
#include <tvm/tir/schedule/state.h>
#include <tvm/tir/schedule/trace.h>
#include <tvm/tir/stmt_functor.h>

#include <unordered_map>
#include <utility>

#include "../../arith/pattern_match.h"
#include "../../node/attr_registry.h"
#include "../../printer/text_printer.h"
#include "../../runtime/thread_storage_scope.h"
#include "../../support/array.h"
#include "../../support/nd_int_set.h"
#include "./analysis.h"
#include "./error.h"
#include "./instruction_traits.h"
#include "./primitive.h"
#include "./transform.h"

namespace tvm {
namespace tir {

/*!
 * \brief A helper macro to convert an sref to the statement it points to,
 * then check if the downcasting succeeded.
 * \param Result The result variable, used for checking
 * \param SRef The SRef to be cast
 * \param Type The type to be cast to, can be Block or For
 */
#define TVM_SREF_AS_OR_ERR(Result, SRef, Type) \
  SRef->StmtAs<Type>();                        \
  ICHECK(Result)

/*!
 * \brief A helper macro to convert an sref to the block it points to,
 * throwing an internal error if downcasting fails
 * \param Result The result variable, used for checking
 * \param SRef The SRef to be cast
 */
#define TVM_SREF_TO_BLOCK(Result, SRef)                   \
  TVM_SREF_AS_OR_ERR(Result, SRef, ::tvm::tir::BlockNode) \
      << "TypeError: Expects StmtSRef `" << #SRef         \
      << "` points to `Block`, but gets: " << (SRef->stmt ? SRef->stmt->GetTypeKey() : "None")

/*!
 * \brief A helper macro to convert an sref to the for-loop it points to,
 * throwing an internal error if downcasting fails
 * \param Result The name of the result variable, used for checking
 * \param SRef The SRef to be cast
 */
#define TVM_SREF_TO_FOR(Result, SRef)                   \
  TVM_SREF_AS_OR_ERR(Result, SRef, ::tvm::tir::ForNode) \
      << "TypeError: Expects StmtSRef `" << #SRef       \
      << "` points to `Loop`, but gets: " << (SRef->stmt ? SRef->stmt->GetTypeKey() : "None")

/*!
 * \brief Downcast a TVM ObjectRef to its corresponding container using `ObjectRef::as<Type>`,
 * then check if the downcasting succeeded.
 * \param Result The result variable, used for checking
 * \param From The ObjectRef to be downcast
 * \param Type The type to be downcast to
 */
#define TVM_TYPE_AS_OR_ERR(Result, From, Type) \
  From.as<Type>();                             \
  ICHECK(Result)

/*!
 * \brief Downcast a TVM ObjectRef to its corresponding container using `ObjectRef::as<Type>`,
 * throwing an internal error if downcast fails.
 * \param Result The result variable, used for checking
 * \param From The ObjectRef to be downcast
 * \param Type The type to be downcast to
 */
#define TVM_TYPE_AS(Result, From, Type)                                           \
  TVM_TYPE_AS_OR_ERR(Result, From, Type)                                          \
      << "TypeError: Expects `" << #From << "` to have type `" << Type::_type_key \
      << "`, but gets: " << (From.defined() ? From->GetTypeKey() : "None")

/*!
 * \brief Convert an array of loop StmtSRefs to an array of loops
 * \param loop_srefs The loop StmtSRefs to be converted
 * \return The conversion result loops
 */
inline Array<For> LoopSRefs2Loops(const Array<StmtSRef>& loop_srefs) {
  Array<For> loops;
  loops.reserve(loop_srefs.size());
  for (StmtSRef loop_sref : loop_srefs) {
    const ForNode* loop = TVM_SREF_TO_FOR(loop, loop_sref);
    loops.push_back(GetRef<For>(loop));
  }
  return loops;
}

/******** Storage scope ********/

/*!
 * \brief Determine if iterators of a storage scope should be relaxed
 * under a specific thread scope
 * \param storage_scope The storage scope that the iterators are on
 * \param thread_scope The thread scope to be relaxed
 * \return A boolean indicating the result
 */
inline bool CanRelaxStorageUnderThread(const runtime::StorageScope& storage_scope,
                                       const runtime::ThreadScope& thread_scope) {
  if (storage_scope.rank == runtime::StorageRank::kWarp) {
    // for warp memory, we only relax threadIdx.x
    return thread_scope.rank == 1 && thread_scope.dim_index == 0;
  }
  return static_cast<int>(storage_scope.rank) <= static_cast<int>(thread_scope.rank);
}

/******** SeqStmt ********/

/*!
 * \brief Remove a specific Stmt from a SeqStmt. If a SeqStmt contains a BlockRealize,
 * whose block is the Stmt to be removed, then remove that BlockRealize too.
 * \param seq The SeqStmt to be removed from
 * \param to_remove The Stmt to be removed
 * \return The removal result
 */
inline Stmt RemoveFromSeqStmt(const SeqStmt& seq, const Stmt& to_remove) {
  ICHECK_GT(seq->size(), 1);
  Array<Stmt> new_stmts;
  new_stmts.reserve(seq->size());
  for (const Stmt& stmt : seq->seq) {
    if (to_remove.same_as(stmt)) {
      continue;
    }
    if (const auto* realize = stmt.as<BlockRealizeNode>()) {
      if (to_remove.same_as(realize->block)) {
        continue;
      }
    }
    new_stmts.push_back(stmt);
  }
  return SeqStmt::Flatten(new_stmts);
}

/*!
 * \brief Convert a Stmt to an Array.
 * \param stmt The Stmt to be converted to
 * \return If the Stmt is SeqStmt, then returns the sequence;
 * Otherwise, returns a single-element Array with the Stmt inside.
 */
inline Array<Stmt> AsArray(const Stmt& stmt) {
  if (const auto* seq_stmt = stmt.as<SeqStmtNode>()) {
    return seq_stmt->seq;
  }
  return {stmt};
}

/******** IterVar ********/

/*!
 * \brief Create a new IterVar for the input For loop, with specified name and type
 * \param loop The loop to be created from
 * \param name The name of the new IterVar
 * \param iter_var_type The type of the new IterVar
 * \return The newly created IterVar
 */
inline IterVar IterVarFromLoop(const For& loop, String name, IterVarType iter_var_type) {
  return IterVar(Range::FromMinExtent(loop->min, loop->extent),
                 Var(std::move(name), loop->loop_var.dtype()), iter_var_type);
}

/******** Integer set ********/

/*!
 * \brief Converts the Ranges to IntSets
 * \param var_dom The ranges of variables
 * \return The integer sets of the variables
 */
inline Map<Var, arith::IntSet> AsIntSet(const Map<Var, Range>& var_dom) {
  std::unordered_map<Var, arith::IntSet, ObjectPtrHash, ObjectPtrEqual> result;
  result.reserve(var_dom.size());
  for (auto kv : var_dom) {
    Var& var = kv.first;
    Range& range = kv.second;
    result.emplace(std::move(var), arith::IntSet::FromRange(std::move(range)));
  }
  return {result.begin(), result.end()};
}

/**************** Loop extents ****************/

/*!
 * \brief Get the extents of a loop
 * \param loop The loop to be queried
 * \return The extent of the loop, nullptr if the extent is not constant
 */
inline const int64_t* GetLoopIntExtent(const ForNode* loop) { return as_const_int(loop->extent); }

/*!
 * \brief Get the extents of a loop
 * \param loop_sref The loop to be queried
 * \return The extent of the loop, nullptr if the extent is not constant
 */
inline const int64_t* GetLoopIntExtent(const StmtSRef& loop_sref) {
  const ForNode* loop = TVM_SREF_TO_FOR(loop, loop_sref);
  return as_const_int(loop->extent);
}

/******** Annotation ********/

/*!
 * \brief Get the annotation on a Block/For
 * \tparam TObjectRef The type of the annotation value
 * \param sref The sref to the block or the for loop
 * \param ann_key The annotation key to be looked up
 * \return NullOpt if not found; otherwise the annotation value
 */
template <class TObjectRef, class TStmtNode>
inline Optional<TObjectRef> GetAnn(const TStmtNode* stmt, const String& ann_key) {
  const Map<String, ObjectRef>* annotations = &stmt->annotations;
  for (const auto& ann : *annotations) {
    if (ann.first == ann_key) {
      return Downcast<TObjectRef>(ann.second);
    }
  }
  return NullOpt;
}

/*!
 * \brief Get the annotation on a Block/For
 * \tparam TObjectRef The type of the annotation value
 * \param sref The sref to the block or the for loop
 * \param ann_key The annotation key to be looked up
 * \return NullOpt if not found; otherwise the annotation value
 */
template <class TObjectRef>
inline Optional<TObjectRef> GetAnn(const StmtSRef& sref, const String& ann_key) {
  if (const auto* loop = sref->StmtAs<ForNode>()) {
    return GetAnn<TObjectRef, ForNode>(loop, ann_key);
  } else if (const auto* block = sref->StmtAs<BlockNode>()) {
    return GetAnn<TObjectRef, BlockNode>(block, ann_key);
  } else {
    LOG(FATAL) << "TypeError: Unknown type of sref: " << sref->stmt->GetTypeKey();
    throw;
  }
}

/*!
 * \brief Check if a Block/For has a specific pair of annotation key and values
 * \param sref The sref to the block or the for loop
 * \param ann_key The annotation key to be checked
 * \param ann_val The annotation value to be checked
 * \return Whether a Block/For has a specific pair of annotation key and values
 */
inline bool HasAnn(const StmtSRef& sref, const String& ann_key, const String& ann_val) {
  Optional<String> result = GetAnn<String>(sref, ann_key);
  return result.defined() && result.value() == ann_val;
}

}  // namespace tir
}  // namespace tvm

#endif  // TVM_TIR_SCHEDULE_UTILS_H_
