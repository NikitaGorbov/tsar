//===--- tsar_private.cpp - Private Variable Analyzer -----------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file implements passes to analyze variables which can be privatized.
//
//===----------------------------------------------------------------------===//

#include <llvm/Pass.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Transforms/Utils/PromoteMemToReg.h>
#include <llvm/Config/llvm-config.h>
#if (LLVM_VERSION_MAJOR < 4 && LLVM_VERSION_MINOR < 5)
#include <llvm/Analysis/Dominators.h>
#else
#include <llvm/IR/Dominators.h>
#endif



#include <llvm/IR/LLVMContext.h>
#include <llvm/Support/raw_ostream.h>
#if (LLVM_VERSION_MAJOR < 4 && LLVM_VERSION_MINOR < 5)
#include <llvm/DebugInfo.h>
#else
#include <llvm/IR/DebugInfo.h>
#endif

#include <utility.h>
#include "tsar_private.h"
#include "tsar_graph.h"
#include "tsar_pass.h"

#include <declaration.h>
#include "tsar_dbg_output.h"

using namespace llvm;
using namespace tsar;

char PrivateRecognitionPass::ID = 0;
INITIALIZE_PASS_BEGIN(PrivateRecognitionPass, "private",
                      "Private Variable Analysis", true, true)
#if (LLVM_VERSION_MAJOR < 4 && LLVM_VERSION_MINOR < 5)
INITIALIZE_PASS_DEPENDENCY(DominatorTree)
#else
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
#endif
INITIALIZE_PASS_DEPENDENCY(LoopInfo)
INITIALIZE_PASS_END(PrivateRecognitionPass, "private",
                    "Private Variable Analysis", true, true)

bool PrivateRecognitionPass::runOnFunction(Function &F) {
  AllocaSet AnlsAllocas;
  LoopInfo &LpInfo = getAnalysis<LoopInfo>();
#if (LLVM_VERSION_MAJOR < 4 && LLVM_VERSION_MINOR < 5)
  DominatorTreeBase<BasicBlock> &DomTree = *(getAnalysis<DominatorTree>().DT);
#else
  DominatorTree &DomTree = getAnalysis<DominatorTreeWrapperPass>().getDomTree();
#endif
  BasicBlock &BB = F.getEntryBlock();
  for (BasicBlock::iterator I = BB.begin(), EI = --BB.end(); I != EI; ++I) {
    AllocaInst *AI = dyn_cast<AllocaInst>(I);
    if (AI && isAllocaPromotable(AI)) 
      AnlsAllocas.insert(AI);
  }
  if (AnlsAllocas.empty())
    return false;
  for (Loop *L : LpInfo) {
    PrivateLNode DFG(L, AnlsAllocas, mPrivates, mLastPrivates,
                     mSecondToLastPrivates, mDynamicPrivates);
    solveLoopDataFlow(&DFG);
  }
  for_each(LpInfo, [this](Loop *L) {
    DebugLoc loc = L->getStartLoc();
    Base::Text Offset(L->getLoopDepth(), ' ');
    errs() << Offset;
    loc.print(getGlobalContext(), errs());
    errs() << "\n";
    errs() << Offset << " last privates:\n";
    auto LP = getLastPrivatesFor(L);
    for (auto I = LP.first, E = LP.second; I != E; ++I) {
      errs() << Offset << "  ";
      printAllocaSource(errs(), *I);
    }
    errs() << Offset << " second to last privates:\n";
    auto SLP = getSecondToLastPrivatesFor(L);
    for (auto I = SLP.first, E = SLP.second; I != E; ++I) {
      errs() << Offset << "  ";
      printAllocaSource(errs(), *I);
    }
    errs() << Offset << " dynamic privates:\n";
    auto DP = getDynamicPrivatesFor(L);
    for (auto I = DP.first, E = DP.second; I != E; ++I) {
      errs() << Offset << "  ";
      printAllocaSource(errs(), *I);
    }
    errs() << "\n";
  });
  return false;
}

void PrivateRecognitionPass::getAnalysisUsage(AnalysisUsage &AU) const {
#if (LLVM_VERSION_MAJOR < 4 && LLVM_VERSION_MINOR < 5)
  AU.addRequired<DominatorTree>();
#else
  AU.addRequired<DominatorTreeWrapperPass>();
#endif
  AU.addRequired<LoopInfo>();
  AU.setPreservesAll();
}

FunctionPass *llvm::createPrivateRecognitionPass() {
  return new PrivateRecognitionPass();
}

bool AllocaDFValue::intersect(const AllocaDFValue &with) {
  assert(mKind != INVALID_KIND && "Collection is corrupted!");
  assert(with.mKind != INVALID_KIND && "Collection is corrupted!");
  if (with.mKind == KIND_FULL)
    return false;
  if (mKind == KIND_FULL) {
    *this = with;
    return true;
  }
  AllocaSet PrevAllocas;
  mAllocas.swap(PrevAllocas);
  for (llvm::AllocaInst *AI : PrevAllocas) {
    if (with.mAllocas.count(AI))
      mAllocas.insert(AI);
  }
  return mAllocas.size() != PrevAllocas.size();
}

bool AllocaDFValue::merge(const AllocaDFValue &with) {
  assert(mKind != INVALID_KIND && "Collection is corrupted!");
  assert(with.mKind != INVALID_KIND && "Collection is corrupted!");
  if (mKind == KIND_FULL)
    return false;
  if (with.mKind == KIND_FULL) {
    mAllocas.clear();
    mKind = KIND_FULL;
    return true;
  }
  bool isChanged = false;
  for (llvm::AllocaInst *AI : with.mAllocas)
    isChanged = mAllocas.insert(AI) || isChanged;
  return isChanged;
}

