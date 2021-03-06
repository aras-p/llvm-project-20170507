//===-- llvm/CodeGen/GlobalISel/Legalizer.cpp -----------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
/// \file This file implements the LegalizerHelper class to legalize individual
/// instructions and the LegalizePass wrapper pass for the primary
/// legalization.
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/GlobalISel/Legalizer.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/CodeGen/GlobalISel/GISelChangeObserver.h"
#include "llvm/CodeGen/GlobalISel/GISelWorkList.h"
#include "llvm/CodeGen/GlobalISel/LegalizationArtifactCombiner.h"
#include "llvm/CodeGen/GlobalISel/LegalizerHelper.h"
#include "llvm/CodeGen/GlobalISel/Utils.h"
#include "llvm/CodeGen/MachineOptimizationRemarkEmitter.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/Support/Debug.h"

#include <iterator>

#define DEBUG_TYPE "legalizer"

using namespace llvm;

char Legalizer::ID = 0;
INITIALIZE_PASS_BEGIN(Legalizer, DEBUG_TYPE,
                      "Legalize the Machine IR a function's Machine IR", false,
                      false)
INITIALIZE_PASS_DEPENDENCY(TargetPassConfig)
INITIALIZE_PASS_END(Legalizer, DEBUG_TYPE,
                    "Legalize the Machine IR a function's Machine IR", false,
                    false)

Legalizer::Legalizer() : MachineFunctionPass(ID) {
  initializeLegalizerPass(*PassRegistry::getPassRegistry());
}

void Legalizer::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TargetPassConfig>();
  getSelectionDAGFallbackAnalysisUsage(AU);
  MachineFunctionPass::getAnalysisUsage(AU);
}

void Legalizer::init(MachineFunction &MF) {
}

static bool isArtifact(const MachineInstr &MI) {
  switch (MI.getOpcode()) {
  default:
    return false;
  case TargetOpcode::G_TRUNC:
  case TargetOpcode::G_ZEXT:
  case TargetOpcode::G_ANYEXT:
  case TargetOpcode::G_SEXT:
  case TargetOpcode::G_MERGE_VALUES:
  case TargetOpcode::G_UNMERGE_VALUES:
  case TargetOpcode::G_CONCAT_VECTORS:
  case TargetOpcode::G_BUILD_VECTOR:
    return true;
  }
}
using InstListTy = GISelWorkList<256>;
using ArtifactListTy = GISelWorkList<128>;

class LegalizerWorkListManager : public GISelChangeObserver {
  InstListTy &InstList;
  ArtifactListTy &ArtifactList;

public:
  LegalizerWorkListManager(InstListTy &Insts, ArtifactListTy &Arts)
      : InstList(Insts), ArtifactList(Arts) {}

  void createdInstr(const MachineInstr &MI) override {
    // Only legalize pre-isel generic instructions.
    // Legalization process could generate Target specific pseudo
    // instructions with generic types. Don't record them
    if (isPreISelGenericOpcode(MI.getOpcode())) {
      if (isArtifact(MI))
        ArtifactList.insert(&MI);
      else
        InstList.insert(&MI);
    }
    LLVM_DEBUG(dbgs() << ".. .. New MI: " << MI);
  }

  void erasingInstr(const MachineInstr &MI) override {
    LLVM_DEBUG(dbgs() << ".. .. Erasing: " << MI);
    InstList.remove(&MI);
    ArtifactList.remove(&MI);
  }

  void changingInstr(const MachineInstr &MI) override {
    LLVM_DEBUG(dbgs() << ".. .. Changing MI: " << MI);
  }

  void changedInstr(const MachineInstr &MI) override {
    // When insts change, we want to revisit them to legalize them again.
    // We'll consider them the same as created.
    LLVM_DEBUG(dbgs() << ".. .. Changed MI: " << MI);
    createdInstr(MI);
  }
};

