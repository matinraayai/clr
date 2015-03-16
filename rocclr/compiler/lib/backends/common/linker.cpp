//
// Copyright (c) 2008 Advanced Micro Devices, Inc. All rights reserved.
//
// TODO: The entire linker implementation should be a pass in LLVM and
// the code in the compiler library should only call this pass.

#include "top.hpp"
#include "library.hpp"
#include "linker.hpp"
#include "os/os.hpp"
#include "thread/monitor.hpp"
#include "utils/libUtils.h"
#include "utils/options.hpp"
#include "utils/target_mappings.h"

#include "acl.h"

#include "llvm/Instructions.h"
#include "llvm/Linker.h"
#include "llvm/GlobalValue.h"
#include "llvm/GlobalVariable.h"

#include "llvm/AMDFixupKernelModule.h"
#include "llvm/AMDResolveLinker.h"
#include "llvm/AMDPrelinkOpt.h"
#include "llvm/AMDUtils.h"
#include "llvm/ADT/Triple.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Analysis/AMDLocalArrayUsage.h"
#include "llvm/Analysis/LoopPass.h"
#include "llvm/Analysis/Passes.h"
#include "llvm/Analysis/Verifier.h"
#include "llvm/Bitcode/ReaderWriter.h"

#include "llvm/CodeGen/LinkAllAsmWriterComponents.h"
#include "llvm/CodeGen/LinkAllCodegenComponents.h"
#if 1 || LLVM_TRUNK_INTEGRATION_CL >= 2270
#else
#include "llvm/CodeGen/ObjectCodeEmitter.h"
#endif
#include "llvm/Config/config.h"

#include "llvm/MC/SubtargetFeature.h"

#include "llvm/Support/CallSite.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/PluginLoader.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/system_error.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/DataLayout.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"

#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/ValueSymbolTable.h"

#if defined(LEGACY_COMPLIB)
#include "llvm/AMDILFuncSupport.h"
#endif

#ifdef _DEBUG
#include "llvm/Assembly/Writer.h"
#endif

// need to undef DEBUG before using DEBUG macro in llvm/Support/Debug.h
#ifdef DEBUG
#undef DEBUG
#endif
#include "llvm/Support/Debug.h"

#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <iostream>
#include <fstream>
#include <sstream>
#include <list>
#include <map>
#include <set>

#ifdef _WIN32
#include <windows.h>
#endif // _WIN32

#ifdef DEBUG_TYPE
#undef DEBUG_TYPE
#endif
#define DEBUG_TYPE "ocl_linker"

static const char* OptionMaskFName = "__option_mask";

