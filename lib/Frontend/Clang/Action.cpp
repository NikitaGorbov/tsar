//===--- Action.cpp --------- TSAR Frontend Action --------------*- C++ -*-===//
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
//===----------------------------------------------------------------------===//
//
// This file implements front-end action which is necessary to analyze and
// transform sources.
//
//===----------------------------------------------------------------------===//

#include "tsar/Frontend/Clang/Action.h"
#include "tsar/Frontend/Clang/FrontendActions.h"
#include "tsar/Frontend/Clang/TransformationContext.h"
#include "tsar/Core/Query.h"
#include "tsar/Core/TransformationContext.h"
#include "tsar/Core/tsar-config.h"
#include "tsar/Support/MetadataUtils.h"
#include "tsar/Support/SMStringSocket.h"
#include <bcl/IntrusiveConnection.h>
#include <bcl/Json.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/CodeGen/ModuleBuilder.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendDiagnostic.h>
#include <clang/Sema/Sema.h>
#include <clang/Serialization/ASTReader.h>
#include <clang/Tooling/CompilationDatabase.h>
#ifdef FLANG_FOUND
# include "tsar/Frontend/Flang/TransformationContext.h"
# include <flang/Parser/parsing.h>
# include <flang/Semantics/semantics.h>
#endif
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/Timer.h>
#include <memory>

using namespace clang;
using namespace clang::tooling;
using namespace llvm;
using namespace tsar;

namespace clang {
/// This consumer builds LLVM IR for the specified file and launch analysis of
/// the LLVM IR.
class AnalysisConsumer : public ASTConsumer {
public:
  /// Constructor.
  AnalysisConsumer(CompilerInstance &CI, StringRef InFile,
    TransformationInfo *TfmInfo, QueryManager &QM)
    : mLLVMIRGeneration(
      "mLLVMIRGeneration",
      "LLVM IR Generation Time"
    ),
    mCI(&CI), mASTContext(nullptr), mLLVMContext(new LLVMContext),
    mGen(CreateLLVMCodeGen(CI.getDiagnostics(), InFile,
      CI.getHeaderSearchOpts(), CI.getPreprocessorOpts(),
      CI.getCodeGenOpts(), *mLLVMContext)),
    mTransformInfo(TfmInfo), mQueryManager(&QM) {
  }

  void HandleCXXStaticMemberVarInstantiation(VarDecl *VD) override {
    mGen->HandleCXXStaticMemberVarInstantiation(VD);
  }

  void Initialize(ASTContext &Ctx) override {
    if (mASTContext) {
      assert(mASTContext == &Ctx &&
        "Existed context must be equal with the specified!");
      return;
    }
    mASTContext = &Ctx;
    if (llvm::TimePassesIsEnabled)
      mLLVMIRGeneration.startTimer();
    mGen->Initialize(Ctx);
    mModule.reset(mGen->GetModule());
    if (llvm::TimePassesIsEnabled)
      mLLVMIRGeneration.stopTimer();
  }

  bool HandleTopLevelDecl(DeclGroupRef D) override {
    PrettyStackTraceDecl CrashInfo(*D.begin(), SourceLocation(),
      mASTContext->getSourceManager(), "LLVM IR generation of declaration");
    if (llvm::TimePassesIsEnabled)
      mLLVMIRGeneration.startTimer();
    mGen->HandleTopLevelDecl(D);
    if (llvm::TimePassesIsEnabled)
      mLLVMIRGeneration.stopTimer();
    return true;
  }

  void HandleInlineFunctionDefinition(FunctionDecl *D) override {
    PrettyStackTraceDecl CrashInfo(D, SourceLocation(),
      mASTContext->getSourceManager(), "LLVM IR generation of inline method");
    if (llvm::TimePassesIsEnabled)
      mLLVMIRGeneration.startTimer();
    mGen->HandleInlineFunctionDefinition(D);
    if (llvm::TimePassesIsEnabled)
      mLLVMIRGeneration.stopTimer();
  }

