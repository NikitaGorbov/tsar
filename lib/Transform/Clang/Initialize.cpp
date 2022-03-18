//=== Initialize.cpp (Clang) --*- C++ -*===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2022 DVM System Group
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either expRess or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//===----------------------------------------------------------------------===//
//
// This file implements a pass to initialize arrays and variables.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Clang/GlobalInfoExtractor.h"
#include "tsar/Analysis/Clang/NoMacroAssert.h"
#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/Core/Query.h"
#include "tsar/Frontend/Clang/TransformationContext.h"
#include "tsar/Support/Clang/Diagnostic.h"
#include "tsar/Support/MetadataUtils.h"
#include "tsar/Transform/Clang/Passes.h"
#include <bcl/utility.h>
#include <clang/AST/Decl.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <clang/Basic/SourceLocation.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/Pass.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>
#include <stack>

using namespace clang;
using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "clang-init"

static int getDimensionsNum(QualType QT, std::vector<int> &DefaultDimensions) {
  int Res = 0;
  bool SizeIsKnown = true;
  while (1) {
    if (QT->isArrayType()) {
      auto AT = QT->getAsArrayTypeUnsafe();
      auto T = dyn_cast_or_null<ConstantArrayType>(AT);
      if (SizeIsKnown && T) { // get size
        uint64_t Dim = T->getSize().getLimitedValue();
        DefaultDimensions.push_back(Dim);
      }
      QT = AT->getElementType();
      Res++;
    } else if (QT->isPointerType()) {
      SizeIsKnown = false;
      QT = QT->getPointeeType();
      Res++;
    } else {
      return Res;
    }
  }
}

namespace {

class ClangInitialize : public FunctionPass, private bcl::Uncopyable {
public:
  static char ID;

  ClangInitialize() : FunctionPass(ID) {
    initializeClangInitializePass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
};

// contains information about variables in
// initialize clause
struct Vars {
  bool RvalIsArray = false;
  std::string LvalName;
  std::string RvalName;
  int DimensionsNum;
  std::vector<int> Dimensions;
  std::vector<int> DefaultDimensions;
};
} // namespace

char ClangInitialize::ID = 0;

INITIALIZE_PASS_IN_GROUP_BEGIN(ClangInitialize, "initialize",
                               "Initialize variables in for", false, false,
                               TransformationQueryManager::getPassRegistry())
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(ClangGlobalInfoPass)
INITIALIZE_PASS_IN_GROUP_END(ClangInitialize, "initialize",
                             "Initialize variables in for", false, false,
                             TransformationQueryManager::getPassRegistry())

namespace {

class DeclVisitor : public RecursiveASTVisitor<DeclVisitor> {

  struct DeclarationInfo {
    DeclarationInfo(Stmt *S) : Scope(S) {}

    Stmt *Scope;
  };

public:
  explicit DeclVisitor(ClangTransformationContext &TfmCtx,
                       const ASTImportInfo &ImportInfo)
      : mTfmCtx(&TfmCtx), mImportInfo(ImportInfo),
        mRewriter(TfmCtx.getRewriter()), mContext(TfmCtx.getContext()),
        mSrcMgr(mRewriter.getSourceMgr()), mLangOpts(mRewriter.getLangOpts()) {}