namespace AMDSpir {
  extern void replaceTrivialFunc(llvm::Module& M);
}
namespace amd {

namespace {

using namespace llvm;

// LoadFile - Read the specified bitcode file in and return it.  This routine
// searches the link path for the specified file to try to find it...
//
inline llvm::Module*
  LoadFile(const std::string &Filename, LLVMContext& Context)
  {
    bool Exists;
    if (sys::fs::exists(Filename, Exists) || !Exists) {
      //    dbgs() << "Bitcode file: '" << Filename.c_str() << "' does not exist.\n";
      return 0;
    }

    llvm::Module* M;
    std::string ErrorMessage;
    OwningPtr<MemoryBuffer> Buffer;
    if (error_code ec = MemoryBuffer::getFileOrSTDIN(Filename, Buffer)) {
      // Error
      M = NULL;
    }
    else {
      M = ParseBitcodeFile(Buffer.get(), Context, &ErrorMessage);
    }

    return M;
  }

inline llvm::Module*
  LoadLibrary(const std::string& libFile, LLVMContext& Context, MemoryBuffer** Buffer) {
    bool Exists;
    if (sys::fs::exists(libFile, Exists) || !Exists) {
      //    dbgs() << "Bitcode file: '" << Filename.c_str() << "' does not exist.\n";
      return 0;
    }

    llvm::Module* M = NULL;
    std::string ErrorMessage;

    static Monitor mapLock;
    static std::map<std::string, void*> FileMap;
    MemoryBuffer* statBuffer;
    {
      ScopedLock sl(mapLock);
      statBuffer = (MemoryBuffer*) FileMap[libFile];
      if (statBuffer == NULL) {
        OwningPtr<MemoryBuffer> PtrBuffer;
        if (error_code ec = MemoryBuffer::getFileOrSTDIN(libFile, PtrBuffer)) {
          // Error
          return NULL;
        }
        else
          statBuffer = PtrBuffer.take();
        M = ParseBitcodeFile(statBuffer, Context, &ErrorMessage);
        FileMap[libFile] = statBuffer;
      }
    }
    *Buffer = MemoryBuffer::getMemBufferCopy(StringRef(statBuffer->getBufferStart(), statBuffer->getBufferSize()), "");
    if ( *Buffer ) {
      M = getLazyBitcodeModule(*Buffer, Context, &ErrorMessage);
      if (!M) {
        delete *Buffer;
        *Buffer = 0;
      }
    }
    return M;
  }

// Load bitcode libary from an array of const char. This assumes that
// the array has a valid ending zero !
llvm::Module*
  LoadLibrary(const char* libBC, size_t libBCSize,
      LLVMContext& Context, MemoryBuffer** Buffer)
  {
    llvm::Module* M = 0;
    std::string ErrorMessage;

    *Buffer = MemoryBuffer::getMemBuffer(StringRef(libBC, libBCSize), "");
    if ( *Buffer ) {
      M = getLazyBitcodeModule(*Buffer, Context, &ErrorMessage);
      if (!M) {
        delete *Buffer;
        *Buffer = 0;
      }
    }
    return M;
  }


static std::set<std::string> *getAmdRtFunctions()
{
  std::set<std::string> *result = new std::set<std::string>();
  for (size_t i = 0; i < sizeof(amdRTFuns)/sizeof(amdRTFuns[0]); ++i)
    result->insert(amdRTFuns[i]);
  return result;
}

}


} // namespace amd

// create a llvm function which simply returns the given mask
static void createConstIntFunc(const char* fname,
                               int mask,
                               llvm::Module* module)
{
  llvm::LLVMContext& context = module->getContext();

  llvm::Type* int32Ty = llvm::Type::getInt32Ty(context);
  llvm::FunctionType* fType = llvm::FunctionType::get(int32Ty, false);
  llvm::Function* function
      = llvm::cast<llvm::Function>(module->getOrInsertFunction(fname, fType));
  function->setDoesNotThrow();
  function->setDoesNotAccessMemory();
  function->addFnAttr(llvm::Attributes::AlwaysInline);
  llvm::BasicBlock* bb = llvm::BasicBlock::Create(context, "entry", function);
  llvm::Value* retVal = llvm::ConstantInt::get(int32Ty, mask);
  llvm::ReturnInst* retInst = llvm::ReturnInst::Create(context, retVal);
  bb->getInstList().push_back(retInst);
  assert(!verifyFunction(*function) && "verifyFunction failed");
}

// create a llvm function that returns a mask of several compile options
// which are used by the built-in library
void amdcl::OCLLinker::createOptionMaskFunction(llvm::Module* module)
{
  unsigned mask = 0;
  if (Options()->oVariables->NoSignedZeros) {
    mask |= MASK_NO_SIGNED_ZEROES;
  }
  if (Options()->oVariables->UnsafeMathOpt) {
    mask |= MASK_UNSAFE_MATH_OPTIMIZATIONS;
    mask |= MASK_NO_SIGNED_ZEROES;
  }
  if (Options()->oVariables->FiniteMathOnly) {
    mask |= MASK_FINITE_MATH_ONLY;
  }
  if (Options()->oVariables->FastRelaxedMath) {
    mask |= MASK_FAST_RELAXED_MATH;
    mask |= MASK_FINITE_MATH_ONLY;
    mask |= MASK_UNSAFE_MATH_OPTIMIZATIONS;
    mask |= MASK_NO_SIGNED_ZEROES;
  }

  if (Options()->oVariables->UniformWorkGroupSize) {
    mask |= MASK_UNIFORM_WORK_GROUP_SIZE;
  }

  createConstIntFunc(OptionMaskFName, mask, module);
}