  void HandleTranslationUnit(ASTContext &ASTCtx) override {
    {
      PrettyStackTraceString CrashInfo("Per-file LLVM IR generation");
      if (llvm::TimePassesIsEnabled)
        mLLVMIRGeneration.startTimer();
      mGen->HandleTranslationUnit(ASTCtx);
      if (llvm::TimePassesIsEnabled)
        mLLVMIRGeneration.stopTimer();
    }
    // Silently ignore if we weren't initialized for some reason.
    if (!mModule)
      return;
    // Make sure IR generation is happy with the module. This is released by
    // the module provider.
    llvm::Module *M = mGen->ReleaseModule();
    if (!M) {
      // The module has been released by IR gen on failures, do not double
      // free.
      mModule.release();
      return;
    }
    assert(mModule.get() == M &&
      "Unexpected module change during IR generation");
    Timer LLVMIRAnalysis(
      "LLVMIRAnalysis",
      "LLVM IR Analysis Time");
    if (llvm::TimePassesIsEnabled)
      LLVMIRAnalysis.startTimer();
    if (mTransformInfo) {
      auto CUs = M->getNamedMetadata("llvm.dbg.cu");
      if (CUs->getNumOperands() == 1) {
        auto *CU = cast<DICompileUnit>(*CUs->op_begin());
        IntrusiveRefCntPtr<TransformationContextBase> TfmCtx{
            new ClangTransformationContext{*mCI, ASTCtx, *mGen}};
        mTransformInfo->setContext(*CU, std::move(TfmCtx));
      }
    }
    mQueryManager->run(M, mTransformInfo);
    if (llvm::TimePassesIsEnabled)
      LLVMIRAnalysis.stopTimer();
  }

  void HandleTagDeclDefinition(TagDecl *D) override {
    PrettyStackTraceDecl CrashInfo(D, SourceLocation(),
      mASTContext->getSourceManager(), "LLVM IR generation of declaration");
    mGen->HandleTagDeclDefinition(D);
  }

  void HandleTagDeclRequiredDefinition(const TagDecl *D) override {
    mGen->HandleTagDeclRequiredDefinition(D);
  }

  void CompleteTentativeDefinition(VarDecl *D) override {
    mGen->CompleteTentativeDefinition(D);
  }

  void AssignInheritanceModel(CXXRecordDecl *RD) override {
    mGen->AssignInheritanceModel(RD);
  }

  void HandleVTable(CXXRecordDecl *RD) override {
    mGen->HandleVTable(RD);
  }

private:
  CompilerInstance *mCI;
  Timer mLLVMIRGeneration;
  ASTContext *mASTContext;
  std::unique_ptr<llvm::LLVMContext> mLLVMContext;
  std::unique_ptr<CodeGenerator> mGen;
  TransformationInfo *mTransformInfo;
  QueryManager *mQueryManager;
  std::unique_ptr<llvm::Module> mModule;
};
}

bool MainAction::BeginSourceFileAction(CompilerInstance &CI) {
  TimePassesIsEnabled = CI.getFrontendOpts().ShowTimers;
  return mQueryManager->beginSourceFile(CI, getCurrentFile());
}

void MainAction::EndSourceFileAction() {
  mQueryManager->endSourceFile();
}

namespace tsar::detail {
JSON_OBJECT_BEGIN(SourceResponse)
JSON_OBJECT_ROOT_PAIR(SourceResponse, Context,
                      tsar::TransformationContextBase *)
SourceResponse() : JSON_INIT_ROOT {}
JSON_OBJECT_END(SourceResponse)
} // namespace tsar::detail

using namespace tsar::detail;

namespace {
class ASTSocket final : public SMStringSocketBase<ASTSocket> {
public:
  void processResponse(const std::string &Response) const {
    llvm::StringRef Json{Response.data() + 1, Response.size() - 2};
    json::Parser<SourceResponse> Parser(Json.str());
    SourceResponse R;
    if (!Parser.parse(R))
      mTfmCtx = nullptr;
    else
      mTfmCtx = R[SourceResponse::Context];
  }

