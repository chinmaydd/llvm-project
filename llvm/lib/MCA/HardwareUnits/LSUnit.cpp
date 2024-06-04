//===----------------------- LSUnit.cpp --------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
/// \file
///
/// A Load-Store Unit for the llvm-mca tool.
///
//===----------------------------------------------------------------------===//

#ifndef NDEBUG
#include "llvm/ADT/StringExtras.h"
#endif
#include "llvm/MCA/HardwareUnits/LSUnit.h"
#include "llvm/MCA/Instruction.h"
#include "llvm/MCA/MetadataCategories.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Debug.h"
#ifndef NDEBUG
#include "llvm/Support/Format.h"
#endif
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "llvm-mca"

namespace llvm {
namespace mca {

uint64_t MDMemoryAccess::getExtendedStartAddr() const {
  if (LLVM_UNLIKELY(BundledMAs))
    return BundledMAs->ExtendedAddr;
  else
    return Addr;
}

uint64_t MDMemoryAccess::getExtendedEndAddr() const {
  if (LLVM_UNLIKELY(BundledMAs))
    return BundledMAs->ExtendedAddr + BundledMAs->ExtendedSize;
  else
    return Addr + Size;
}

void MDMemoryAccess::append(bool NewIsStore,
                            uint64_t NewAddr, unsigned NewSize) {
  if (!BundledMAs)
    BundledMAs = std::make_shared<BundledMemoryAccesses>(Addr, Size);
  auto &BMA = *BundledMAs;

  if (NewAddr < BMA.ExtendedAddr)
    BMA.ExtendedAddr = NewAddr;

  uint64_t NewEnd = NewAddr + NewSize;
  if (NewEnd > BMA.ExtendedAddr + BMA.ExtendedSize)
    BMA.ExtendedSize = NewEnd - BMA.ExtendedAddr;

  BMA.Accesses.emplace_back(NewIsStore, NewAddr, NewSize);
}

#ifndef NDEBUG
raw_ostream &operator<<(raw_ostream &OS, const MDMemoryAccess & MDA) {
  OS << "[ " << format_hex(MDA.Addr, 16) << " - "
             << format_hex(uint64_t(MDA.Addr + MDA.Size), 16) << " ], ";
  OS << "IsStore: " << toStringRef(MDA.IsStore);
  return OS;
}
#endif

LSUnitBase::LSUnitBase(const MCSchedModel &SM, unsigned LQ, unsigned SQ,
                       bool AssumeNoAlias, MetadataRegistry *MDR)
    : LQSize(LQ), SQSize(SQ), UsedLQEntries(0), UsedSQEntries(0),
      NoAlias(AssumeNoAlias), NextGroupID(1),
      MDRegistry(MDR) {
  if (SM.hasExtraProcessorInfo()) {
    const MCExtraProcessorInfo &EPI = SM.getExtraProcessorInfo();
    if (!LQSize && EPI.LoadQueueID) {
      const MCProcResourceDesc &LdQDesc = *SM.getProcResource(EPI.LoadQueueID);
      LQSize = std::max(0, LdQDesc.BufferSize);
    }

    if (!SQSize && EPI.StoreQueueID) {
      const MCProcResourceDesc &StQDesc = *SM.getProcResource(EPI.StoreQueueID);
      SQSize = std::max(0, StQDesc.BufferSize);
    }
  }
}

Optional<MDMemoryAccess>
LSUnitBase::getMemoryAccessMD(const InstRef &IR) const {
  if (MDRegistry && IR.getInstruction()->getMetadataToken()) {
    auto &Registry = (*MDRegistry)[MD_LSUnit_MemAccess];
    unsigned MDTok = *IR.getInstruction()->getMetadataToken();
    return Registry.get<MDMemoryAccess>(MDTok);
  }
  return llvm::None;
}

bool LSUnitBase::noAlias(unsigned GID,
                         const Optional<MDMemoryAccess> &MDA) const {
  if (MDA) {
    const MemoryGroup &MG = getGroup(GID);
    LLVM_DEBUG(dbgs() << "[LSUnit][MD]: Comparing GID " << GID
                      << " with MDMemoryAccess "
                      << *MDA << "\n");
    bool Result = !MG.isMemAccessAlias(*MDA);
    LLVM_DEBUG(dbgs() << "[LSUnit][MD]: GID is alias with MDA: "
                      << toStringRef(!Result) << "\n");
    if (!Result) {
        LLVM_DEBUG(dbgs() << "[LSUnit] We have alias!\n");
    }
    return Result;
  } else
    return assumeNoAlias();
}

LSUnitBase::~LSUnitBase() {}

void LSUnitBase::cycleEvent() {
  for (const std::pair<unsigned, std::unique_ptr<MemoryGroup>> &G : Groups)
    G.second->cycleEvent();
}

#ifndef NDEBUG
void LSUnitBase::dump() const {
  dbgs() << "[LSUnit] LQ_Size = " << getLoadQueueSize() << '\n';
  dbgs() << "[LSUnit] SQ_Size = " << getStoreQueueSize() << '\n';
  dbgs() << "[LSUnit] NextLQSlotIdx = " << getUsedLQEntries() << '\n';
  dbgs() << "[LSUnit] NextSQSlotIdx = " << getUsedSQEntries() << '\n';
  dbgs() << "\n";
  for (const auto &GroupIt : Groups) {
    const MemoryGroup &Group = *GroupIt.second;
    dbgs() << "[LSUnit] Group (" << GroupIt.first << "): "
           << "[ #Preds = " << Group.getNumPredecessors()
           << ", #GIssued = " << Group.getNumExecutingPredecessors()
           << ", #GExecuted = " << Group.getNumExecutedPredecessors()
           << ", #Inst = " << Group.getNumInstructions()
           << ", #IIssued = " << Group.getNumExecuting()
           << ", #IExecuted = " << Group.getNumExecuted() << '\n';
  }
}
#endif

unsigned LSUnit::dispatch(const InstRef &IR) {
  const InstrDesc &Desc = IR.getInstruction()->getDesc();
  auto MaybeMDA = getMemoryAccessMD(IR);
  bool IsStoreBarrier = IR.getInstruction()->isAStoreBarrier();
  bool IsLoadBarrier = IR.getInstruction()->isALoadBarrier();
  assert((Desc.MayLoad || Desc.MayStore) && "Not a memory operation!");

  if (Desc.MayLoad)
    acquireLQSlot();
  if (isStore(Desc, MaybeMDA))
    acquireSQSlot();

  if (isStore(Desc, MaybeMDA)) {
    unsigned NewGID = createMemoryGroup();
    MemoryGroup &NewGroup = getGroup(NewGID);
    NewGroup.addInstruction();
    NewGroup.addMemAccess(MaybeMDA);
    LLVM_DEBUG(if (MaybeMDA)
                 dbgs() << "[LSUnit][MD]: GID " << NewGID
                        << " has a new MemAccessMD: "
                        << *MaybeMDA << "\n");

    // A store may not pass a previous load or load barrier.
    unsigned ImmediateLoadDominator =
        std::max(CurrentLoadGroupID, CurrentLoadBarrierGroupID);
    if (ImmediateLoadDominator) {
      MemoryGroup &IDom = getGroup(ImmediateLoadDominator);
      LLVM_DEBUG(dbgs() << "[LSUnit]: GROUP DEP: (" << ImmediateLoadDominator
                        << ") --> (" << NewGID << ")\n");
      IDom.addSuccessor(&NewGroup, !noAlias(ImmediateLoadDominator, MaybeMDA));
    }

    // A store may not pass a previous store barrier.
    if (CurrentStoreBarrierGroupID) {
      MemoryGroup &StoreGroup = getGroup(CurrentStoreBarrierGroupID);
      LLVM_DEBUG(dbgs() << "[LSUnit]: GROUP DEP: ("
                        << CurrentStoreBarrierGroupID
                        << ") --> (" << NewGID << ")\n");
      StoreGroup.addSuccessor(&NewGroup, true);
    }

    // A store may not pass a previous store.
    if (CurrentStoreGroupID &&
        (CurrentStoreGroupID != CurrentStoreBarrierGroupID)) {
      MemoryGroup &StoreGroup = getGroup(CurrentStoreGroupID);
      LLVM_DEBUG(dbgs() << "[LSUnit]: GROUP DEP: (" << CurrentStoreGroupID
                        << ") --> (" << NewGID << ")\n");
      StoreGroup.addSuccessor(&NewGroup, !noAlias(CurrentStoreGroupID,
                                                  MaybeMDA));
    }


    CurrentStoreGroupID = NewGID;
    if (IsStoreBarrier)
      CurrentStoreBarrierGroupID = NewGID;

    if (Desc.MayLoad) {
      CurrentLoadGroupID = NewGID;
      if (IsLoadBarrier)
        CurrentLoadBarrierGroupID = NewGID;
    }

    return NewGID;
  }

  assert(Desc.MayLoad && "Expected a load!");

  unsigned ImmediateLoadDominator =
      std::max(CurrentLoadGroupID, CurrentLoadBarrierGroupID);

  // A new load group is created if we are in one of the following situations:
  // 1) This is a load barrier (by construction, a load barrier is always
  //    assigned to a different memory group).
  // 2) There is no load in flight (by construction we always keep loads and
  //    stores into separate memory groups).
  // 3) There is a load barrier in flight. This load depends on it.
  // 4) There is an intervening store between the last load dispatched to the
  //    LSU and this load. We always create a new group even if this load
  //    does not alias the last dispatched store.
  // 5) There is no intervening store and there is an active load group.
  //    However that group has already started execution, so we cannot add
  //    this load to it.
  bool ShouldCreateANewGroup =
      IsLoadBarrier || !ImmediateLoadDominator ||
      CurrentLoadBarrierGroupID == ImmediateLoadDominator ||
      ImmediateLoadDominator <= CurrentStoreGroupID ||
      getGroup(ImmediateLoadDominator).isExecuting();

  if (ShouldCreateANewGroup) {
    unsigned NewGID = createMemoryGroup();
    MemoryGroup &NewGroup = getGroup(NewGID);
    NewGroup.addInstruction();
    NewGroup.addMemAccess(MaybeMDA);
    LLVM_DEBUG(if (MaybeMDA)
                 dbgs() << "[LSUnit][MD]: GID " << NewGID
                        << " has a new MemAccessMD: "
                        << *MaybeMDA << "\n");

    // A load may not pass a previous store or store barrier
    // unless flag 'NoAlias' is set.
    if (CurrentStoreGroupID && !noAlias(CurrentStoreGroupID, MaybeMDA)) {
      MemoryGroup &StoreGroup = getGroup(CurrentStoreGroupID);
      LLVM_DEBUG(dbgs() << "[LSUnit]: GROUP DEP: (" << CurrentStoreGroupID
                        << ") --> (" << NewGID << ")\n");
      StoreGroup.addSuccessor(&NewGroup, true);
    }

    // A load barrier may not pass a previous load or load barrier.
    if (IsLoadBarrier) {
      if (ImmediateLoadDominator) {
        MemoryGroup &LoadGroup = getGroup(ImmediateLoadDominator);
        LLVM_DEBUG(dbgs() << "[LSUnit]: GROUP DEP: ("
                          << ImmediateLoadDominator
                          << ") --> (" << NewGID << ")\n");
        LoadGroup.addSuccessor(&NewGroup, true);
      }
    } else {
      // A younger load cannot pass a older load barrier.
      if (CurrentLoadBarrierGroupID) {
        MemoryGroup &LoadGroup = getGroup(CurrentLoadBarrierGroupID);
        LLVM_DEBUG(dbgs() << "[LSUnit]: GROUP DEP: ("
                          << CurrentLoadBarrierGroupID
                          << ") --> (" << NewGID << ")\n");
        LoadGroup.addSuccessor(&NewGroup, true);
      }
    }

    CurrentLoadGroupID = NewGID;
    if (IsLoadBarrier)
      CurrentLoadBarrierGroupID = NewGID;
    return NewGID;
  }

  // A load may pass a previous load.
  MemoryGroup &Group = getGroup(CurrentLoadGroupID);
  Group.addInstruction();
  Group.addMemAccess(MaybeMDA);
  LLVM_DEBUG(if (MaybeMDA)
               dbgs() << "[LSUnit][MD]: GID " << CurrentLoadGroupID
                      << " has a new MemAccessMD: "
                      << *MaybeMDA << "\n");
  return CurrentLoadGroupID;
}

LSUnit::Status LSUnit::isAvailable(const InstRef &IR) const {
  const InstrDesc &Desc = IR.getInstruction()->getDesc();
  auto MaybeMDA = getMemoryAccessMD(IR);
  if (Desc.MayLoad && isLQFull())
    return LSUnit::LSU_LQUEUE_FULL;
  if (isStore(Desc, MaybeMDA) && isSQFull())
    return LSUnit::LSU_SQUEUE_FULL;
  return LSUnit::LSU_AVAILABLE;
}

void LSUnitBase::onInstructionExecuted(const InstRef &IR) {
  unsigned GroupID = IR.getInstruction()->getLSUTokenID();
  auto It = Groups.find(GroupID);
  assert(It != Groups.end() && "Instruction not dispatched to the LS unit");
  It->second->onInstructionExecuted(IR);
  if (It->second->isExecuted())
    Groups.erase(It);
}

void LSUnitBase::onInstructionRetired(const InstRef &IR) {
  const InstrDesc &Desc = IR.getInstruction()->getDesc();
  auto MaybeMDA = getMemoryAccessMD(IR);
  bool IsALoad = Desc.MayLoad;
  bool IsAStore = isStore(Desc, MaybeMDA);
  assert((IsALoad || IsAStore) && "Expected a memory operation!");

  if (IsALoad) {
    releaseLQSlot();
    LLVM_DEBUG(dbgs() << "[LSUnit]: Instruction idx=" << IR.getSourceIndex()
                      << " has been removed from the load queue.\n");
  }

  if (IsAStore) {
    releaseSQSlot();
    LLVM_DEBUG(dbgs() << "[LSUnit]: Instruction idx=" << IR.getSourceIndex()
                      << " has been removed from the store queue.\n");
  }
}

void LSUnit::onInstructionExecuted(const InstRef &IR) {
  const Instruction &IS = *IR.getInstruction();
  if (!IS.isMemOp())
    return;

  LSUnitBase::onInstructionExecuted(IR);
  unsigned GroupID = IS.getLSUTokenID();
  if (!isValidGroupID(GroupID)) {
    if (GroupID == CurrentLoadGroupID)
      CurrentLoadGroupID = 0;
    if (GroupID == CurrentStoreGroupID)
      CurrentStoreGroupID = 0;
    if (GroupID == CurrentLoadBarrierGroupID)
      CurrentLoadBarrierGroupID = 0;
    if (GroupID == CurrentStoreBarrierGroupID)
      CurrentStoreBarrierGroupID = 0;
  }
}

} // namespace mca
} // namespace llvm
