#include <easy/attributes.h>

#include <llvm/Pass.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/DebugInfo.h>

#define DEBUG_TYPE "easy-register-layout"
#include <llvm/Support/Debug.h>

#include <llvm/Support/raw_ostream.h>

#include "Utils.h"
#include <numeric>
#include <iostream>

using namespace llvm;

namespace easy {
  struct RegisterLayout : public ModulePass {
    static char ID;

    RegisterLayout()
      : ModulePass(ID) {};

    bool runOnModule(Module &M) override {
      
      SmallVector<Function*, 8> LayoutFunctions;
      collectLayouts(M, LayoutFunctions);

      if(LayoutFunctions.empty())
        return false;

      Function* Register = declareRegisterLayout(M);
      registerLayouts(LayoutFunctions, Register);

      return true;
    }

    static void collectLayouts(Module &M, SmallVectorImpl<Function*> &LayoutFunctions) {
      for(Function &F : M)
        if(F.getSection() == LAYOUT_SECTION)
          LayoutFunctions.push_back(&F);
    }

    static Function* GetSerializeStruct(Function* F) {
      // fragile!
      for(Instruction &I : llvm::instructions(F)) {
        if(isa<IntrinsicInst>(I))
          continue;
        for(Value* Op : I.operands()) {
          if(ConstantExpr* OpCE = dyn_cast<ConstantExpr>(Op))
            if(OpCE->getOpcode() == Instruction::BitCast)
              Op = OpCE->getOperand(0);
          if(Function* F = dyn_cast<Function>(Op))
            return F;
        }
      }
      assert(false && "unreachable");
      return nullptr;
    }

    static Function* DeclareMalloc(Module &M) {
      if(Function* Malloc = M.getFunction("malloc"))
        return Malloc;
      Type* IntPtrTy = M.getDataLayout().getIntPtrType(M.getContext());
      Type* I8PtrTy = Type::getInt8PtrTy(M.getContext());
      FunctionType* FTy = FunctionType::get(I8PtrTy, {IntPtrTy}, false);
      return Function::Create(FTy, Function::ExternalLinkage, "malloc", &M);
    }

    template<class TypeIterator>
    static size_t GetStructSize(TypeIterator begin, TypeIterator end, DataLayout const &DL) {
      return std::accumulate(begin, end, 0ul,
                             [&DL](size_t A, Type* T) { return A + DL.getTypeStoreSize(T); } );
    }

    static void SerializeStruct(IRBuilder<> &B, size_t &Offset, DataLayout const & DL,
                                Value* Buf, Value* ByVal, Type* CurLevelTy, SmallVectorImpl<Value*> &GEPOffset) {
      StructType* Struct = dyn_cast<StructType>(CurLevelTy);
      if(!Struct) {
        Value* ArgPtr = B.CreateGEP(ByVal->getType(), ByVal, GEPOffset);
        Value* Argument = B.CreateLoad(ArgPtr->getType(), ArgPtr);

        Value* Ptr = B.CreateConstGEP1_32(Buf->getType(), Buf, Offset);
        B.CreateStore(Argument, Ptr);

        Offset += DL.getTypeStoreSize(Argument->getType());
        return;
      }

      Type* I32Ty = Type::getInt32Ty(B.getContext());
      GEPOffset.push_back(nullptr);
      for(size_t Arg = 0; Arg != Struct->getNumElements(); Arg++) {
        GEPOffset.back() = ConstantInt::get(I32Ty, Arg);
        SerializeStruct(B, Offset, DL, Buf, ByVal, Struct->getElementType(Arg), GEPOffset);
      }
      GEPOffset.pop_back();
    }

    static void SerializeArguments(IRBuilder<> &B, size_t &Offset, DataLayout const & DL, Value* Buf, Function* F) {
      for(size_t Arg = 0; Arg != F->arg_size(); Arg++) {
        Value *Argument = F->arg_begin()+Arg;
        Type* ArgTy = Argument->getType();

        Value* Ptr = B.CreateConstGEP1_32(Buf->getType(), Buf, Offset);
        Ptr = B.CreatePointerCast(Ptr, PointerType::getUnqual(ArgTy), Argument->getName() + ".ptr");
        B.CreateStore(Argument, Ptr);

        Offset += DL.getTypeStoreSize(Argument->getType());
      }
    }

    static void DefineSerializeStruct(Function *F) {
      if(!F->isDeclaration())
        return;

      LLVMContext &C = F->getContext();

      Module &M = *F->getParent();
      Function *Malloc = DeclareMalloc(M);

      DataLayout const &DL = M.getDataLayout();

      FunctionType *FTy = F->getFunctionType();
      StructType *STy = F->arg_begin()->hasByValAttr() ? cast<StructType>(FTy->getParamType(0)->getContainedType(0)) : nullptr;
      bool PassedAsAPointer = STy;

      Type* I32 = Type::getInt32Ty(C);
      Type* I32Ptr = PointerType::getUnqual(I32);
      size_t I32Size = DL.getTypeStoreSize(I32);

      BasicBlock *BB = BasicBlock::Create(C, "entry", F);
      IRBuilder<> B(BB);

      size_t ArgSize;
      if(PassedAsAPointer) ArgSize = GetStructSize(STy->element_begin(), STy->element_end(), DL);
      else ArgSize = GetStructSize(FTy->param_begin(), FTy->param_end(), DL);

      Value* Buf = B.CreateCall(Malloc, {ConstantInt::get(Malloc->getFunctionType()->getParamType(0), I32Size + ArgSize)}, "buf");
      B.CreateStore(ConstantInt::get(I32, ArgSize), B.CreatePointerCast(Buf, I32Ptr, "size.ptr"));

      SmallVector<Value*, 2> Offset = { ConstantInt::getNullValue(I32) };
      if(PassedAsAPointer) SerializeStruct(B, I32Size, DL, Buf, F->arg_begin(), STy, Offset);
      else SerializeArguments(B, I32Size, DL, Buf, F);

      B.CreateRet(Buf);
    }

    static void registerLayouts(SmallVectorImpl<Function*> &LayoutFunctions, Function* Register) {

      FunctionType* RegisterTy = Register->getFunctionType();
      Type* IdTy = RegisterTy->getParamType(0);
      Type* NTy = RegisterTy->getParamType(1);

      // register the layout info in a constructor
      Function* Ctor = GetCtor(*Register->getParent(), "register_layout");
      IRBuilder<> B(Ctor->getEntryBlock().getTerminator());

      for(Function *F : LayoutFunctions) {
        Function* SerializeStructFun = GetSerializeStruct(F);

        DefineSerializeStruct(SerializeStructFun);

        size_t N = SerializeStructFun->getFunctionType()->getNumParams();
        Value* Id = B.CreatePointerCast(SerializeStructFun, IdTy);
        B.CreateCall(Register, {Id, ConstantInt::get(NTy, N, false)});
      }
    }

    static Function* declareRegisterLayout(Module &M) {
      StringRef Name = "easy_register_layout";
      if(Function* F = M.getFunction(Name))
        return F;

      LLVMContext &C = M.getContext();
      DataLayout const &DL = M.getDataLayout();

      Type* Void = Type::getVoidTy(C);
      Type* I8Ptr = Type::getInt8PtrTy(C);
      Type* SizeT = DL.getLargestLegalIntType(C);

      FunctionType* FTy =
          FunctionType::get(Void, {I8Ptr, SizeT}, false);
      return Function::Create(FTy, Function::ExternalLinkage, Name, &M);
    }
  };

  char RegisterLayout::ID = 0;

  llvm::Pass* createRegisterLayoutPass() {
    return new RegisterLayout();
  }
}
