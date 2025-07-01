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
  /*for (Function &F : M) {
    waslr::handleFunction(F);
  }*/
  waslr::randomize(M);
  return PreservedAnalyses::all();
}

bool WASLR::isRequired(){
  return true;
}

namespace waslr {

  /**
   * Notes:
   * - have a look at visitors in the future
   * - MemorySSA could be helpful for data flow analysis
   */

  /**
   * Randomizes the base offset of the stack pointer
   * the stack pointer does not exist in LLVM IR, so we declare it in hopes that wasm-lld will see it and not overwrite
   */
  void randomize(Module &M){
    LLVMContext &Ctx = M.getContext();

    FunctionType *initFuncTy = FunctionType::get(Type::getVoidTy(Ctx), {}, false);
    // WeakODRLinkage: To merge duplicate functions at link time
    //Function *initFunc = Function::Create(initFuncTy, GlobalValue::WeakODRLinkage, "init_waslr", &M);
    //initFunc->addFnAttr("wasm-export-name", "init_waslr");

    //randomizeStackBase(M, Ctx, builder);
    randomizeFunctionStackFrames(M, Ctx);

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

  void debugPrint(Module &M, LLVMContext &Ctx, IRBuilder<> &builder, StringRef msg){
    // Insert call to print debug message
    Type *i8PtrTy = PointerType::getInt8Ty(Ctx);
    FunctionType *consoleFuncTy = FunctionType::get(Type::getVoidTy(Ctx), {i8PtrTy}, false);
    Function *consoleFunc = M.getFunction("console");
    if (!consoleFunc) {
        consoleFunc = Function::Create(consoleFuncTy, GlobalValue::ExternalLinkage, "console", &M);
    }
  
    Constant *debugStr = builder.CreateGlobalStringPtr(msg);
    builder.CreateCall(consoleFunc, {debugStr});
  }

  /*
    Currently, for testing, we insert a global for our random offset that is first imported and then used to generate a random value inside the module. 
  */
  void insertRandomValue(Module &M, LLVMContext &Ctx, IRBuilder<> &builder) {
    Type *i32Ty = Type::getInt32Ty(Ctx);
    GlobalVariable *waslr_value = new GlobalVariable(M, i32Ty, false, GlobalValue::InternalLinkage, nullptr, "__waslr_value", nullptr, GlobalValue::NotThreadLocal, 1);
    uint32_t randOffset = 10; // static right now 
    Constant *randVal = ConstantInt::get(i32Ty, randOffset);
    Value *curVal = builder.CreateLoad(i32Ty, waslr_value, "waslr_old");
    Value *newVal = builder.CreateAdd(curVal, randVal, "waslr_new");
    builder.CreateStore(newVal, waslr_value);
  }

  void randomizeStackBase(Module &M, LLVMContext &Ctx, IRBuilder<> &builder){
    outs() << "Randomizing Stack\n";
    Type *i32Ty = Type::getInt32Ty(Ctx);
    // Declare __stack_pointer as external to be resolved later
    // Address Space 1 = Wasm globals (https://github.com/emscripten-core/emscripten/issues/12793#issuecomment-915251249)
    GlobalVariable *stackPtr = new GlobalVariable(M, i32Ty, false, GlobalValue::ExternalLinkage, nullptr, "__stack_pointer", nullptr, GlobalValue::NotThreadLocal, 1);
    // __stack_pointer may/will come from a different linkage unit
    stackPtr->setDSOLocal(false);

    // Randomize stack pointer value (example constant here)
    uint32_t randOffset = 69420; 
    Constant *randVal = ConstantInt::get(i32Ty, randOffset);
    builder.CreateStore(randVal, stackPtr);
  }


  void randomizeFunctionStackFrames(Module &M, LLVMContext &Ctx) {
    // get stack bounds and stack pointer
    Type *i32Ty = Type::getInt32Ty(Ctx);
    PointerType *ptrTy = PointerType::getUnqual(i32Ty);

    FunctionCallee prol_f = M.getOrInsertFunction("__waslr_stack_prol", FunctionType::get(Type::getVoidTy(Ctx), {ptrTy}, false));
    FunctionCallee epi_f = M.getOrInsertFunction("__waslr_stack_epil", FunctionType::get(Type::getVoidTy(Ctx), {ptrTy}, false));
      
    for (Function &F : M) {
      outs() << "Function: " << F.getName() << "\n";
      if (F.empty()) continue;

      StringRef Name = F.getName();
      if (Name == "__waslr_stack_prol" || Name == "__waslr_stack_epil")
        continue;

      // Instrument entry
      BasicBlock &entry = F.getEntryBlock();
      IRBuilder<> builder(&*entry.getFirstInsertionPt());

      AllocaInst *stack_size = builder.CreateAlloca(i32Ty, nullptr, "stack_size");

      builder.CreateCall(prol_f, {stack_size});

      // Instrument return blocks
      for (BasicBlock &BB: F) {
        Instruction *term = BB.getTerminator();
        if(term && isa<ReturnInst>(term)) {
          IRBuilder<> builder(term);
          builder.CreateCall(epi_f, {stack_size});
        }
      }
    }
  }

  int getReadOnlySize(Module &M) {
    for (auto &GV : M.globals()){
      // how to do it across modules without LTO?
      // if one module has X bytes of RO data, while another has Y bytes 
    }
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

      if (GV.hasSection()) {
        errs() << " Section: " << GV.getSection() << "\n";
      } else {
        errs() << "NO Section\n";
      }

      errs() << " ------ \n\n";
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
