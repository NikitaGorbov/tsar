//===--- tsar_finliner.h - Frontend Inliner (clang) -------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file declares classes and methods necessary for function source-level 
/// inlining.
///
//===----------------------------------------------------------------------===//


#ifndef TSAR_FUNCTION_INLINER_H
#define TSAR_FUNCTION_INLINER_H

#include "AnalysisWrapperPass.h"
#include "tsar_pass.h"
#include "tsar_query.h"
#include "tsar_action.h"
#include "tsar_transformation.h"

#include <set>

#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Lex/Lexer.h>
#include <clang/Lex/Pragma.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <llvm/IR/Module.h>

namespace tsar {

class InlinePragmaHandler : public clang::PragmaHandler {
private:
  /// This set contains locations of each handled pragma.
  std::set<unsigned> mPragmaLocSet;

public:
  /// Creates handler.
  InlinePragmaHandler() : clang::PragmaHandler("inline") {}

  void HandlePragma(clang::Preprocessor& PP,
    clang::PragmaIntroducerKind Introducer, clang::Token& FirstToken) override;
};

class FunctionInlinerQueryManager : public QueryManager {
  void run(llvm::Module *M, TransformationContext *Ctx) override;
  bool beginSourceFile(clang::CompilerInstance& CI, llvm::StringRef File);

private:
  InlinePragmaHandler* mIPH;
};

struct FunctionInlineInfo : private bcl::Uncopyable {
  // place data for further passes
};
}

namespace llvm {
using FunctionInlinerImmutableWrapper = AnalysisWrapperPass<tsar::FunctionInlineInfo>;

class FunctionInlinerImmutableStorage :
  public ImmutablePass, private bcl::Uncopyable {
public:
  static char ID;

  FunctionInlinerImmutableStorage() : ImmutablePass(ID) {}

  const tsar::FunctionInlineInfo& getFunctionInlineInfo() const noexcept {
    return mFunctionInlineInfo;
  }

  tsar::FunctionInlineInfo& getFunctionInlineInfo() noexcept {
    return mFunctionInlineInfo;
  }

private:
  tsar::FunctionInlineInfo mFunctionInlineInfo;
};

class FunctionInlinerPass :
  public ModulePass, private bcl::Uncopyable {
public:
  static char ID;

  FunctionInlinerPass() : ModulePass(ID) {
    initializeFunctionInlinerPassPass(*PassRegistry::getPassRegistry());
  }

  bool runOnModule(llvm::Module& M) override;

  void getAnalysisUsage(AnalysisUsage& AU) const override;
};
}

namespace detail {

/// Contains information required for correct and complete function body
/// instantiation and access methods to it
class Template {
public:
  const clang::FunctionDecl* getFuncDecl() const {
    return mFuncDecl;
  }

  void setFuncDecl(const clang::FunctionDecl* FD) {
    mFuncDecl = FD;
    return;
  }

  void addParmRef(const clang::ParmVarDecl* PVD, const clang::DeclRefExpr* DRE) {
    mParmRefs[PVD].push_back(DRE);
    return;
  }

  std::vector<const clang::DeclRefExpr*> getParmRefs(const clang::ParmVarDecl* PVD) const {
    auto pr = [PVD](const std::pair<const clang::ParmVarDecl*,
      std::vector<const clang::DeclRefExpr*>>&lhs) -> bool {
      return lhs.first == PVD;
    };
    if (std::find_if(std::begin(mParmRefs), std::end(mParmRefs), pr)
      == std::end(mParmRefs)) {
      return std::vector<const clang::DeclRefExpr*>();
    } else {
      return mParmRefs.at(PVD);
    }
  }

  void addRetStmt(const clang::ReturnStmt* RS) {
    mRSs.insert(RS);
    return;
  }

  std::set<const clang::ReturnStmt*> getRetStmts() const {
    return mRSs;
  }

  bool isSingleReturn() const {
    return mIsSingleReturn;
  }

  void setSingleReturn(bool isSingleReturn) {
    mIsSingleReturn = isSingleReturn;
    return;
  }

private:
  /// mFuncDecl == nullptr <-> instantiation is disabled for all calls
  const clang::FunctionDecl* mFuncDecl;
  std::map<const clang::ParmVarDecl*, std::vector<const clang::DeclRefExpr*>> mParmRefs;
  std::set<const clang::ReturnStmt*> mRSs;

  bool mIsSingleReturn;
};

/// Represents one specific place in user source code where one of specified
/// functions (for inlining) is called
struct TemplateInstantiation {
  const clang::FunctionDecl* mFuncDecl;
  const clang::Stmt* mStmt;
  const clang::CallExpr* mCallExpr;
  /// mTemplate == nullptr <-> instantiation is disabled for this call
  Template* mTemplate;
};

inline bool operator==(
  const TemplateInstantiation& lhs, const TemplateInstantiation& rhs) {
  return lhs.mFuncDecl == rhs.mFuncDecl
    && lhs.mStmt == rhs.mStmt
    && lhs.mCallExpr == rhs.mCallExpr
    && lhs.mTemplate == rhs.mTemplate;
}

}

