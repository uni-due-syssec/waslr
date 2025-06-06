//===-- HelloWorld.cpp - Example Transformations --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "WASLR.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include "llvm/IR/Module.h"

using namespace llvm;

PreservedAnalyses WASLR::run(Module &M, ModuleAnalysisManager &AM) {
  outs() << "Running WASLR Pass on " << M.getName() << "\n";
  outs() << "Globals Addr Space: " << M.getDataLayout().getDefaultGlobalsAddressSpace() << "\n";
  for (Function &F : M) {
    waslr::handleFunction(F);
  }
  waslr::randomizeStackBase(M);
  return PreservedAnalyses::all();
}

bool WASLR::isRequired(){
  return true;
}

namespace waslr {
    void handleFunction(Function &F) {
      llvm::StringRef funcName = F.getName();
      if (F.isIntrinsic()){
        outs() << "Function " << funcName << " Intrinsic! Linkage: " << F.getLinkage() << "\n";
        return;
      }
      if (F.empty()){
        outs() << "Function " << funcName << " empty! Linkage: " << F.getLinkage() << "\n";
        return;
      }
      if (F.getLinkage() == GlobalValue::AvailableExternallyLinkage) {
        outs() << "Function " << funcName << " not defined in this Module!\n";
        return;
      }

      outs() << "Found Function " << funcName << "Linkage: " << F.getLinkage() << "\n";
      /*for (BasicBlock &BB : F) {
        for (Instruction &Inst : BB) {
          handleInstruction(Inst);
        }
      }*/
  }

  /**
   * Notes:
   * - have a look at visitors in the future
   * - MemorySSA could be helpful for data flow analysis
   */
  void handleInstruction(Instruction &Inst) {
    // Try casting to Store / Load Instruction and check if not null
    if (StoreInst *SI = dyn_cast<StoreInst>(&Inst)) {
      // Write Memory
      // get Address Space of the pointer Operand
      unsigned addrspace = SI->getPointerAddressSpace();
      outs() << "Found Instruction that reads from memory! ADDRSPACE: " << addrspace << "\n";
    } else if (LoadInst *LI = dyn_cast<LoadInst>(&Inst)) {
      // Read Memory
      // get Address Space of the pointer Operand
      unsigned addrspace = LI->getPointerAddressSpace();
      outs() << "Found Instruction that writes to memory! ADDRSPACE: " << addrspace << "\n";
    }
  }

  /**
   * Randomizes the base offset of the stack pointer
   * the stack pointer does not exist in LLVM IR, so we declare it in hopes that wasm-lld will see it and not overwrite
   */
  void randomizeStackBase(Module &M){
    outs() << "Randomizing Stack\n";
    LLVMContext &Ctx = M.getContext();
    Type *i32Ty = Type::getInt32Ty(Ctx);

    // Declare __stack_pointer as external to be resolved later
    // Address Space 1 = Wasm globals (https://github.com/emscripten-core/emscripten/issues/12793#issuecomment-915251249)
    GlobalVariable *stackPtr = new GlobalVariable(M, i32Ty, false, GlobalValue::ExternalLinkage, nullptr, "__stack_pointer", nullptr, GlobalValue::NotThreadLocal, 1);
    // __stack_pointer may/will come from a different linkage unit
    stackPtr->setDSOLocal(false);

    FunctionType *initFuncTy = FunctionType::get(Type::getVoidTy(Ctx), {}, false);
    // WeakODRLinkage: To merge duplicate functions at link time
    Function *initFunc = Function::Create(initFuncTy, GlobalValue::WeakODRLinkage, "init_stack_pointer", &M);
    initFunc->addFnAttr("wasm-export-name", "init_stack_pointer");

    BasicBlock *entryBB = BasicBlock::Create(Ctx, "entry", initFunc);
    IRBuilder<> builder(entryBB);

    // Insert call to print debug message
    Type *i8PtrTy = PointerType::getInt8Ty(Ctx);
    FunctionType *consoleFuncTy = FunctionType::get(Type::getVoidTy(Ctx), {i8PtrTy}, false);
    Function *consoleFunc = M.getFunction("console");
    if (!consoleFunc) {
        consoleFunc = Function::Create(consoleFuncTy, GlobalValue::ExternalLinkage, "console", &M);
    }
  
    Constant *debugStr = builder.CreateGlobalStringPtr("Setting stack pointer to 69420");
    builder.CreateCall(consoleFunc, {debugStr});

    // Randomize stack pointer value (example constant here)
    uint32_t randOffset = 69420; 
    Constant *randVal = ConstantInt::get(i32Ty, randOffset);
    builder.CreateStore(randVal, stackPtr);
    builder.CreateRetVoid();

    // Alternative: Use inline assembly to set the stack pointer
    /*std::string asmStr = "i32.const 69420\nglobal.set __stack_pointer";
    FunctionType *asmFuncTy = FunctionType::get(Type::getVoidTy(Ctx), {}, false);
    InlineAsm *inlineAsm = InlineAsm::get(asmFuncTy, asmStr, "", true, false);
    builder.CreateCall(inlineAsm);

    builder.CreateRetVoid();*/

    // LLVM Global Constructors are added to WASM GCs by the backend
    // does not help rn, because ctors are not automatically called on instantiation. Also, since wasm-ld doesnt support LTO plugins, it leads to duplicate calls (could add a global variable check but thats not as clean as simply exporting and calling a function) 
    // llvm::appendToGlobalCtors(M, initFunc, 0);

    //printGlobals(M);
  }