  bool TraverseStmt(Stmt *S) {
    if (!S)
      return true;

    bool ast = false;
    Pragma P(*S);

    if (findClause(P, ClauseId::Initialize, mClauses)) {
      auto locationForInits = S->getEndLoc();
      mIsInPragma = true;
      ast = RecursiveASTVisitor::TraverseStmt(S);
      mIsInPragma = false;
      std::vector<std::string> inits;
      while (mVarStack.size()) {
        std::string txtStr, beforeFor, forBody, lval, rval, indeces;
        if (mVarStack.top().DimensionsNum) { // lvalue is array
          if (mVarStack.top().Dimensions.size() < mVarStack.top().DimensionsNum) {
            if (mVarStack.top().DefaultDimensions.size() ==
                mVarStack.top().DimensionsNum) {
              mVarStack.top().Dimensions = mVarStack.top().DefaultDimensions;
            } else {
              mVarStack.pop();
              continue; // dimensions ar mandatory for arrays, skip
            }           // initialization if no dimensions found
          }
          lval = mVarStack.top().LvalName;
          rval = mVarStack.top().RvalName;
          for (auto it{mVarStack.top().Dimensions.begin()},
               EI{mVarStack.top().Dimensions.end()};
               it != EI; ++it) {
            int intCounter = it - mVarStack.top().Dimensions.begin();
            std::string strCounter = "i" + std::to_string(intCounter);
            indeces += "[" + strCounter + "]";
            txtStr += "for (int " + strCounter + " = 0; " + strCounter + " < " +
                      std::to_string(*it) + "; " + strCounter + "++) {\n";
          }
          if (mVarStack.top().RvalIsArray) {
            rval += indeces;
          }
          lval += indeces;
          forBody = lval + " = " + rval + ";\n";
          txtStr += forBody;
          for (int i = 0; i < mVarStack.top().DimensionsNum; i++) {
            txtStr += "}\n";
          }
        } else { // Initialize non-array variable
          txtStr =
              mVarStack.top().LvalName + " = " + mVarStack.top().RvalName + ";\n";
        }
        inits.push_back(txtStr);
        mVarStack.pop();
      }

      llvm::SmallVector<clang::CharSourceRange, 8> ToRemove;
      auto IsPossible = pragmaRangeToRemove(P, mClauses, mSrcMgr, mLangOpts,
                                            mImportInfo, ToRemove);
      if (!IsPossible.first)
        if (IsPossible.second & PragmaFlags::IsInMacro)
          toDiag(mSrcMgr.getDiagnostics(), mClauses.front()->getBeginLoc(),
                 tsar::diag::warn_remove_directive_in_macro);
        else if (IsPossible.second & PragmaFlags::IsInHeader)
          toDiag(mSrcMgr.getDiagnostics(), mClauses.front()->getBeginLoc(),
                 tsar::diag::warn_remove_directive_in_include);
        else
          toDiag(mSrcMgr.getDiagnostics(), mClauses.front()->getBeginLoc(),
                 tsar::diag::warn_remove_directive);
      Rewriter::RewriteOptions RemoveEmptyLine;
      /// TODO (kaniandr@gmail.com): it seems that RemoveLineIfEmpty is
      /// set to true then removing (in RewriterBuffer) works incorrect.
      RemoveEmptyLine.RemoveLineIfEmpty = false;
      for (auto SR : ToRemove)
        mRewriter.RemoveText(SR, RemoveEmptyLine); // delete each range

      for (std::vector<std::string>::iterator it = inits.begin();
           it != inits.end(); ++it) {
        mRewriter.InsertTextAfterToken(locationForInits, *it);
      }
      return ast;
    }
    return RecursiveASTVisitor::TraverseStmt(S);
  }

  bool TraverseDeclRefExpr(clang::DeclRefExpr *Ex) {
    std::string VarName;
    if (mIsInPragma) {
      if (mWaitingForDimensions &&
          mCurDimensionNum == mVarStack.top().DimensionsNum) {
        mWaitingForDimensions = false;
        mCurDimensionNum = 0;
      }
      if (auto *Var{dyn_cast<VarDecl>(Ex->getDecl())}) {
        VarName = Var->getName();
      }
      if (mWaitingForVar) { // get lvalue
        ValueDecl *VD = Ex->getDecl();
        QualType QT = VD->getType();
        Vars Tmp;

        Tmp.LvalName = VarName;
        mVarStack.push(Tmp);
        mVarStack.top().DimensionsNum =
            getDimensionsNum(QT, mVarStack.top().DefaultDimensions);
        mWaitingForDimensions = false;
      } else { // get rvalue
        ValueDecl *VD = Ex->getDecl();
        QualType QT = VD->getType();
        if (QT->isArrayType() || QT->isPointerType()) {
          mVarStack.top().RvalIsArray = true;
        }
        mVarStack.top().RvalName = VarName;
        if (mVarStack.top().DimensionsNum > 0) {
          mWaitingForDimensions = true;
        }
      }
      mWaitingForVar = !mWaitingForVar;
    }
    return RecursiveASTVisitor::TraverseDeclRefExpr(Ex);
  }