bool Legalizer::runOnMachineFunction(MachineFunction &MF) {
  // If the ISel pipeline failed, do not bother running that pass.
  if (MF.getProperties().hasProperty(
          MachineFunctionProperties::Property::FailedISel))
    return false;
  LLVM_DEBUG(dbgs() << "Legalize Machine IR for: " << MF.getName() << '\n');
  init(MF);
  const TargetPassConfig &TPC = getAnalysis<TargetPassConfig>();
  MachineOptimizationRemarkEmitter MORE(MF, /*MBFI=*/nullptr);

  const size_t NumBlocks = MF.size();
  MachineRegisterInfo &MRI = MF.getRegInfo();

  // Populate Insts
  InstListTy InstList(&MF);
  ArtifactListTy ArtifactList(&MF);
  ReversePostOrderTraversal<MachineFunction *> RPOT(&MF);
  // Perform legalization bottom up so we can DCE as we legalize.
  // Traverse BB in RPOT and within each basic block, add insts top down,
  // so when we pop_back_val in the legalization process, we traverse bottom-up.
  for (auto *MBB : RPOT) {
    if (MBB->empty())
      continue;
    for (MachineInstr &MI : *MBB) {
      // Only legalize pre-isel generic instructions: others don't have types
      // and are assumed to be legal.
      if (!isPreISelGenericOpcode(MI.getOpcode()))
        continue;
      if (isArtifact(MI))
        ArtifactList.insert(&MI);
      else
        InstList.insert(&MI);
    }
  }
  LegalizerWorkListManager WorkListObserver(InstList, ArtifactList);
  LegalizerHelper Helper(MF, WorkListObserver);
  const LegalizerInfo &LInfo(Helper.getLegalizerInfo());
  LegalizationArtifactCombiner ArtCombiner(Helper.MIRBuilder, MF.getRegInfo(), LInfo);
  auto RemoveDeadInstFromLists = [&WorkListObserver](MachineInstr *DeadMI) {
    WorkListObserver.erasingInstr(*DeadMI);
  };
  bool Changed = false;
  do {
    while (!InstList.empty()) {
      MachineInstr &MI = *InstList.pop_back_val();
      assert(isPreISelGenericOpcode(MI.getOpcode()) && "Expecting generic opcode");
      if (isTriviallyDead(MI, MRI)) {
        LLVM_DEBUG(dbgs() << MI << "Is dead; erasing.\n");
        MI.eraseFromParentAndMarkDBGValuesForRemoval();
        continue;
      }

      // Do the legalization for this instruction.
      auto Res = Helper.legalizeInstrStep(MI);
      // Error out if we couldn't legalize this instruction. We may want to
      // fall back to DAG ISel instead in the future.
      if (Res == LegalizerHelper::UnableToLegalize) {
        Helper.MIRBuilder.stopObservingChanges();
        reportGISelFailure(MF, TPC, MORE, "gisel-legalize",
                           "unable to legalize instruction", MI);
        return false;
      }
      Changed |= Res == LegalizerHelper::Legalized;
    }
    while (!ArtifactList.empty()) {
      MachineInstr &MI = *ArtifactList.pop_back_val();
      assert(isPreISelGenericOpcode(MI.getOpcode()) && "Expecting generic opcode");
      if (isTriviallyDead(MI, MRI)) {
        LLVM_DEBUG(dbgs() << MI << "Is dead\n");
        RemoveDeadInstFromLists(&MI);
        MI.eraseFromParentAndMarkDBGValuesForRemoval();
        continue;
      }
      SmallVector<MachineInstr *, 4> DeadInstructions;
      if (ArtCombiner.tryCombineInstruction(MI, DeadInstructions)) {
        for (auto *DeadMI : DeadInstructions) {
          LLVM_DEBUG(dbgs() << *DeadMI << "Is dead\n");
          RemoveDeadInstFromLists(DeadMI);
          DeadMI->eraseFromParentAndMarkDBGValuesForRemoval();
        }
        Changed = true;
        continue;
      }
      // If this was not an artifact (that could be combined away), this might
      // need special handling. Add it to InstList, so when it's processed
      // there, it has to be legal or specially handled.
      else
        InstList.insert(&MI);
    }
  } while (!InstList.empty());

  // For now don't support if new blocks are inserted - we would need to fix the
  // outerloop for that.
  if (MF.size() != NumBlocks) {
    MachineOptimizationRemarkMissed R("gisel-legalize", "GISelFailure",
                                      MF.getFunction().getSubprogram(),
                                      /*MBB=*/nullptr);
    R << "inserting blocks is not supported yet";
    reportGISelFailure(MF, TPC, MORE, R);
    return false;
  }

  return Changed;
}
