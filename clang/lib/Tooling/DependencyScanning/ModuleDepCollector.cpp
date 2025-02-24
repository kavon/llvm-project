//===- ModuleDepCollector.cpp - Callbacks to collect deps -------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "clang/Tooling/DependencyScanning/ModuleDepCollector.h"

#include "clang/Frontend/CompilerInstance.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Tooling/DependencyScanning/DependencyScanningWorker.h"

using namespace clang;
using namespace tooling;
using namespace dependencies;

std::vector<std::string> ModuleDeps::getFullCommandLine(
    std::function<StringRef(ModuleID)> LookupPCMPath,
    std::function<const ModuleDeps &(ModuleID)> LookupModuleDeps) const {
  std::vector<std::string> Ret = NonPathCommandLine;

  // TODO: Build full command line. That also means capturing the original
  //       command line into NonPathCommandLine.

  dependencies::detail::appendCommonModuleArguments(
      ClangModuleDeps, LookupPCMPath, LookupModuleDeps, Ret);

  return Ret;
}

void dependencies::detail::appendCommonModuleArguments(
    llvm::ArrayRef<ModuleID> Modules,
    std::function<StringRef(ModuleID)> LookupPCMPath,
    std::function<const ModuleDeps &(ModuleID)> LookupModuleDeps,
    std::vector<std::string> &Result) {
  llvm::StringSet<> AlreadyAdded;

  std::function<void(llvm::ArrayRef<ModuleID>)> AddArgs =
      [&](llvm::ArrayRef<ModuleID> Modules) {
        for (const ModuleID &MID : Modules) {
          if (!AlreadyAdded.insert(MID.ModuleName + MID.ContextHash).second)
            continue;
          const ModuleDeps &M = LookupModuleDeps(MID);
          // Depth first traversal.
          AddArgs(M.ClangModuleDeps);
          Result.push_back(("-fmodule-file=" + LookupPCMPath(MID)).str());
          if (!M.ClangModuleMapFile.empty()) {
            Result.push_back("-fmodule-map-file=" + M.ClangModuleMapFile);
          }
        }
      };

  AddArgs(Modules);
}

void ModuleDepCollectorPP::FileChanged(SourceLocation Loc,
                                       FileChangeReason Reason,
                                       SrcMgr::CharacteristicKind FileType,
                                       FileID PrevFID) {
  if (Reason != PPCallbacks::EnterFile)
    return;
  
  // This has to be delayed as the context hash can change at the start of
  // `CompilerInstance::ExecuteAction`.
  if (MDC.ContextHash.empty()) {
    MDC.ContextHash =
        Instance.getInvocation().getModuleHash(Instance.getDiagnostics());
    MDC.Consumer.handleContextHash(MDC.ContextHash);
  }

  SourceManager &SM = Instance.getSourceManager();

  // Dependency generation really does want to go all the way to the
  // file entry for a source location to find out what is depended on.
  // We do not want #line markers to affect dependency generation!
  if (Optional<StringRef> Filename =
          SM.getNonBuiltinFilenameForID(SM.getFileID(SM.getExpansionLoc(Loc))))
    MDC.FileDeps.push_back(
        std::string(llvm::sys::path::remove_leading_dotslash(*Filename)));
}

void ModuleDepCollectorPP::InclusionDirective(
    SourceLocation HashLoc, const Token &IncludeTok, StringRef FileName,
    bool IsAngled, CharSourceRange FilenameRange, const FileEntry *File,
    StringRef SearchPath, StringRef RelativePath, const Module *Imported,
    SrcMgr::CharacteristicKind FileType) {
  if (!File && !Imported) {
    // This is a non-modular include that HeaderSearch failed to find. Add it
    // here as `FileChanged` will never see it.
    MDC.FileDeps.push_back(std::string(FileName));
  }
  handleImport(Imported);
}

void ModuleDepCollectorPP::moduleImport(SourceLocation ImportLoc,
                                        ModuleIdPath Path,
                                        const Module *Imported) {
  handleImport(Imported);
}

void ModuleDepCollectorPP::handleImport(const Module *Imported) {
  if (!Imported)
    return;

  const Module *TopLevelModule = Imported->getTopLevelModule();
  MDC.ModularDeps[MDC.ContextHash + TopLevelModule->getFullModuleName()]
      .ImportedByMainFile = true;
  DirectModularDeps.insert(TopLevelModule);
}