bool AllocaDFValue::operator==(const AllocaDFValue &RHS) const {
  assert(mKind != INVALID_KIND && "Collection is corrupted!");
  assert(RHS.mKind != INVALID_KIND && "Collection is corrupted!");
  if (this == &RHS || mKind == KIND_FULL && RHS.mKind == KIND_FULL)
    return true;
  if (mKind != RHS.mKind)
    return false;
  if (mAllocas.size() != RHS.mAllocas.size())
    return false;
  for (llvm::AllocaInst *AI : mAllocas)
    if (!RHS.mAllocas.count(AI))
      return false;
  return true;
}

PrivateBBNode::PrivateBBNode(BasicBlock *BB, const AllocaSet &AnlsAllocas) :
PrivateDFNode(AnlsAllocas), BlockBase::BlockDFBase(BB) {
  for (Instruction &I : BB->getInstList()) {
    if (isa<StoreInst>(I)) {
      AllocaInst *AI = cast<AllocaInst>(I.getOperand(1));
      if (mAnlsAllocas.count(AI))
        mDefs.insert(AI);
    } else if (isa<LoadInst>(I)) {
      AllocaInst *AI = cast<AllocaInst>(I.getOperand(0));
      if (mAnlsAllocas.count(AI) && !mDefs.count(AI))
        mUses.insert(AI);
    }
  }
}

bool PrivateBBNode::transferFunction(AllocaDFValue In) {
  mIn = std::move(In);
  AllocaDFValue newOut(AllocaDFValue::emptyValue());
  newOut.insert(mDefs.begin(), mDefs.end());
  newOut.merge(mIn);
  if (mOut != newOut) {
    mOut = std::move(newOut); 
    return true;
  }
  return false;
}

void PrivateLNode::collapse() {
  // We need two types of defs:
  // * ExitingDefs is a set of must define allocas (mDefs) for the loop.
  //   These allocas always have definitions inside the loop regardless
  //   of execution paths of iterations of the loop.
  // * LatchDefs is a set of must define allocas before a branch to
  //   a next arbitrary iteration.
  AllocaDFValue ExitingDefs(topElement());
  for (PrivateDFNode *N : getExitingNodes())
    ExitingDefs.intersect(N->getOut());
  AllocaDFValue LatchDefs(topElement());
  for (PrivateDFNode *N : getLatchNodes())
    LatchDefs.intersect(N->getOut());
  AllocaSet AllNodesAccesses;
  for (PrivateDFNode *N : getNodes()) {
    // First, we calculat a set of allocas accessed in loop nodes.
    // Second, we calculate a set of allocas (mUses)
    // which get values outside the loop or from previouse loop iterations.
    // These allocas can not be privatized.
    for (AllocaInst *AI : N->getUses()) {
      AllNodesAccesses.insert(AI);
      if (!N->getIn().exist(AI))
        mUses.insert(AI);
    }
    // It is possible that some allocas are only written in the loop.
    // In this case this allocas are not located at set of node uses but
    // they are located at set of node defs.
    // We also calculate a set of must define allocas (mDefs) for the loop.
    // These allocas always have definitions inside the loop regardless 
    // of execution paths of iterations of the loop.
    for (AllocaInst *AI : N->getDefs()) {
      AllNodesAccesses.insert(AI);
      if (ExitingDefs.exist(AI))
        mDefs.insert(AI);
    }
  }
  // Calculation of a last private variables differs depending on internal
  // representation of a loop.There are two type of representations.
  // 1. The first type has a following pattern:
  //   iter: if (...) goto exit;
  //             ...
  //         goto iter;
  //   exit:
  // For example, representation of a for-loop refers to this type.
  // In this case allocas from the LatchDefs collection should be used
  // to determine candidates for last private variables. These allocas will be
  // stored in the SecondToLastPrivates collection, i.e. the last definition of
  // these allocas is executed on the second to the last loop iteration
  // (on the last iteration the loop condition check is executed only).
  // 2. The second type has a following patterm:
  //   iter:
  //             ...
  //         if (...) goto exit; else goto iter;
  //   exit:
  // For example, representation of a do-while-loop refers to this type.
  // In this case allocas from the ExitDefs collection should be used.
  // The result will be stored in the LastPrivates collection.
  // In some cases it is impossible to determine in static an iteration
  // where the last definition of an alloca have been executed. Such allocas
  // will be stored in the DynamicPrivates collection.
  // Note, in this step only candidates for last privates and privates
  // variables are calculated. The result should be corrected further.
  AllocaSet *LastPrivates = new AllocaSet();
  AllocaSet *SecondToLastPrivates = new AllocaSet();
  AllocaSet *DynamicPrivates = new AllocaSet();
  for (AllocaInst *AI : AllNodesAccesses)
    if (!mUses.count(AI)) {
      if (mDefs.count(AI))
        LastPrivates->insert(AI);
      else if (LatchDefs.exist(AI))
        SecondToLastPrivates->insert(AI);
      else
        DynamicPrivates->insert(AI);
    }
  mLastPrivates.insert(std::make_pair(getLoop(), LastPrivates));
  mSecondToLastPrivates.insert(std::make_pair(getLoop(), SecondToLastPrivates));
  mDynamicPrivates.insert(std::make_pair(getLoop(), DynamicPrivates));
}