// Create functions that returns true or false for some features which
// are used by the built-in library
void amdcl::OCLLinker::createASICIDFunctions(llvm::Module* module)
{
  if (!isAMDILTarget(Elf()->target))
    return;

  uint64_t features = aclGetChipOptions(Elf()->target);

  llvm::StringRef chip(aclGetChip(Elf()->target));
  llvm::StringRef family(aclGetFamily(Elf()->target));

  createConstIntFunc("__amdil_have_hw_fma32",
                        chip == "Cypress"
                     || chip == "Cayman"
                     || family == "SI"
                     || family == "CI"
                     || family == "KV"
                     || family == "TN"
                     || family == "VI"
                     || family == "CZ",
                     module);
  createConstIntFunc("__amdil_have_fast_fma32",
                        chip == "Cypress"
                     || chip == "Cayman"
                     || chip == "Tahiti"
                     || chip == "Hawaii"
                     || chip == "Carrizo",
                     module);
  createConstIntFunc("__amdil_have_bitalign", !!(features & F_EG_BASE), module);
  createConstIntFunc("__amdil_is_cypress", chip == "Cypress", module);
  createConstIntFunc("__amdil_is_ni",
                        chip == "Cayman"
                     || family == "TN",
                     module);
  createConstIntFunc("__amdil_is_gcn",
                        family == "SI"
                     || family == "CI"
                     || family == "VI"
                     || family == "KV"
                     || family == "CZ",
                     module);
}

bool
amdcl::OCLLinker::linkWithModule(
    llvm::Module* Dst, llvm::Module* Src,
    std::map<const llvm::Value*, bool> *ModuleRefMap)
{
#ifndef NDEBUG
  if (Options()->oVariables->EnableDebugLinker) {
      llvm::DebugFlag = true;
      llvm::setCurrentDebugType(DEBUG_TYPE);
  }
#endif
  std::string ErrorMessage;
  if (llvm::linkWithModule(Dst, Src, ModuleRefMap, &ErrorMessage)) {
    DEBUG(llvm::dbgs() << "Error: " << ErrorMessage << "\n");
    BuildLog() += "\nInternal Error: linking libraries failed!\n";
    LogError("linkWithModule(): linking bc libraries failed!");
    return true;
  }
  return false;
}



static void delete_llvm_module(llvm::Module *a)
{
  delete a;
}
  bool
amdcl::OCLLinker::linkLLVMModules(std::vector<llvm::Module*> &libs)
{
  // Load input modules first
  bool Failed = false;
  for (size_t i = 0; i < libs.size(); ++i) {
    std::string ErrorMsg;
    if (!libs[i]) {
      char ErrStr[128];
      sprintf(ErrStr,
          "Error: cannot load input %d bc for linking: %s\n",
          (int)i, ErrorMsg.c_str());
      BuildLog() += ErrStr;
      Failed = true;
      break;
    }

    if (Options()->isDumpFlagSet(amd::option::DUMP_BC_ORIGINAL)) {
      std::string MyErrorInfo;
      char buf[128];
      sprintf(buf, "_original%d.bc", (int)i);
      std::string fileName = Options()->getDumpFileName(buf);
      llvm::raw_fd_ostream outs(fileName.c_str(), MyErrorInfo,
          llvm::raw_fd_ostream::F_Binary);
      if (MyErrorInfo.empty())
        llvm::WriteBitcodeToFile(libs[i], outs);
      else
        printf(MyErrorInfo.c_str());
    }
  }

  if (!Failed) {
    // Link input modules together
    for (size_t i = 0; i < libs.size(); ++i) {
      DEBUG(llvm::dbgs() << "LinkWithModule " << i << ":\n");
      if (amdcl::OCLLinker::linkWithModule(LLVMBinary(), libs[i], NULL)) {
        Failed = true;
      }
    }
  }

  if (Failed) {
    delete LLVMBinary();
  }
  std::for_each(libs.begin(), libs.end(), std::ptr_fun(delete_llvm_module));
  libs.clear();
  return Failed;

}