  // TODO for static data
  void printGlobals(Module &M) {
    
    for (auto &GV : M.globals()) {
      outs() << "Global: " << GV.getName() << "\n";
      errs() << "  Type: ";
      GV.getType()->print(errs());
      errs() << "\n";

      errs() << "  Is Constant: " << (GV.isConstant() ? "Yes" : "No") << "\n";
      errs() << "  Linkage: " << GV.getLinkage() << "\n";
      errs() << "  Visibility: " << GV.getVisibility() << "\n";
      errs() << "  Address Space: " << GV.getAddressSpace() << "\n";
      errs() << " ------ \n";
      for (User *U : GV.users()){
        if (Instruction *I = dyn_cast<Instruction>(U)) {
            errs() << " INST FOUND \n";
            I->print(errs());
            errs() << "\n";
        } else if (ConstantExpr *CE = dyn_cast<ConstantExpr>(U)) {
            errs() << " CONST FOUND \n";
            I->print(errs());
            errs() << "\n";
        }
      }

      if (GV.hasInitializer()) {
          errs() << "  Initializer: ";
          GV.getInitializer()->print(errs());
          errs() << "\n";
      }
    }
  }
}

PassPluginLibraryInfo getPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "WASLR", LLVM_VERSION_STRING,
    [](PassBuilder &PB) {
      // Needed if we want to run it by name
      /*PB.registerPipelineParsingCallback(
          [&](StringRef Name, ModulePassManager &MPM,
            ArrayRef<PassBuilder::PipelineElement>) {
          if (Name == "waslr") {
            outs() << "Enabled My Pass \n"; // debug
            MPM.addPass(WASLR());
            return true;
          }
          return false;
      });*/
      // Needed if we want full LTO? Maybe
      /*PB.registerFullLinkTimeOptimizationLastEPCallback(
        [&](ModulePassManager &MPM, OptimizationLevel OL) {
          outs() << "HELLO\n";
          MPM.addPass(WASLR());
        }
      );*/
      // Needed if we want to run it during compilation (at the end)
      PB.registerOptimizerLastEPCallback(
        [&](ModulePassManager &MPM, OptimizationLevel OL) {
          MPM.addPass(WASLR());
          return true;
        }
      );
      // Note: in LLVM 20, LTO should work like this: PB.registerOptimizerLastEPCallback(
      //  [&](ModulePassManager &MPM, OptimizationLevel OL, ThinOrFullLTOPhase Phase) {
    }};
}

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  outs() << "WASLR Plugin Loaded\n";
  return getPassPluginInfo();
}