  bool TraverseIntegerLiteral(IntegerLiteral *IL) {

    if (mIsInPragma) {
      if (mWaitingForDimensions &&
          mCurDimensionNum == mVarStack.top().DimensionsNum) {
        mWaitingForDimensions = false;
        mCurDimensionNum = 0;
      }
      int Val = IL->getValue().getLimitedValue();
      if (mWaitingForDimensions) {
        if (mVarStack.size()) {
          mVarStack.top().Dimensions.push_back(Val);
          mCurDimensionNum++;
        }
      } else if (!mWaitingForVar) { // get rvalue
        mVarStack.top().RvalName = std::to_string(Val);
        mWaitingForVar = !mWaitingForVar;
        if (mVarStack.top().DimensionsNum > 0) {
          mWaitingForDimensions = true;
        }
      }
    }
    return RecursiveASTVisitor::TraverseIntegerLiteral(IL);
  }

private:
  /// Return current scope.
  Stmt *getScope() {
    for (auto I = mScopes.rbegin(), EI = mScopes.rend(); I != EI; ++I)
      if (isa<ForStmt>(*I) || isa<CompoundStmt>(*I))
        return *I;
    return nullptr;
  }

  bool mIsInPragma = false;
  bool mWaitingForVar = true;
  bool mWaitingForDimensions = false;
  int mCurDimensionNum = 0;
  std::vector<Stmt *> mScopes;
  ClangTransformationContext *mTfmCtx;
  const ASTImportInfo &mImportInfo;
  Rewriter &mRewriter;
  ASTContext &mContext;
  SourceManager &mSrcMgr;
  const LangOptions &mLangOpts;
  SmallVector<Stmt *, 1> mClauses;
  std::stack<Vars> mVarStack;
};
} // namespace

bool ClangInitialize::runOnFunction(Function &F) {
  auto *M = F.getParent();
  auto *DISub{findMetadata(&F)};
  if (!DISub)
    return false;
  auto *CU{DISub->getUnit()};
  if (isC(CU->getSourceLanguage()) && isCXX(CU->getSourceLanguage()))
    return false;
  auto &TfmInfo{getAnalysis<TransformationEnginePass>()};
  auto *TfmCtx{TfmInfo ? dyn_cast_or_null<ClangTransformationContext>(
                             TfmInfo->getContext(*CU))
                       : nullptr};
  if (!TfmCtx || !TfmCtx->hasInstance()) {
    F.getContext().emitError(
        "cannot transform sources"
        ": transformation context is not available for the '" +
        F.getName() + "' function");
    return false;
  }
  auto FuncDecl = TfmCtx->getDeclForMangledName(F.getName());
  if (!FuncDecl)
    return false;
  ASTImportInfo ImportStub;
  const auto *ImportInfo = &ImportStub;
  if (auto *ImportPass = getAnalysisIfAvailable<ImmutableASTImportInfoPass>())
    ImportInfo = &ImportPass->getImportInfo();
  auto &GIP = getAnalysis<ClangGlobalInfoPass>();
  auto *GI{GIP.getGlobalInfo(TfmCtx)};
  assert(GI && "Global declaration must be collected!");
  DeclVisitor Visitor(*TfmCtx, *ImportInfo);

  Visitor.TraverseDecl(FuncDecl);
  return false;
}

void ClangInitialize::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<ClangGlobalInfoPass>();
  AU.setPreservesAll();
}

FunctionPass *llvm::createClangInitialize() { return new ClangInitialize(); }