void amdcl::OCLLinker::fixupOldTriple(llvm::Module *module)
{
  llvm::Triple triple(module->getTargetTriple());

  // Bug 9357: "amdopencl" used to be a hacky "OS" that was Linux or Windows
  // depending on the host. It only really matters for x86. If we are trying to
  // use an old binary module still using the old triple, replace it with a new
  // one.
  if (triple.getOSName() == "amdopencl") {
    if (triple.getArch() == llvm::Triple::amdil ||
        triple.getArch() == llvm::Triple::amdil64) {
      triple.setOS(llvm::Triple::UnknownOS);
    } else {
      llvm::Triple hostTriple(llvm::sys::getDefaultTargetTriple());
      triple.setOS(hostTriple.getOS());
    }

    triple.setEnvironment(llvm::Triple::AMDOpenCL);
    module->setTargetTriple(triple.str());
  }
}

// On 64 bit device, aclBinary target is set to 64 bit by default. When 32 bit
// LLVM or SPIR binary is loaded, aclBinary target needs to be modified to
// match LLVM or SPIR bitness.
// Returns false on error.
static bool
checkAndFixAclBinaryTarget(llvm::Module* module, aclBinary* elf,
    std::string& buildLog) {
  if (module->getTargetTriple().empty()) {
    LogWarning("Module has no target triple");
    return true;
  }

  llvm::Triple triple(module->getTargetTriple());
  const char* newArch = NULL;
  if (elf->target.arch_id == aclAMDIL64 &&
     (triple.getArch() == llvm::Triple::amdil ||
     triple.getArch() == llvm::Triple::spir))
       newArch = "amdil";
  else if (elf->target.arch_id == aclX64 &&
      (triple.getArch() == llvm::Triple::x86 ||
      triple.getArch() == llvm::Triple::spir))
      newArch = "x86";
  else if (elf->target.arch_id == aclHSAIL64 &&
      (triple.getArch() == llvm::Triple::hsail ||
      triple.getArch() == llvm::Triple::spir))
      newArch = "hsail";
  if (newArch != NULL) {
    acl_error errorCode;
    elf->target = aclGetTargetInfo(newArch, aclGetChip(elf->target),
        &errorCode);
    if (errorCode != ACL_SUCCESS) {
      assert(0 && "Invalid arch id or chip id in elf target");
      buildLog += "Internal Error: failed to link modules correctlty.\n";
      return false;
    }
  }

  reinterpret_cast<amd::option::Options*>(elf->options)->libraryType_ =
      getLibraryType(&elf->target);

  // Check consistency between module triple and aclBinary target
  if (elf->target.arch_id == aclAMDIL64 &&
      (triple.getArch() == llvm::Triple::amdil64 ||
      triple.getArch() == llvm::Triple::spir64))
    return true;
  if (elf->target.arch_id == aclAMDIL &&
      (triple.getArch() == llvm::Triple::amdil ||
      triple.getArch() == llvm::Triple::spir))
    return true;
  if (elf->target.arch_id == aclHSAIL64 &&
      (triple.getArch() == llvm::Triple::hsail64 ||
      triple.getArch() == llvm::Triple::spir64))
    return true;
  if (elf->target.arch_id == aclHSAIL &&
      (triple.getArch() == llvm::Triple::hsail ||
      triple.getArch() == llvm::Triple::spir))
    return true;
  if (elf->target.arch_id == aclX64 &&
      (triple.getArch() == llvm::Triple::x86_64 ||
      triple.getArch() == llvm::Triple::spir64))
    return true;
  if (elf->target.arch_id == aclX86 &&
      (triple.getArch() == llvm::Triple::x86 ||
      triple.getArch() == llvm::Triple::spir))
    return true;
  DEBUG_WITH_TYPE("linkTriple", llvm::dbgs() <<
      "[checkAndFixAclBinaryTarget] " <<
      " aclBinary target: " << elf->target.arch_id <<
      " chipId: " << elf->target.chip_id <<
      " module triple: " << module->getTargetTriple() <<
      '\n');

  //ToDo: There is bug 9996 in compiler library about converting BIF30 to BIF21
  //which causes regressions in ocltst if the following check is enabled.
  //Fix the bugs then enable the following check
#if 0
  assert(0 && "Inconsistent LLVM target and elf target");
  buildLog += "Internal Error: failed to link modules correctlty.\n";
  return false;
#else
  LogWarning("Inconsistent LLVM target and elf target");
  return true;
#endif
}