void ModuleDepCollectorPP::EndOfMainFile() {
  FileID MainFileID = Instance.getSourceManager().getMainFileID();
  MDC.MainFile = std::string(
      Instance.getSourceManager().getFileEntryForID(MainFileID)->getName());

  for (const Module *M : DirectModularDeps)
    handleTopLevelModule(M);

  for (auto &&I : MDC.ModularDeps)
    MDC.Consumer.handleModuleDependency(I.second);

  for (auto &&I : MDC.FileDeps)
    MDC.Consumer.handleFileDependency(*MDC.Opts, I);
}

void ModuleDepCollectorPP::handleTopLevelModule(const Module *M) {
  assert(M == M->getTopLevelModule() && "Expected top level module!");

  auto ModI = MDC.ModularDeps.insert(
      std::make_pair(MDC.ContextHash + M->getFullModuleName(), ModuleDeps{}));

  if (!ModI.first->second.ID.ModuleName.empty())
    return;

  ModuleDeps &MD = ModI.first->second;

  const FileEntry *ModuleMap = Instance.getPreprocessor()
                                   .getHeaderSearchInfo()
                                   .getModuleMap()
                                   .getModuleMapFileForUniquing(M);

  MD.ClangModuleMapFile = std::string(ModuleMap ? ModuleMap->getName() : "");
  MD.ID.ModuleName = M->getFullModuleName();
  MD.ImplicitModulePCMPath = std::string(M->getASTFile()->getName());
  MD.ID.ContextHash = MDC.ContextHash;
  serialization::ModuleFile *MF =
      MDC.Instance.getASTReader()->getModuleManager().lookup(M->getASTFile());
  MDC.Instance.getASTReader()->visitInputFiles(
      *MF, true, true, [&](const serialization::InputFile &IF, bool isSystem) {
        // __inferred_module.map is the result of the way in which an implicit
        // module build handles inferred modules. It adds an overlay VFS with
        // this file in the proper directory and relies on the rest of Clang to
        // handle it like normal. With explicitly built modules we don't need
        // to play VFS tricks, so replace it with the correct module map.
        if (IF.getFile()->getName().endswith("__inferred_module.map")) {
          MD.FileDeps.insert(ModuleMap->getName());
          return;
        }
        MD.FileDeps.insert(IF.getFile()->getName());
      });
  MD.NonPathCommandLine = {
    "-remove-preceeding-explicit-module-build-incompatible-options",
    "-fno-implicit-modules", "-emit-module", "-fmodule-name=" + MD.ID.ModuleName,
  };
  
  if (M->IsSystem)
    MD.NonPathCommandLine.push_back("-fsystem-module");

  llvm::DenseSet<const Module *> AddedModules;
  addAllSubmoduleDeps(M, MD, AddedModules);
}

void ModuleDepCollectorPP::addAllSubmoduleDeps(
    const Module *M, ModuleDeps &MD,
    llvm::DenseSet<const Module *> &AddedModules) {
  addModuleDep(M, MD, AddedModules);

  for (const Module *SubM : M->submodules())
    addAllSubmoduleDeps(SubM, MD, AddedModules);
}

void ModuleDepCollectorPP::addModuleDep(
    const Module *M, ModuleDeps &MD,
    llvm::DenseSet<const Module *> &AddedModules) {
  for (const Module *Import : M->Imports) {
    if (Import->getTopLevelModule() != M->getTopLevelModule()) {
      if (AddedModules.insert(Import->getTopLevelModule()).second)
        MD.ClangModuleDeps.push_back(
            {std::string(Import->getTopLevelModuleName()),
             Instance.getInvocation().getModuleHash(
                 Instance.getDiagnostics())});
      handleTopLevelModule(Import->getTopLevelModule());
    }
  }
}

ModuleDepCollector::ModuleDepCollector(
    std::unique_ptr<DependencyOutputOptions> Opts, CompilerInstance &I,
    DependencyConsumer &C)
    : Instance(I), Consumer(C), Opts(std::move(Opts)) {}

void ModuleDepCollector::attachToPreprocessor(Preprocessor &PP) {
  PP.addPPCallbacks(std::make_unique<ModuleDepCollectorPP>(Instance, *this));
}

void ModuleDepCollector::attachToASTReader(ASTReader &R) {}