  auto getContext() {
    for (auto &Callback : mReceiveCallbacks)
      Callback({Data, Delimiter});
    // Note, that callback run send() in client, so mAnalysisPass is already
    // set here.
    assert(mResponseKind == Data && "Unknown response: wait for data!");
    return IntrusiveRefCntPtr<TransformationContextBase>(mTfmCtx);
  }

private:
  mutable TransformationContextBase *mTfmCtx{nullptr};
};

class SourceQueryManager : public QueryManager {
public:
  explicit SourceQueryManager(bcl::IntrusiveConnection *C) : mConnection(C) {
    assert(C && "Connection must not be null!");
  }
  void run(llvm::Module *M, TransformationInfo *TfmInfo) override {
    bool WaitForRequest{true};
    while (WaitForRequest &&
           mConnection->answer([&WaitForRequest, M,
                                TfmInfo](std::string &Request) -> std::string {
             if (Request == ASTSocket::Release) {
               WaitForRequest = false;
               return {ASTSocket::Notify};
             } else if (Request == ASTSocket::Data) {
               auto CUs{M->getNamedMetadata("llvm.dbg.cu")};
               assert(CUs && "DICompileUnit metadata must exist!");
               auto I{find_if(CUs->operands(),
                              [](auto *Op) { return isa<DICompileUnit>(Op); })};
               assert(I != CUs->operands().end() &&
                      "DICompileUnit metadata must exist!");
               SourceResponse Response;
               Response[SourceResponse::Context] =
                   TfmInfo->getContext(*cast<DICompileUnit>(*I));
               return ASTSocket::Data +
                      json::Parser<SourceResponse>::unparseAsObject(Response);
             } else {
               return {ASTSocket::Invalid};
             }
           }))
      ;
  }

private:
  bcl::IntrusiveConnection *mConnection;
};

template <TransformationContextBase::Kind FrontendKind> struct ActionHelper {
  IntrusiveRefCntPtr<TransformationContextBase> CreateTransformationContext(
      [[maybe_unused]] const llvm::Module &M,
      [[maybe_unused]] const DICompileUnit &CU,
      [[maybe_unused]] StringRef IRSource, [[maybe_unused]] StringRef Path,
      [[maybe_unused]] ArrayRef<std::string> CommandLine) {
    return nullptr;
  }
};

template<>
struct ActionHelper<TransformationContextBase::TC_Clang> {
  ~ActionHelper() {
    for (auto &S : mSockets)
      S->release();
  }

  IntrusiveRefCntPtr<TransformationContextBase>
  CreateTransformationContext([[maybe_unused]] const llvm::Module &M,
                              [[maybe_unused]] const DICompileUnit &CU,
                              [[maybe_unused]] StringRef IRSource,
                              StringRef Path,
                              ArrayRef<std::string> CommandLine) {
    mSockets.push_back(std::make_unique<ASTSocket>());
    bcl::IntrusiveConnection::connect(
        mSockets.back().get(), ASTSocket::Delimiter,
        [this, &CommandLine, &Path](bcl::IntrusiveConnection C) {
          auto Compilations{std::unique_ptr<CompilationDatabase>(
              new FixedCompilationDatabase(".", CommandLine))};
          ClangTool CTool(*Compilations, makeArrayRef(Path.str()));
          SourceQueryManager SQM{&C};
          QueryManager *QM{&SQM};
          CTool.run(newActionFactory<MainAction, GenPCHPragmaAction>(
                        std::forward_as_tuple(CommandLine, QM))
                        .get());
        });
    return mSockets.back()->getContext();
  }

private:
  std::vector<std::unique_ptr<ASTSocket>> mSockets;
};

#ifdef FLANG_FOUND
template<>
struct ActionHelper<TransformationContextBase::TC_Flang> {
  Fortran::common::IntrinsicTypeDefaultKinds DefaultKinds;

