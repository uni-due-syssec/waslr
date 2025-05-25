#include "llvm/IR/PassManager.h"
#include "llvm/IR/Instruction.h"

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
  void randomizeStackBase(llvm::Module &M);
  void printGlobals(llvm::Module &M);
}