// Result of testing, just keep in case we need it

#include "WebAssembly.h"
#include "WebAssemblyMachineFunctionInfo.h"
#include "WebAssemblySubtarget.h"
#include "WebAssemblyUtilities.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
using namespace llvm;

#define DEBUG_TYPE "wasm-waslr"

namespace {
class WebAssemblyWASLR final : public MachineFunctionPass {
  StringRef getPassName() const override {
    return "WebAssembly WASLR";
  }

  bool runOnMachineFunction(MachineFunction &MF) override;

  public:
  static char ID; // Pass identification, replacement for typeid
  WebAssemblyWASLR() : MachineFunctionPass(ID) {}
};
} // end anonymous namespace

char WebAssemblyWASLR::ID = 0;
INITIALIZE_PASS(WebAssemblyWASLR, DEBUG_TYPE,
                "WebAssembly Nullify DBG_VALUE_LISTs", false, false)

FunctionPass *llvm::createWebAssemblyWASLR() {
  return new WebAssemblyWASLR();
}

bool WebAssemblyWASLR::runOnMachineFunction(
    MachineFunction &MF) {
  LLVM_DEBUG(dbgs() << "********** Nullify DBG_VALUE_LISTs **********\n"
                       "********** Function: "
                    << MF.getName() << '\n');
  bool Changed = false;
  if (!MF.getFrameInfo().hasVarSizedObjects()) {
    return Changed;
  }
  llvm::outs() << "Function: " << MF.getName() << " has VSOs\n";
  for (auto &MBB : MF) {
    for (auto &MI : MBB) {
      if ((MI.getOpcode() == WebAssembly::GLOBAL_SET_I32 ||
       MI.getOpcode() == WebAssembly::GLOBAL_SET_I64) &&
      strcmp(MI.getOperand(0).getSymbolName(), "__stack_pointer") == 0) {
        llvm::outs() << "Found Stack Pointer Update\n";
      }
    }
  }
   
  return Changed;
}

// Look at WebAssemblyRegStackify for good examples