  IntrusiveRefCntPtr<TransformationContextBase>
  CreateTransformationContext(const llvm::Module &M, const DICompileUnit &CU,
      StringRef IRSource, StringRef Path, ArrayRef<std::string> CommandLine) {
    Fortran::parser::Options Options;
    Options.predefinitions.emplace_back("__F18", "1");
    Options.predefinitions.emplace_back("__F18_MAJOR__", "1");
    Options.predefinitions.emplace_back("__F18_MINOR__", "1");
    Options.predefinitions.emplace_back("__F18_PATCHLEVEL__", "1");
    Options.features.Enable(
      Fortran::common::LanguageFeature::BackslashEscapes, true);
    auto Extension = sys::path::extension(Path);
    Options.isFixedForm =
      (Extension == ".f" || Extension == ".F" || Extension == ".ff");
    Options.searchDirectories.emplace_back("."s);
    IntrusiveRefCntPtr<TransformationContextBase> TfmCtx{
        new FlangTransformationContext{Options, DefaultKinds}};
    auto &Parsing{cast<FlangTransformationContext>(TfmCtx)->getParsing()};
    Parsing.Prescan(std::string{Path},
                    cast<FlangTransformationContext>(TfmCtx)->getOptions());
    if (!Parsing.messages().empty() &&
      Parsing.messages().AnyFatalError()) {
      Parsing.messages().Emit(errs(), Parsing.cooked());
      errs() << IRSource << " could not scan " << Path << '\n';
      return nullptr;
    }
    Parsing.Parse(outs());
    Parsing.ClearLog();
    Parsing.messages().Emit(errs(), Parsing.cooked());
    if (!Parsing.consumedWholeFile()) {
      Parsing.EmitMessage(errs(), Parsing.finalRestingPlace(),
        "parser FAIL (final position)");
      return nullptr;
    }
    if (!Parsing.messages().empty() &&
      Parsing.messages().AnyFatalError() || !Parsing.parseTree()) {
      errs() << IRSource << " could not parse " << Path << '\n';
      return nullptr;
    }
    auto &ParseTree{ *Parsing.parseTree() };
    Fortran::semantics::Semantics Semantics{
        cast<FlangTransformationContext>(TfmCtx)->getContext(), ParseTree,
        Parsing.cooked(), false};
    Semantics.Perform();
    Semantics.EmitMessages(llvm::errs());
    if (Semantics.AnyFatalError()) {
      errs() << IRSource << " semantic errors in " << Path << '\n';
      return nullptr;
    }
    cast<FlangTransformationContext>(TfmCtx)->initialize(M, CU);
    return TfmCtx;
  }
};
#endif
}

namespace json {
template <>
struct CellTraits<tsar::detail::json_::SourceResponseImpl::Context> {
  using CellKey = tsar::detail::json_::SourceResponseImpl::Context;
  using ValueType = CellKey::ValueType;
  inline static bool parse(ValueType &Dest, Lexer &Lex)
      noexcept(
        noexcept(Traits<ValueType>::parse(Dest, Lex))) {
    uintptr_t RawDest;
    auto Res = Traits<uintptr_t>::parse(RawDest, Lex);
    if (Res)
      Dest = reinterpret_cast<tsar::TransformationContextBase *>(RawDest);
    return Res;
  }
  inline static void unparse(String &JSON, const ValueType &Obj)
      noexcept(
        noexcept(Traits<ValueType>::unparse(JSON, Obj))) {
    Traits<uintptr_t>::unparse(JSON, reinterpret_cast<uintptr_t>(Obj));
  }
  inline static typename std::result_of<
    decltype(&CellKey::name)()>::type name()
      noexcept(noexcept(CellKey::name())) {
    return CellKey::name();
  }
};
}

JSON_DEFAULT_TRAITS(::, SourceResponse)

