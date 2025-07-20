// Result of testing, just keep in case we need it

#include "WebAssembly.h"
#include "WebAssemblySubtarget.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
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
  // Our backend, including WebAssemblyDebugValueManager, currently cannot
  // handle DBG_VALUE_LISTs correctly. So this makes them undefined, which will
  // appear as "optimized out".
  llvm::outs() << "Running WASLR Pass!\n";
  /**for (auto &MBB : MF) {
    for (auto &MI : MBB) {
      if (MI.getOpcode() == TargetOpcode::DBG_VALUE_LIST) {
        MI.setDebugValueUndef();
        Changed = true;
      }
    }
  }**/
   
  return Changed;
}