int
amdcl::OCLLinker::link(llvm::Module* input, std::vector<llvm::Module*> &libs)
{
  bool IsGPUTarget = isGpuTarget(Elf()->target);
  uint64_t start_time = 0ULL, time_link = 0ULL, time_prelinkopt = 0ULL;
  if (Options()->oVariables->EnableBuildTiming) {
    start_time = amd::Os::timeNanos();
  }

  fixupOldTriple(input);

  if (!checkAndFixAclBinaryTarget(input, Elf(), BuildLog()))
    return 1;

  int ret = 0;
  if (Options()->oVariables->UseJIT) {
    delete hookup_.amdrtFunctions;
    hookup_.amdrtFunctions = amd::getAmdRtFunctions();
  } else {
    hookup_.amdrtFunctions = NULL;
  }
  if (Options()->isOptionSeen(amd::option::OID_LUThreshold) || !IsGPUTarget) {
    setUnrollScratchThreshold(Options()->oVariables->LUThreshold);
  } else {
    setUnrollScratchThreshold(500);
  }
  setGPU(IsGPUTarget);

  setPreLinkOpt(false);

  // We are doing whole program optimization
  setWholeProgram(true);

  llvmbinary_ = input;

  if ( !LLVMBinary() ) {
    BuildLog() += "Internal Error: cannot load bc application for linking\n";
    return 1;
  }

  if (linkLLVMModules(libs)) {
    BuildLog() += "Internal Error: failed to link modules correctlty.\n";
    return 1;
  }

  // Don't link in built-in libraries if we are only creating the library.
  if (Options()->oVariables->clCreateLibrary) {
    return 0;
  }

  if (Options()->isDumpFlagSet(amd::option::DUMP_BC_ORIGINAL)) {
    std::string MyErrorInfo;
    std::string fileName = Options()->getDumpFileName("_original.bc");
    llvm::raw_fd_ostream outs(fileName.c_str(), MyErrorInfo, llvm::raw_fd_ostream::F_Binary);
    if (MyErrorInfo.empty())
      WriteBitcodeToFile(LLVMBinary(), outs);
    else
      printf(MyErrorInfo.c_str());
  }
  std::vector<llvm::Module*> LibMs;

  // The AMDIL GPU libraries include 32 bit specific, 64 bit specific and common
  // libraries. The common libraries do not have target triple. A search is
  // performed to find the first library containing non-empty target triple
  // and use it for translating SPIR.
  amd::LibraryDescriptor  LibDescs[
    amd::LibraryDescriptor::MAX_NUM_LIBRARY_DESCS];
  int sz;
  std::string LibTargetTriple;
  std::string LibDataLayout;
  if (amd::getLibDescs(Options()->libraryType_, LibDescs, sz) != 0) {
    // FIXME: If we error here, we don't clean up, so we crash in debug build
    // on compilerfini().
    BuildLog() += "Internal Error: finding libraries failed!\n";
    return 1;
  }
  for (int i=0; i < sz; i++) {
    llvm::MemoryBuffer* Buffer = 0;
    llvm::Module* Library = amd::LoadLibrary(LibDescs[i].start, LibDescs[i].size, Context(), &Buffer);
    DEBUG(llvm::dbgs() << "Loaded library " << i << "\n");
    if ( !Library ) {
      BuildLog() += "Internal Error: cannot load library!\n";
      delete LLVMBinary();
      for (int j = 0; j < i; ++j) {
        delete LibMs[j];
      }
      LibMs.clear();
      return 1;
#ifndef NDEBUG
    } else {
      if ( llvm::verifyModule( *Library ) ) {
        BuildLog() += "Internal Error: library verification failed!\n";
        exit(1);
      }
#endif
    }
    DEBUG_WITH_TYPE("linkTriple", llvm::dbgs() << "Library[" << i << "] " <<
        Library->getTargetTriple() << ' ' << Library->getDataLayout() << '\n');
    // Find the first library whose target triple is not empty.
    if (LibTargetTriple.empty() && !Library->getTargetTriple().empty()) {
        LibTargetTriple = Library->getTargetTriple();
        LibDataLayout = Library->getDataLayout();
    }
    LibMs.push_back(Library);
  }

  // Check consistency of target and data layout
  assert (!LibTargetTriple.empty() && "At least one library should have triple");
#ifndef NDEBUG
  for (size_t i = 0, e = LibMs.size(); i < e; ++i) {
    if (LibMs[i]->getTargetTriple().empty())
      continue;
    assert (LibMs[i]->getTargetTriple() == LibTargetTriple &&
        "Library target triple should match");
    assert (LibMs[i]->getDataLayout() == LibDataLayout &&
        "Library data layout should match");
  }
#endif


  AMDSpir::replaceTrivialFunc(*LLVMBinary());

  if (!llvm::fixupKernelModule(LLVMBinary(), LibTargetTriple, LibDataLayout))
    return 1;

  // For HSAIL targets, when the option -cl-fp32-correctly-rounded-divide-sqrt
  // lower divide and sqrt functions to precise HSAIL builtin library functions.
  bool LowerToPreciseFunctions = (isHSAILTriple(llvm::Triple(LibTargetTriple)) &&
                                  Options()->oVariables->FP32RoundDivideSqrt);

  // Before doing anything else, quickly optimize Module
  if (Options()->oVariables->EnableBuildTiming) {
    time_prelinkopt = amd::Os::timeNanos();
  }
  std::string clp_errmsg;
  llvm::Module *OnFlyLib = AMDPrelinkOpt(LLVMBinary(), true /*Whole*/,
    !Options()->oVariables->OptSimplifyLibCall,
    Options()->oVariables->UnsafeMathOpt,
    Options()->oVariables->OptUseNative,
    Options()->oVariables->OptLevel,
    LowerToPreciseFunctions,
    IsGPUTarget, clp_errmsg);

  if (!clp_errmsg.empty()) {
    delete LLVMBinary();
    for (unsigned int i = 0; i < LibMs.size(); ++ i) {
      delete LibMs[i];
    }
    LibMs.clear();
    BuildLog() += clp_errmsg;
    BuildLog() += "Internal Error: on-fly library generation failed\n";
    return 1;
  }

  if (OnFlyLib) {
    // OnFlyLib must be the last!
    LibMs.push_back(OnFlyLib);
  }

  if (Options()->oVariables->EnableBuildTiming) {
    time_prelinkopt = amd::Os::timeNanos() - time_prelinkopt;
  }
  // Now, do linking by extracting from the builtins library only those
  // functions that are used in the kernel(s).
  if (Options()->oVariables->EnableBuildTiming) {
    time_link = amd::Os::timeNanos();
  }

  std::string ErrorMessage;

  // build the reference map
  llvm::ReferenceMapBuilder RefMapBuilder(LLVMBinary(), LibMs);

  RefMapBuilder.InitReferenceMap();

  if (IsGPUTarget && RefMapBuilder.isInExternFuncs("printf")) {
    DEBUG(llvm::dbgs() << "Adding printf funs:\n");
    // The following functions need forcing as printf-conversion happens
    // after this link stage
    static const char* forcedRefs[] = {
      "___initDumpBuf",
      "___dumpBytes_v1b8",
      "___dumpBytes_v1b16",
      "___dumpBytes_v1b32",
      "___dumpBytes_v1b64",
      "___dumpBytes_v1b128",
      "___dumpBytes_v1b256",
      "___dumpBytes_v1b512",
      "___dumpBytes_v1b1024",
      "___dumpBytes_v1bs",
      "___dumpStringID"
    };
    RefMapBuilder.AddForcedReferences(forcedRefs,
      sizeof(forcedRefs)/sizeof(forcedRefs[0]));
  }
  if (!IsGPUTarget && Options()->oVariables->UseJIT) {
    RefMapBuilder.AddForcedReferences(amd::amdRTFuns,
      sizeof(amd::amdRTFuns)/sizeof(amd::amdRTFuns[0]));
  }

  RefMapBuilder.AddReferences();

  // inject an llvm function that returns the mask of several compile
  // options, which are used by the built-in library
  const std::list<std::string>& ExternFuncs
    = RefMapBuilder.getExternFunctions();
  const std::list<std::string>::const_iterator it
    = std::find(ExternFuncs.begin(), ExternFuncs.end(), OptionMaskFName);
  if (it != ExternFuncs.end()) {
    createOptionMaskFunction(LLVMBinary());
  }

  createASICIDFunctions(LLVMBinary());

  // Link libraries to get every functions that are referenced.
  std::string ErrorMsg;
  if (resolveLink(LLVMBinary(), LibMs, RefMapBuilder.getModuleRefMaps(),
                  &ErrorMsg)) {
      BuildLog() += ErrorMsg;
      BuildLog() += "\nInternal Error: linking libraries failed!\n";
      return 1;
  }
  LibMs.clear();


  if (Options()->oVariables->EnableBuildTiming) {
    time_link = amd::Os::timeNanos() - time_link;
    std::stringstream tmp_ss;
    tmp_ss << "    LLVM time (link+opt): "
      << (amd::Os::timeNanos() - start_time)/1000ULL
          << " us\n"
          << "      prelinkopt: "  << time_prelinkopt/1000ULL << " us\n"
          << "      link: "  << time_link/1000ULL << " us\n"
            ;
    appendLogToCL(CL(), tmp_ss.str());
  }

#if defined(LEGACY_COMPLIB)
  // Disable outline macro for mem2reg=0 unless -fdebug-call
  // is on.
  if (!Options()->oVariables->OptMem2reg && !Options()->oVariables->DebugCall)
    Options()->oVariables->UseMacroForCall = false;

  if (isAMDILTarget(Elf()->target) &&
      getFamilyEnum(&Elf()->target) >= FAMILY_SI &&
      !Options()->oVariables->clInternalKernel &&
      (Options()->oVariables->OptMem2reg ||
      Options()->oVariables->DebugCall)) {
    auto OV = Options()->oVariables;
    AMDILFuncSupport::PostLinkProcForFuncSupport(
        OV->AddUserNoInline,
        OV->AddLibNoInline,
        OV->InlineCostThreshold,
        OV->InlineSizeThreshold,
        OV->InlineKernelSizeThreshold,
        OV->AllowMultiLevelCall && OV->UseMacroForCall,
        LLVMBinary(), LibMs);
  }
#endif

  if (Options()->isDumpFlagSet(amd::option::DUMP_BC_LINKED)) {
    std::string MyErrorInfo;
    std::string fileName = Options()->getDumpFileName("_linked.bc");
    llvm::raw_fd_ostream outs(fileName.c_str(), MyErrorInfo, llvm::raw_fd_ostream::F_Binary);
    // FIXME: Need to add this to the elf binary!
    if (MyErrorInfo.empty())
      WriteBitcodeToFile(LLVMBinary(), outs);
    else
      printf(MyErrorInfo.c_str());
  }

    // Check if kernels containing local arrays are called by other kernels.
    std::string localArrayUsageError;
    if (!llvm::AMDCheckLocalArrayUsage(*LLVMBinary(), &localArrayUsageError)) {
      BuildLog() += "Error: " + localArrayUsageError + '\n';
      return 1;
    }

  return 0;
}