void MainAction::ExecuteAction() {
  // If this is an IR file, we have to treat it specially.
  if (getCurrentFileKind().getLanguage() != Language::LLVM_IR) {
    ASTFrontendAction::ExecuteAction();
    return;
  }
  if (!hasIRSupport()) {
    errs() << getCurrentFile() << " error: requested action is not available\n";
    return;
  }
  bool Invalid;
  CompilerInstance &CI = getCompilerInstance();
  SourceManager &SM = CI.getSourceManager();
  FileID FID = SM.getMainFileID();
  auto *MainFile = SM.getBuffer(FID, &Invalid);
  if (Invalid)
    return;
  llvm::SMDiagnostic Err;
  LLVMContext Ctx;
  std::unique_ptr<llvm::Module> M =
    parseIR(MainFile->getMemBufferRef(), Err, Ctx);
  if (!M) {
    // Translate from the diagnostic info to the SourceManager location if
    // available.
    SourceLocation Loc;
    if (Err.getLineNo() > 0) {
      assert(Err.getColumnNo() >= 0);
      Loc = SM.translateFileLineCol(SM.getFileEntryForID(FID),
        Err.getLineNo(), Err.getColumnNo() + 1);
    }
    // Strip off a leading diagnostic code if there is one.
    StringRef Msg = Err.getMessage();
    if (Msg.startswith("error: "))
      Msg = Msg.substr(7);
    unsigned DiagID =
      CI.getDiagnostics().getCustomDiagID(DiagnosticsEngine::Error, "%0");
    CI.getDiagnostics().Report(Loc, DiagID) << Msg;
    return;
  }
  const auto &TargetOpts = CI.getTargetOpts();
  if (M->getTargetTriple() != TargetOpts.Triple) {
    CI.getDiagnostics().Report(SourceLocation(),
      diag::warn_fe_override_module)
      << TargetOpts.Triple;
    M->setTargetTriple(TargetOpts.Triple);
  }
  Timer LLVMIRAnalysis(
#if LLVM_VERSION_MAJOR > 3
    "LLVMIRAnalysis",
#endif
    "LLVM IR Analysis Time");
  if (llvm::TimePassesIsEnabled)
    LLVMIRAnalysis.startTimer();
  ActionHelper<TransformationContextBase::TC_Clang> ClangHelper;
  ActionHelper<TransformationContextBase::TC_Flang> FlangHelper;
  if (mTfmInfo) {
    auto CUs = M->getNamedMetadata("llvm.dbg.cu");
    for (auto *Op : CUs->operands())
      if (auto *CU = dyn_cast<DICompileUnit>(Op)) {
        SmallString<128> Path{CU->getFilename()};
        sys::fs::make_absolute(CU->getDirectory(), Path);
        if (isFortran(CU->getSourceLanguage())) {
          if (auto TfmCtx = FlangHelper.CreateTransformationContext(
                  *M, *CU, getCurrentFile(), Path, mTfmInfo->getCommandLine()))
            mTfmInfo->setContext(*CU, std::move(TfmCtx));
        } else if (isC(CU->getSourceLanguage()) ||
                   isCXX(CU->getSourceLanguage()))
          if (auto TfmCtx = ClangHelper.CreateTransformationContext(
                  *M, *CU, getCurrentFile(), Path, mTfmInfo->getCommandLine()))
            mTfmInfo->setContext(*CU, std::move(TfmCtx));
      }
  }
  mQueryManager->run(M.get(), mTfmInfo.get());
  if (llvm::TimePassesIsEnabled)
    LLVMIRAnalysis.stopTimer();
}

std::unique_ptr<ASTConsumer>
MainAction::CreateASTConsumer(CompilerInstance &CI, StringRef InFile) {
  return std::make_unique<AnalysisConsumer>(CI, InFile, mTfmInfo.get(),
                                            *mQueryManager);
}

MainAction::MainAction(ArrayRef<std::string> CL, QueryManager *QM,
                       bool LoadSources)
    : mQueryManager(QM),
      mTfmInfo(LoadSources ? new TransformationInfo(CL) : nullptr) {
  assert(QM && "Query manager must not be null!");
}
