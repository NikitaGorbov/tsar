//===-- Matcher.h --------- High and Low Level Matcher (Clang) --*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2018 DVM System Group
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//
//===----------------------------------------------------------------------===//
//
// This file defines general classes and functions to match some entities
// (loops, variables, etc.) in a source high-level code and appropriate entities
// (loops, allocas, etc.) in low-level LLVM IR.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_CLANG_MATCHER_H
#define TSAR_CLANG_MATCHER_H

#include "tsar/Analysis/AST/Matcher.h"
#include "tsar/Support/Clang/PresumedLocationInfo.h"
#include <clang/Basic/SourceManager.h>

namespace tsar {
/// This is a base class which is inherited to match different entities (loops,
/// variables, etc.).
template <typename ImplTy, typename IRItemTy, typename ASTItemTy,
          typename IRLocationTy = llvm::DILocation *,
          typename IRLocationMapInfo = DILocationMapInfo,
          typename RawLocationTy = unsigned,
          typename RawLocationMapInfo = llvm::DenseMapInfo<RawLocationTy>,
          typename MatcherTy =
              Bimap<bcl::tagged<ASTItemTy, AST>, bcl::tagged<IRItemTy, IR>>,
          typename UnmatchedASTSetTy = llvm::DenseSet<ASTItemTy>>
class ClangMatchASTBase
    : public MatchASTBase<ImplTy, IRItemTy, ASTItemTy, clang::SourceLocation,
                          IRLocationTy, IRLocationMapInfo, RawLocationTy,
                          RawLocationMapInfo, MatcherTy, UnmatchedASTSetTy> {

  using BaseT = MatchASTBase<ImplTy, IRItemTy, ASTItemTy, clang::SourceLocation,
                             IRLocationTy, IRLocationMapInfo, RawLocationTy,
                             RawLocationMapInfo, MatcherTy, UnmatchedASTSetTy>;

public:
  using typename BaseT::LocToASTMap;
  using typename BaseT::LocToIRMap;
  using typename BaseT::Matcher;
  using typename BaseT::UnmatchedASTSet;

  /// \brief Constructor.
  ///
  /// \param[in] SrcMgr Clang source manager to deal with locations.
  /// \param[in, out] M Representation of match.
  /// \param[in, out] UM Storage for unmatched ast entities.
  /// \param[in, out] LocToIR Map from entity location to a queue
  /// of IR entities.
  /// \param[in, out] LocToMacro A map form entity expansion location to a queue
  /// of AST entities. All entities explicitly (not implicit loops) defined in
  /// macros is going to store in this map. The key in this map is a raw
  /// encoding for expansion location.
  ClangMatchASTBase(clang::SourceManager &SrcMgr, Matcher &M,
                    UnmatchedASTSet &UM, LocToIRMap &LocToIR,
                    LocToASTMap &LocToMacro)
      : BaseT(M, UM, LocToIR, LocToMacro), mSrcMgr(&SrcMgr) {}

  /// Finds low-level representation of an entity at the specified location.
  ///
  /// \return Iterator to an element in LocToIRMap.
  typename LocToIRMap::iterator findItrForLocation(clang::SourceLocation Loc) {
    if (Loc.isInvalid())
      return BaseT::mLocToIR->end();
    auto PLoc{mSrcMgr->getPresumedLoc(Loc, false)};
    return BaseT::mLocToIR->find_as(PLoc);
  }

  clang::PresumedLoc getPresumedLoc(RawLocationTy Loc) {
    return mSrcMgr->getPresumedLoc(
        clang::SourceLocation::getFromRawEncoding(Loc), false);
  }

protected:
  clang::SourceManager *mSrcMgr;
};
} // namespace tsar
#endif // TSAR_CLANG_MATCHER_H
