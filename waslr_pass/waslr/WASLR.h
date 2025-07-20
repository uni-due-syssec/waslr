#include "llvm/IR/PassManager.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/IRBuilder.h"

namespace llvm {

  class WASLR : public PassInfoMixin<WASLR> {
  public:
    PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
    static bool isRequired();
  };

}

namespace waslr {
  void handleFunction(llvm::Function &F);
  void handleInstruction(llvm::Instruction &Inst);
  void printSFSizeTest(llvm::Module &M);
  void randomize(llvm::Module &M);
  void insertRandomValue(llvm::Module &M, llvm::LLVMContext &Ctx, llvm::IRBuilder<> &builder);
  void randomizeStackBase(llvm::Module &M, llvm::LLVMContext &Ctx, llvm::IRBuilder<> &builder);
  void randomizeFunctionStackFrames(llvm::Module &M, llvm::LLVMContext &Ctx);
  void debugPrint(llvm::Module &M, llvm::LLVMContext &Ctx, llvm::IRBuilder<> &builder, llvm::StringRef msg);
  void printGlobals(llvm::Module &M);
}