namespace tsar {

/// This class provides both AST traversing and source code buffer modification
/// (through Rewriter API). Note that the only result of its work - modified
/// Rewriter (buffer) object inside passed Transformation Context.
class FInliner :
  public clang::RecursiveASTVisitor<FInliner>,
  public clang::ASTConsumer {
public:
  explicit FInliner(tsar::TransformationContext* TfmCtx)
    : mTransformContext(TfmCtx), mContext(TfmCtx->getContext()),
    mRewriter(TfmCtx->getRewriter()),
    mSourceManager(TfmCtx->getRewriter().getSourceMgr()) {
  }

  bool VisitFunctionDecl(clang::FunctionDecl* FD);

  bool VisitReturnStmt(clang::ReturnStmt* RS);

  bool VisitExpr(clang::Expr* E);

  /// Traverses AST, collects necessary information using overriden methods above
  /// and applies it to source code using private methods below
  void HandleTranslationUnit(clang::ASTContext& Context);

private:
  /// Constructs correct language declaration of \p identifier with \p type
  /// Uses bruteforce with linear complexity dependent on number of tokens
  /// in \p type where token is non-whitespace character or special sequence.
  /// \p context is a string containing declarations used in case of referencing
  /// in \p type.
  /// \returns vector of tokens which can be transformed to text string for
  /// insertion into source code
  /// SLOW!
  std::vector<std::string> construct(
    const std::string& type, const std::string& identifier,
    const std::string& context, std::map<std::string, std::string>& replacements);

  /// Does instantiation of \p TI using \p args generating non-collidable
  /// identifiers/labels if necessary. Since instantiation is recursive,
  /// collects all visible and newly created named declarations in \p decls
  /// to avoid later possible collisions.
  /// \returns text of instantiated function body and result identifier
  std::pair<std::string, std::string> compile(
    const ::detail::TemplateInstantiation& TI,
    const std::vector<std::string>& args,
    std::set<std::string>& decls);

  std::string getSourceText(const clang::SourceRange& SR) const;

  /// get raw tokens (preserves order)
  std::vector<clang::Token> getRawTokens(const clang::SourceRange& SR) const;

  std::set<std::string> getIdentifiers(const clang::Decl* D) const;
  std::set<std::string> getIdentifiers(const clang::TagDecl* TD) const;

  /// T must provide getSourceRange() method
  template<typename T>
  clang::SourceRange getRange(T* node) const;

  clang::SourceLocation getLoc(clang::SourceLocation SL) const;

  /// Merges \p _Cont of tokens to string using \p delimiter between each pair
  /// of tokens.
  template<typename _Container>
  std::string join(const _Container& _Cont, const std::string& delimiter) const;

  /// Exchanges contents of passed objects - useful if specified objects haven't
  /// necessary operators available (e.g. private operator=, etc).
  /// Used only for turning off llvm::errs() during bruteforce in construct() -
  /// each variant is attempted for parsing into correct AST (only one variant
  /// gives correct AST) with multiple warnings and errors.
  template<typename T>
  void swap(T& lhs, T& rhs) const;

  /// Appends numeric suffix to the end of \p prefix, avoids collision using
  /// \p identifiers
  /// \returns new identifier (which is already inserted into identifiers)
  std::string addSuffix(
    const std::string& prefix, std::set<std::string>& identifiers) const;

  /// Splits string \p s into tokens using pattern \p p
  std::vector<std::string> tokenize(std::string s, std::string p) const;

  /// if \p S is declaration statement we shouldn't place braces if
  /// declarations were referenced outside it
  bool requiresBraces(const clang::FunctionDecl* FD, const clang::Stmt* S);

  /// Local matcher to find correct node in AST during construct()
  class : public clang::ast_matchers::MatchFinder::MatchCallback {
  public:
    void run(const clang::ast_matchers::MatchFinder::MatchResult& MatchResult) {
      const clang::VarDecl* VD
        = MatchResult.Nodes.getNodeAs<clang::VarDecl>("varDecl");
      if (!VD) {
        return;
      }
      if (VD->getName() == identifier
        && processor(VD->getType().getAsString()) == type) {
        ++count;
      }
      return;
    }
    void setParameters(const std::string& type, const std::string& identifier,
      const std::function<std::string(const std::string&)>& processor) {
      this->type = type;
      this->identifier = identifier;
      this->processor = processor;
      return;
    }
    int getCount(void) const {
      return count;
    }
    void initCount(void) {
      this->count = 0;
      return;
    }
  private:
    std::string type;
    std::string identifier;
    std::function<std::string(const std::string&)> processor;
    int count;
  } varDeclHandler;

  // [C99 6.7.2, 6.7.3]
  const std::vector<std::string> mKeywords = { "register",
    "void", "char", "short", "int", "long", "float", "double",
    "signed", "unsigned", "_Bool", "_Complex", "struct", "union", "enum",
    "typedef", "const", "restrict", "volatile" };
  const std::string mIdentifierPattern = "[[:alpha:]_]\\w*";

  tsar::TransformationContext* mTransformContext;

  clang::ASTContext& mContext;
  clang::SourceManager& mSourceManager;
  clang::Rewriter& mRewriter;
  //clang::Preprocessor& mPreprocessor;

  /// last seen function decl (with body we are currently in)
  clang::FunctionDecl* mCurrentFD;

  /// for statements - for detecting call expressions which can be inlined
  std::vector<const clang::Stmt*> mFSs;

  std::set<std::string> mGlobalIdentifiers;
  std::map<const clang::FunctionDecl*, std::set<std::string>> mExtIdentifiers, mIntIdentifiers;
  std::map<std::string, std::set<const clang::Decl*>> mOutermostDecls;
  std::map<const clang::FunctionDecl*, std::set<const clang::Decl*>> mForwardDecls;
  std::map<const clang::FunctionDecl*, std::set<const clang::Stmt*>> mUnreachableStmts;
  std::map<const clang::FunctionDecl*, std::set<const clang::Expr*>> mExprs;

  std::map<const clang::FunctionDecl*, ::detail::Template> mTs;
  std::map<const clang::FunctionDecl*, std::vector<::detail::TemplateInstantiation>> mTIs;
};

}

#endif//TSAR_FUNCTION_INLINER_H
