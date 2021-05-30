// RUN: %clangxx %cxxflags %include_flags %ld_flags %s -Xclang -load -Xclang %lib_pass -o %t
// RUN: %t > %t.out
// RUN: %FileCheck %s < %t.out

#include <easy/jit.h>
#include <easy/runtime/Context.h>
#include <easy/attributes.h>
#include <easy/param.h>
#include <easy/function_wrapper.h>

#include <easy/runtime/BitcodeTracker.h>
#include <easy/runtime/Function.h>
#include <easy/runtime/RuntimePasses.h>
#include <easy/runtime/LLVMHolderImpl.h>
#include <easy/runtime/Utils.h>
#include <easy/exceptions.h>

#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>
#include <llvm/Transforms/IPO.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Support/Host.h> 
#include <llvm/Target/TargetMachine.h> 
#include <llvm/Support/TargetRegistry.h> 
#include <llvm/Analysis/TargetTransformInfo.h> 
#include <llvm/Analysis/TargetLibraryInfo.h> 
#include <llvm/Support/FileSystem.h>

#include <functional>
#include <cstdio>

using namespace std::placeholders;

double add (double a, double b) {
  return a+b;
}

std::unique_ptr<llvm::Module> _Compile(void *Addr, easy::Context const& C) {

  auto &BT = BitcodeTracker::GetTracker();

  const char* Name;
  GlobalMapping* Globals;
  std::tie(Name, Globals) = BT.getNameAndGlobalMapping(Addr);

  std::unique_ptr<llvm::Module> M;
  std::unique_ptr<llvm::LLVMContext> Ctx;
  std::tie(M, Ctx) = BT.getModule(Addr);

  return M;
}

template<class T, class ... Args>
std::unique_ptr<llvm::Module> _jit_with_context(easy::Context const &C, T &&Fun) {

  auto* FunPtr = meta::get_as_pointer(Fun);

  std::unique_ptr<llvm::Module> M = _Compile(reinterpret_cast<void*>(FunPtr), C);

  return M;
}

template<class T, class ... Args>
auto EASY_JIT_COMPILER_INTERFACE _get_module(T &&Fun, Args&& ... args) {
  auto C = get_context_for<T, Args...>(std::forward<Args>(args)...);
  return _jit_with_context<T, Args...>(C, std::forward<T>(Fun));
}

int main() {
  easy::FunctionWrapper<double(double)> inc = easy::jit(add, _1, 1);

  std::unique_ptr<llvm::Module> M = _get_module(add, _1, _2);

  // CHECK: inc(4.00) is 5.00
  // CHECK: inc(5.00) is 6.00
  // CHECK: inc(6.00) is 7.00
  // CHECK: inc(7.00) is 8.00
  for(int v = 4; v != 8; ++v)
    printf("inc(%.2f) is %.2f\n", (double)v, inc(v));

  return 0;
}
