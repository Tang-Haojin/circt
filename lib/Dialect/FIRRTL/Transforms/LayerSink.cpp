//===- LayerSink.cpp - Sink ops into layer blocks -------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file sinks operations into layer blocks.
//
//===----------------------------------------------------------------------===//

#include "PassDetails.h"

#include "circt/Dialect/FIRRTL/FIRRTLConnectionGraph.h"
#include "mlir/IR/Dominance.h"
#include "mlir/Interfaces/ControlFlowInterfaces.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"
#include "mlir/Transforms/ControlFlowSinkUtils.h"
#include "llvm/ADT/SCCIterator.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "firrtl-layer-sink"

using namespace circt;
using namespace firrtl;

namespace {
/// A control-flow sink pass.
struct LayerSink : public LayerSinkBase<LayerSink> {
  void runOnOperation() override;
};
} // end anonymous namespace

static Operation *mergeOperations(Operation *parent, Operation *user) {

  assert(user && "the user must not be null");

  auto *userParent = user->getParentOp();
  if (!parent)
    return userParent;

  while (!parent->isProperAncestor(user))
    parent = parent->getParentOp();

  return parent;
}

void LayerSink::runOnOperation() {
  LLVM_DEBUG(
      llvm::dbgs() << "==----- Running LayerSink "
                      "---------------------------------------------------===\n"
                   << "Module: '" << getOperation().getName() << "'\n";
  );

  int sccNumber = 0;
  FIRRTLOperation *moduleOp = getOperation();
  Region &parentRegion = moduleOp->getRegion(0);
  for (llvm::scc_iterator<detail::FIRRTLOperation *>
           i = llvm::scc_begin(moduleOp),
           e = llvm::scc_end(moduleOp);
       i != e; ++i) {
    LLVM_DEBUG({
      llvm::dbgs() << "SCC:\n"
                   << "  number: " << sccNumber++ << "\n"
                   << "  operations:\n";
      for (auto *op : *i) {
        llvm::errs() << "  - " << *op << "\n";
      }
    });

    Operation *commonParent = nullptr;
    llvm::DenseSet<Operation *> sccOps(i->begin(), i->end());
    for (auto *op : *i) {
      if (auto connect = dyn_cast<FConnectLike>(op)) {
        auto dest = connect.getDest();
        // If the connect destination is a block argument, then this is driving
        // a port.  Set the common parent to the module.
        if (isa<BlockArgument>(dest)) {
          commonParent = moduleOp;
          continue;
        }
        // If the destination is in the SCC, then skip this connect.
        auto *definingOp = dest.getDefiningOp();
        if (sccOps.contains(definingOp))
          continue;
        commonParent = mergeOperations(commonParent, definingOp);
      }
      for (auto &use : op->getUses()) {
        auto *user = use.getOwner();
        // Skip users in the same SCC.
        if (sccOps.contains(user))
          continue;
        // Skip uses that are connect desintations.
        if (auto connect = dyn_cast<FConnectLike>(user))
          if (use.get() == connect.getDest())
            continue;
        commonParent = mergeOperations(commonParent, user);
      }
    }
    LLVM_DEBUG({
      if (commonParent)
        llvm::dbgs() << "  commonParent: " << *commonParent << "\n";
      else
        llvm::dbgs() << "  commonParent: <null>\n";
    });

    if (!commonParent)
      continue;

    SmallVector<Operation *> sccOpsReverseBlockOrdered(i->begin(), i->end());
    SmallVector<FConnectLike> connects;
    llvm::sort(
        sccOpsReverseBlockOrdered.begin(), sccOpsReverseBlockOrdered.end(),
        [](Operation *a, Operation *b) { return !a->isBeforeInBlock(b); });
    for (auto *op : sccOpsReverseBlockOrdered) {
      if (auto connect = dyn_cast<FConnectLike>(op)) {
        connects.push_back(connect);
        continue;
      }
      auto &region = commonParent->getRegion(0);
      op->moveBefore(&region.front(), region.front().begin());
    }
    for (auto connect : connects) {
      auto afterValue = connect.getDest();
      if (afterValue.getDefiningOp()->isBeforeInBlock(
              connect.getSrc().getDefiningOp()))
        afterValue = connect.getSrc();
      connect->moveAfter(afterValue.getDefiningOp());
    }
  }
}

std::unique_ptr<mlir::Pass> circt::firrtl::createLayerSinkPass() {
  return std::make_unique<LayerSink>();
}
