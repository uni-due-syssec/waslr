//===-- WebAssemblyFrameLowering.cpp - WebAssembly Frame Lowering ----------==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file contains the WebAssembly implementation of
/// TargetFrameLowering class.
///
/// On WebAssembly, there aren't a lot of things to do here. There are no
/// callee-saved registers to save, and no spill slots.
///
/// The stack grows downward.
///
//===----------------------------------------------------------------------===//

#include "WebAssemblyFrameLowering.h"
#include "MCTargetDesc/WebAssemblyMCTargetDesc.h"
#include "Utils/WebAssemblyTypeUtilities.h"
#include "WebAssembly.h"
#include "WebAssemblyInstrInfo.h"
#include "WebAssemblyMachineFunctionInfo.h"
#include "WebAssemblySubtarget.h"
#include "WebAssemblyTargetMachine.h"
#include "llvm/CodeGen/Analysis.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/MCAsmInfo.h"
using namespace llvm;

#define DEBUG_TYPE "wasm-frame-info"

// TODO: wasm64
// TODO: Emit TargetOpcode::CFI_INSTRUCTION instructions

// In an ideal world, when objects are added to the MachineFrameInfo by
// FunctionLoweringInfo::set, we could somehow hook into target-specific code to
// ensure they are assigned the right stack ID.  However there isn't a hook that
// runs between then and DAG building time, though, so instead we hoist stack
// objects lazily when they are first used, and comprehensively after the DAG is
// built via the PreprocessISelDAG hook, called by the
// SelectionDAGISel::runOnMachineFunction.  We have to do it in two places
// because we want to do it while building the selection DAG for uses of alloca,
// but not all alloca instructions are used so we have to follow up afterwards.
std::optional<unsigned>
WebAssemblyFrameLowering::getLocalForStackObject(MachineFunction &MF,
                                                 int FrameIndex) {
  MachineFrameInfo &MFI = MF.getFrameInfo();

  // If already hoisted to a local, done.
  if (MFI.getStackID(FrameIndex) == TargetStackID::WasmLocal)
    return static_cast<unsigned>(MFI.getObjectOffset(FrameIndex));

  // If not allocated in the object address space, this object will be in
  // linear memory.
  const AllocaInst *AI = MFI.getObjectAllocation(FrameIndex);
  if (!AI || !WebAssembly::isWasmVarAddressSpace(AI->getAddressSpace()))
    return std::nullopt;

  // Otherwise, allocate this object in the named value stack, outside of linear
  // memory.
  SmallVector<EVT, 4> ValueVTs;
  const WebAssemblyTargetLowering &TLI =
      *MF.getSubtarget<WebAssemblySubtarget>().getTargetLowering();
  WebAssemblyFunctionInfo *FuncInfo = MF.getInfo<WebAssemblyFunctionInfo>();
  ComputeValueVTs(TLI, MF.getDataLayout(), AI->getAllocatedType(), ValueVTs);
  MFI.setStackID(FrameIndex, TargetStackID::WasmLocal);
  // Abuse SP offset to record the index of the first local in the object.
  unsigned Local = FuncInfo->getParams().size() + FuncInfo->getLocals().size();
  MFI.setObjectOffset(FrameIndex, Local);
  // Allocate WebAssembly locals for each non-aggregate component of the
  // allocation.
  for (EVT ValueVT : ValueVTs)
    FuncInfo->addLocal(ValueVT.getSimpleVT());
  // Abuse object size to record number of WebAssembly locals allocated to
  // this object.
  MFI.setObjectSize(FrameIndex, ValueVTs.size());
  return static_cast<unsigned>(Local);
}

/// We need a base pointer in the case of having items on the stack that
/// require stricter alignment than the stack pointer itself.  Because we need
/// to shift the stack pointer by some unknown amount to force the alignment,
/// we need to record the value of the stack pointer on entry to the function.
bool WebAssemblyFrameLowering::hasBP(const MachineFunction &MF) const {
  const auto *RegInfo =
      MF.getSubtarget<WebAssemblySubtarget>().getRegisterInfo();
  return RegInfo->hasStackRealignment(MF);
}

/// Return true if the specified function should have a dedicated frame pointer
/// register.
bool WebAssemblyFrameLowering::hasFPImpl(const MachineFunction &MF) const {
  const MachineFrameInfo &MFI = MF.getFrameInfo();

  // When we have var-sized objects, we move the stack pointer by an unknown
  // amount, and need to emit a frame pointer to restore the stack to where we
  // were on function entry.
  // If we already need a base pointer, we use that to fix up the stack pointer.
  // If there are no fixed-size objects, we would have no use of a frame
  // pointer, and thus should not emit one.
  bool HasFixedSizedObjects = MFI.getStackSize() > 0;
  bool NeedsFixedReference = !hasBP(MF) || HasFixedSizedObjects;

  return MFI.isFrameAddressTaken() ||
         (MFI.hasVarSizedObjects() && NeedsFixedReference) ||
         MFI.hasStackMap() || MFI.hasPatchPoint();
}

/// Under normal circumstances, when a frame pointer is not required, we reserve
/// argument space for call sites in the function immediately on entry to the
/// current function. This eliminates the need for add/sub sp brackets around
/// call sites. Returns true if the call frame is included as part of the stack
/// frame.
bool WebAssemblyFrameLowering::hasReservedCallFrame(
    const MachineFunction &MF) const {
  return !MF.getFrameInfo().hasVarSizedObjects();
}

// Returns true if this function needs a local user-space stack pointer for its
// local frame (not for exception handling).
bool WebAssemblyFrameLowering::needsSPForLocalFrame(
    const MachineFunction &MF) const {
  auto &MFI = MF.getFrameInfo();
  auto &MRI = MF.getRegInfo();
  // llvm.stacksave can explicitly read SP register and it can appear without
  // dynamic alloca.
  bool HasExplicitSPUse =
      any_of(MRI.use_operands(getSPReg(MF)),
             [](MachineOperand &MO) { return !MO.isImplicit(); });

  return MFI.getStackSize() || MFI.adjustsStack() || hasFP(MF) ||
         HasExplicitSPUse;
}

// In function with EH pads, we need to make a copy of the value of
// __stack_pointer global in SP32/64 register, in order to use it when
// restoring __stack_pointer after an exception is caught.
bool WebAssemblyFrameLowering::needsPrologForEH(
    const MachineFunction &MF) const {
  auto EHType = MF.getTarget().getMCAsmInfo()->getExceptionHandlingType();
  return EHType == ExceptionHandling::Wasm &&
         MF.getFunction().hasPersonalityFn() && MF.getFrameInfo().hasCalls();
}

/// Returns true if this function needs a local user-space stack pointer.
/// Unlike a machine stack pointer, the wasm user stack pointer is a global
/// variable, so it is loaded into a register in the prolog.
bool WebAssemblyFrameLowering::needsSP(const MachineFunction &MF) const {
  return needsSPForLocalFrame(MF) || needsPrologForEH(MF);
}

/// Returns true if the local user-space stack pointer needs to be written back
/// to __stack_pointer global by this function (this is not meaningful if
/// needsSP is false). If false, the stack red zone can be used and only a local
/// SP is needed.
bool WebAssemblyFrameLowering::needsSPWriteback(
    const MachineFunction &MF) const {
  auto &MFI = MF.getFrameInfo();
  assert(needsSP(MF));
  // When we don't need a local stack pointer for its local frame but only to
  // support EH, we don't need to write SP back in the epilog, because we don't
  // bump down the stack pointer in the prolog. We need to write SP back in the
  // epilog only if
  // 1. We need SP not only for EH support but also because we actually use
  // stack or we have a frame address taken.
  // 2. We cannot use the red zone.
  bool CanUseRedZone = MFI.getStackSize() <= RedZoneSize && !MFI.hasCalls() &&
                       !MF.getFunction().hasFnAttribute(Attribute::NoRedZone);
  return needsSPForLocalFrame(MF) && !CanUseRedZone;
}

unsigned WebAssemblyFrameLowering::getSPReg(const MachineFunction &MF) {
  return MF.getSubtarget<WebAssemblySubtarget>().hasAddr64()
             ? WebAssembly::SP64
             : WebAssembly::SP32;
}

unsigned WebAssemblyFrameLowering::getFPReg(const MachineFunction &MF) {
  return MF.getSubtarget<WebAssemblySubtarget>().hasAddr64()
             ? WebAssembly::FP64
             : WebAssembly::FP32;
}

unsigned
WebAssemblyFrameLowering::getOpcConst(const MachineFunction &MF) {
  return MF.getSubtarget<WebAssemblySubtarget>().hasAddr64()
             ? WebAssembly::CONST_I64
             : WebAssembly::CONST_I32;
}

unsigned WebAssemblyFrameLowering::getOpcAdd(const MachineFunction &MF) {
  return MF.getSubtarget<WebAssemblySubtarget>().hasAddr64()
             ? WebAssembly::ADD_I64
             : WebAssembly::ADD_I32;
}

unsigned WebAssemblyFrameLowering::getOpcSub(const MachineFunction &MF) {
  return MF.getSubtarget<WebAssemblySubtarget>().hasAddr64()
             ? WebAssembly::SUB_I64
             : WebAssembly::SUB_I32;
}

unsigned WebAssemblyFrameLowering::getOpcAnd(const MachineFunction &MF) {
  return MF.getSubtarget<WebAssemblySubtarget>().hasAddr64()
             ? WebAssembly::AND_I64
             : WebAssembly::AND_I32;
}

unsigned
WebAssemblyFrameLowering::getOpcGlobGet(const MachineFunction &MF) {
  return MF.getSubtarget<WebAssemblySubtarget>().hasAddr64()
             ? WebAssembly::GLOBAL_GET_I64
             : WebAssembly::GLOBAL_GET_I32;
}

unsigned
WebAssemblyFrameLowering::getOpcGlobSet(const MachineFunction &MF) {
  return MF.getSubtarget<WebAssemblySubtarget>().hasAddr64()
             ? WebAssembly::GLOBAL_SET_I64
             : WebAssembly::GLOBAL_SET_I32;
}

wasm::ValType 
WebAssemblyFrameLowering::getIValueType(const MachineFunction &MF) {
  return MF.getSubtarget<WebAssemblySubtarget>().hasAddr64()
             ? wasm::ValType::I64
             : wasm::ValType::I32;
}

void WebAssemblyFrameLowering::writeSPToGlobal(
    unsigned SrcReg, MachineFunction &MF, MachineBasicBlock &MBB,
    MachineBasicBlock::iterator &InsertStore, const DebugLoc &DL) const {
  const auto *TII = MF.getSubtarget<WebAssemblySubtarget>().getInstrInfo();

  const char *ES = "__stack_pointer";
  auto *SPSymbol = MF.createExternalSymbolName(ES);

  BuildMI(MBB, InsertStore, DL, TII->get(getOpcGlobSet(MF)))
      .addExternalSymbol(SPSymbol)
      .addReg(SrcReg);
}

MachineBasicBlock::iterator
WebAssemblyFrameLowering::eliminateCallFramePseudoInstr(
    MachineFunction &MF, MachineBasicBlock &MBB,
    MachineBasicBlock::iterator I) const {
  assert(!I->getOperand(0).getImm() && (hasFP(MF) || hasBP(MF)) &&
         "Call frame pseudos should only be used for dynamic stack adjustment");
  auto &ST = MF.getSubtarget<WebAssemblySubtarget>();
  const auto *TII = ST.getInstrInfo();
  if (I->getOpcode() == TII->getCallFrameDestroyOpcode() &&
      needsSPWriteback(MF)) {
    DebugLoc DL = I->getDebugLoc();
    writeSPToGlobal(getSPReg(MF), MF, MBB, I, DL);
  }
  return MBB.erase(I);
}

void copyValTypes(ArrayRef<wasm::ValType> In, SmallVectorImpl<wasm::ValType> &Out){
  Out.clear();
  for (wasm::ValType Ty : In)
    Out.push_back(Ty);
}

void setFunctionSignature(MCSymbolWasm *FnSym, MCContext &Ctx, ArrayRef<wasm::ValType> Params = {}, ArrayRef<wasm::ValType> Results = {}) { 
  FnSym->setType(wasm::WASM_SYMBOL_TYPE_FUNCTION);

  auto *Sig = Ctx.createWasmSignature();
  copyValTypes(Params, Sig->Params);
  copyValTypes(Results, Sig->Returns);

  FnSym->setSignature(Sig);
}

namespace llvm {
  struct WasmPEContext {
    const WebAssemblyInstrInfo *TII;
    MachineRegisterInfo &MRI;
    MachineBasicBlock::iterator InsertPt;
    const TargetRegisterClass *PtrRC;
    unsigned SPRegister; 
    uint64_t SFSize;
    DebugLoc DL;
  };
}

void WebAssemblyFrameLowering::emitPrologue(MachineFunction &MF,
                                            MachineBasicBlock &MBB) const {
  // Maybe do ".setMIFlag(MachineInstr::FrameSetup)" on emitted instructions
  auto &MFI = MF.getFrameInfo();
  assert(MFI.getCalleeSavedInfo().empty() &&
         "WebAssembly should not have callee-saved registers");

  auto FI = MF.getInfo<WebAssemblyFunctionInfo>();
  if (MF.getName() == "malloc" || MF.getName() == "free") {
    FI->setIsAllocFn(true);   
  }

  if (!needsSP(MF) && !FI->isAllocFn()){
    return;
  }
  uint64_t StackSize = MFI.getStackSize();

  auto &ST = MF.getSubtarget<WebAssemblySubtarget>();
  const auto *TII = ST.getInstrInfo();
  auto &MRI = MF.getRegInfo();
  bool waslrEnabled = MF.getTarget().Options.EnableWASLR;

  auto InsertPt = MBB.begin();
  while (InsertPt != MBB.end() &&
         WebAssembly::isArgument(InsertPt->getOpcode()))
    ++InsertPt;
  DebugLoc DL;

  // I32RegClass or I64RegClass depending on Wasm target (Wasm32 vs Wasm64)
  const TargetRegisterClass *PtrRC = MRI.getTargetRegisterInfo()->getPointerRegClass(MF);

  unsigned SPReg = getSPReg(MF); // the actual SP global
  if (StackSize) SPReg = MRI.createVirtualRegister(PtrRC); // a local

  const char *ES = "__stack_pointer";
  auto *SPSymbol = MF.createExternalSymbolName(ES);
  BuildMI(MBB, InsertPt, DL, TII->get(getOpcGlobGet(MF)), SPReg)
      .addExternalSymbol(SPSymbol);

  const WasmPEContext Ctx {
    TII,
    MRI,
    InsertPt,
    PtrRC,
    SPReg,
    StackSize,
    DL,
  };

  bool isRand = false;
  if (waslrEnabled) {
    if (MF.getFunction().hasFnAttribute(llvm::Attribute::WASLRNoRand)){
      if(FI->isAllocFn())
        emitAllocPrologue(MF, MBB, Ctx);
      else 
        emitOrigPrologue(MF, MBB, Ctx);
    } else {
      isRand = true;
      emitRandPrologue(MF, MBB, Ctx);
    }
  } else
    emitOrigPrologue(MF, MBB, Ctx);

  // we always use the base pointer, but hasBP just checks if the stack pointer needs alignment
  if (hasBP(MF)) {
    Register BitmaskReg = MRI.createVirtualRegister(PtrRC);
    Align Alignment = MFI.getMaxAlign();
    BuildMI(MBB, InsertPt, DL, TII->get(getOpcConst(MF)), BitmaskReg)
        .addImm((int64_t) ~(Alignment.value() - 1));
    BuildMI(MBB, InsertPt, DL, TII->get(getOpcAnd(MF)), getSPReg(MF))
        .addReg(getSPReg(MF))
        .addReg(BitmaskReg);
  }

  // in general, our prologues/epilogues NEVER need a frame pointer since we use the base pointer to restore the previous stack pointer
  // we only use the FP in the rand case, since we need the start of the allocated frame to call free in the epilogue
  if (isRand || (!isRand && hasFP(MF))) {
    // Unlike most conventional targets (where FP points to the saved FP),
    // FP points to the bottom of the fixed-size locals, so we can use positive
    // offsets in load/store instructions.
    BuildMI(MBB, InsertPt, DL, TII->get(WebAssembly::COPY), getFPReg(MF))
        .addReg(getSPReg(MF));
  }
  if ((StackSize && needsSPWriteback(MF)) || FI->isAllocFn()) {
    //unsigned SFVreg = FI->getSFPointerVreg();
    //unsigned writebackSrcReg = SFVreg == -1U ? getSPReg(MF) : SFVreg;
    writeSPToGlobal(getSPReg(MF), MF, MBB, InsertPt, DL);
  }
}

void WebAssemblyFrameLowering::emitEpilogue(MachineFunction &MF,
                                            MachineBasicBlock &MBB) const {
  uint64_t StackSize = MF.getFrameInfo().getStackSize();
  auto FI = MF.getInfo<WebAssemblyFunctionInfo>();

  if ((!needsSP(MF) || !needsSPWriteback(MF)) && !FI->isAllocFn()){
    return;
  }
  auto &ST = MF.getSubtarget<WebAssemblySubtarget>();
  const auto *TII = ST.getInstrInfo();
  auto &MRI = MF.getRegInfo();
  auto InsertPt = MBB.getFirstTerminator();
  bool waslrEnabled = MF.getTarget().Options.EnableWASLR;
  DebugLoc DL;

  if (InsertPt != MBB.end())
    DL = InsertPt->getDebugLoc();

  // Restore the stack pointer. If we had fixed-size locals, add the offset
  // subtracted in the prolog.
  unsigned SPReg = 0;
  const TargetRegisterClass *PtrRC = MRI.getTargetRegisterInfo()->getPointerRegClass(MF);

  const WasmPEContext Ctx {
    TII,
    MRI,
    InsertPt,
    PtrRC,
    SPReg,
    StackSize,
    DL,
  };

  if (waslrEnabled) {
    if (MF.getFunction().hasFnAttribute(llvm::Attribute::WASLRNoRand)){
      if(FI->isAllocFn())
        emitAllocEpilogue(MF, MBB, Ctx);
      else
        emitOrigEpilogue(MF, MBB, Ctx);
    } else 
      emitRandEpilogue(MF, MBB, Ctx);
  } else 
    emitOrigEpilogue(MF, MBB, Ctx);
}

void WebAssemblyFrameLowering::emitOrigPrologue(MachineFunction &MF, MachineBasicBlock &MBB, const WasmPEContext &Ctx) const {
  auto *PtrRC = Ctx.PtrRC;
  auto InsertPt = Ctx.InsertPt;
  auto *TII = Ctx.TII;
  auto &MRI = Ctx.MRI;
  auto SPReg = Ctx.SPRegister;
  auto StackSize = Ctx.SFSize;
  auto DL = Ctx.DL;

  if (hasBP(MF)) {
    auto FI = MF.getInfo<WebAssemblyFunctionInfo>();
    Register BasePtr = MRI.createVirtualRegister(PtrRC);
	// BasePtr = LOCAL GET SPReg 
    FI->setBasePointerVreg(BasePtr);
    BuildMI(MBB, InsertPt, DL, TII->get(WebAssembly::COPY), BasePtr)
        .addReg(SPReg);
  }
  
  // if function has Stack Frame, subtract size from __stack_pointer 
  if (StackSize) {
	// Create Register to hold the constant Stack Size
    Register OffsetReg = MRI.createVirtualRegister(PtrRC);
	// OffsetReg = const StackSize 
    BuildMI(MBB, InsertPt, DL, TII->get(getOpcConst(MF)), OffsetReg)
        .addImm(StackSize);
	// SPReg = SPReg - OffsetReg 
    BuildMI(MBB, InsertPt, DL, TII->get(getOpcSub(MF)), getSPReg(MF))
        .addReg(SPReg)
        .addReg(OffsetReg);
  }
}

void WebAssemblyFrameLowering::emitOrigEpilogue(MachineFunction &MF, MachineBasicBlock &MBB, const WasmPEContext &Ctx) const {
  auto *PtrRC = Ctx.PtrRC;
  auto InsertPt = Ctx.InsertPt;
  auto *TII = Ctx.TII;
  auto &MRI = Ctx.MRI;
  auto SPReg = Ctx.SPRegister;
  auto StackSize = Ctx.SFSize;
  auto DL = Ctx.DL;

  unsigned SPFPReg = hasFP(MF) ? getFPReg(MF) : getSPReg(MF);

  if (hasBP(MF)) {
    auto FI = MF.getInfo<WebAssemblyFunctionInfo>();
    SPReg = FI->getBasePointerVreg();
  } else if (StackSize) {
    Register OffsetReg = MRI.createVirtualRegister(PtrRC);
    BuildMI(MBB, InsertPt, DL, TII->get(getOpcConst(MF)), OffsetReg)
        .addImm(StackSize);
    // In the epilog we don't need to write the result back to the SP32/64
    // physreg because it won't be used again. We can use a stackified register
    // instead.
    SPReg = MRI.createVirtualRegister(PtrRC);
    BuildMI(MBB, InsertPt, DL, TII->get(getOpcAdd(MF)), SPReg)
        .addReg(SPFPReg)
        .addReg(OffsetReg);
  } else {
    SPReg = SPFPReg;
  }

  writeSPToGlobal(SPReg, MF, MBB, InsertPt, DL);
}

void WebAssemblyFrameLowering::emitRandPrologue(MachineFunction &MF, MachineBasicBlock &MBB, const WasmPEContext &Ctx) const {
  auto *PtrRC = Ctx.PtrRC;
  auto InsertPt = Ctx.InsertPt;
  auto *TII = Ctx.TII;
  auto &MRI = Ctx.MRI;
  auto SPReg = Ctx.SPRegister;
  auto StackSize = Ctx.SFSize;
  auto DL = Ctx.DL;

  // Instead of only using the BasePtr when required, we use always use it to store the previous Stack Pointer
  // Advantage: We can access the same Register in the Epilogue without further effort
  // Otherwise: Modify WebAssemblyMachineFunctionInfo and add a new field to store "our" Base Pointer

  Register BasePtr = MRI.createVirtualRegister(PtrRC);
  auto FI = MF.getInfo<WebAssemblyFunctionInfo>();
  FI->setBasePointerVreg(BasePtr);
  
  BuildMI(MBB, InsertPt, DL, TII->get(WebAssembly::COPY), BasePtr)
      .addReg(SPReg);

  if (StackSize){
    // get pointer to random allocation of SFSize bytes
    // SFPtr = getRandomChunk(SF_Size)
    //Register SFPtrReg = MRI.createVirtualRegister(PtrRC);
    Register SFSizeReg = MRI.createVirtualRegister(PtrRC);
    // Save Register for Epilogue
    //FI->setSFPointerVreg(SFPtrReg);

    BuildMI(MBB, InsertPt, DL, TII->get(getOpcConst(MF)), SFSizeReg)
      .addImm(StackSize);

    MCSymbolWasm *AllocFn = cast<MCSymbolWasm>(MF.getContext().getOrCreateSymbol("malloc"));
    wasm::ValType VT = getIValueType(MF);
    SmallVector<wasm::ValType, 1> params = {VT};
    SmallVector<wasm::ValType, 1> results = {VT};
    setFunctionSignature(AllocFn, MF.getContext(), params, results);

    BuildMI(MBB, InsertPt, DL, TII->get(WebAssembly::CALL))
      .addDef(getSPReg(MF))
      .addSym(AllocFn)
      .addReg(SFSizeReg);

    /*BuildMI(MBB, InsertPt, DL, TII->get(WebAssembly::COPY), getSPReg(MF))
      .addReg(getFPReg(MF));*/

    // Since we plan to use ralloc for both Stack and Heap, we cannot return a pointer to the END of the chunk by default
    // Instead, we subtract SFSize from the returned pointer
    /*BuildMI(MBB, InsertPt, DL, TII->get(getOpcSub(MF)), getSPReg(MF))
      .addReg(SFPtrReg)
      .addReg(SFSizeReg);*/
  }
}

void WebAssemblyFrameLowering::emitRandEpilogue(MachineFunction &MF, MachineBasicBlock &MBB, const WasmPEContext &Ctx) const {
  auto InsertPt = Ctx.InsertPt;
  auto *TII = Ctx.TII;
  auto SPReg = Ctx.SPRegister;
  auto StackSize = Ctx.SFSize;
  auto DL = Ctx.DL;
  auto FI = MF.getInfo<WebAssemblyFunctionInfo>();
  
  // We always want to restore the original SP from the BP
  SPReg = FI->getBasePointerVreg();
  writeSPToGlobal(SPReg, MF, MBB, InsertPt, DL);
  // free the space allocated for the stack
  if (StackSize) {
    MCSymbolWasm *FreeFn = cast<MCSymbolWasm>(MF.getContext().getOrCreateSymbol("free"));
    wasm::ValType VT = getIValueType(MF);
    SmallVector<wasm::ValType, 1> params = {VT};
    setFunctionSignature(FreeFn, MF.getContext(), params);

    BuildMI(MBB, InsertPt, DL, TII->get(WebAssembly::CALL))
      .addSym(FreeFn)
      .addReg(getFPReg(MF));
  }
}

void WebAssemblyFrameLowering::emitAllocPrologue(MachineFunction &MF, MachineBasicBlock &MBB, const WasmPEContext &Ctx) const {
  auto *PtrRC = Ctx.PtrRC;
  auto InsertPt = Ctx.InsertPt;
  auto *TII = Ctx.TII;
  auto &MRI = Ctx.MRI;
  auto SPReg = Ctx.SPRegister;
  auto StackSize = Ctx.SFSize;
  auto DL = Ctx.DL;

  // Instead of only using the BasePtr when required, we use always use it to store the previous Stack Pointer
  // Advantage: We can access the same Register in the Epilogue without further effort
  // Otherwise: Modify WebAssemblyMachineFunctionInfo and add a new field to store "our" Base Pointer

  Register BasePtr = MRI.createVirtualRegister(PtrRC);
  auto FI = MF.getInfo<WebAssemblyFunctionInfo>();
  FI->setBasePointerVreg(BasePtr);
  
  BuildMI(MBB, InsertPt, DL, TII->get(WebAssembly::COPY), BasePtr)
      .addReg(SPReg);

  Module &M = *MF.getFunction().getParent();
  GlobalVariable *GV = M.getGlobalVariable("__waslr_alloc_stackframes");
  // if function has Stack Frame, subtract size from __stack_pointer 
  if (StackSize) {
    // Note: this works since this prologue is only generated for functions in the waslr runtime, where the global is also defined
    // If the Prologue would be generated for functions in other files, this would probably break
    // But in our case that will never happen.

    Register SPTargetReg = MRI.createVirtualRegister(PtrRC);
    BuildMI(MBB, InsertPt, DL, TII->get(getOpcGlobGet(MF)), SPTargetReg)
        .addGlobalAddress(GV);
    //BuildMI(MBB, InsertPt, DL, TII->get(getOpcConst(MF)), SPTargetReg)
    //    .addImm(allocStackframeStart);
      
	  // Create Register to hold the constant Stack Size
    Register OffsetReg = MRI.createVirtualRegister(PtrRC);
	  // OffsetReg = const StackSize 
    BuildMI(MBB, InsertPt, DL, TII->get(getOpcConst(MF)), OffsetReg)
        .addImm(StackSize);
	  // SPReg = SPReg - OffsetReg 
    BuildMI(MBB, InsertPt, DL, TII->get(getOpcSub(MF)), getSPReg(MF))
        .addReg(SPTargetReg)
        .addReg(OffsetReg);
  } else {
    Register SPTargetReg = MRI.createVirtualRegister(PtrRC);
    BuildMI(MBB, InsertPt, DL, TII->get(getOpcGlobGet(MF)), SPTargetReg)
        .addGlobalAddress(GV);
    //BuildMI(MBB, InsertPt, DL, TII->get(getOpcConst(MF)), SPTargetReg)
    //    .addImm(allocStackframeStart);
    BuildMI(MBB, InsertPt, DL, TII->get(WebAssembly::COPY), getSPReg(MF)).addReg(SPTargetReg);
    //FI->setSFPointerVreg(SPTargetReg);
  }
}

void WebAssemblyFrameLowering::emitAllocEpilogue(MachineFunction &MF, MachineBasicBlock &MBB, const WasmPEContext &Ctx) const {
  auto InsertPt = Ctx.InsertPt;
  auto SPReg = Ctx.SPRegister;
  auto DL = Ctx.DL;
  auto FI = MF.getInfo<WebAssemblyFunctionInfo>();
  
  // We always want to restore the original SP from the BP
  SPReg = FI->getBasePointerVreg();
  writeSPToGlobal(SPReg, MF, MBB, InsertPt, DL);
}

bool WebAssemblyFrameLowering::isSupportedStackID(
    TargetStackID::Value ID) const {
  // Use the Object stack for WebAssembly locals which can only be accessed
  // by name, not via an address in linear memory.
  if (ID == TargetStackID::WasmLocal)
    return true;

  return TargetFrameLowering::isSupportedStackID(ID);
}

TargetFrameLowering::DwarfFrameBase
WebAssemblyFrameLowering::getDwarfFrameBase(const MachineFunction &MF) const {
  DwarfFrameBase Loc;
  Loc.Kind = DwarfFrameBase::WasmFrameBase;
  const WebAssemblyFunctionInfo &MFI = *MF.getInfo<WebAssemblyFunctionInfo>();
  if (needsSP(MF) && MFI.isFrameBaseVirtual()) {
    unsigned LocalNum = MFI.getFrameBaseLocal();
    Loc.Location.WasmLoc = {WebAssembly::TI_LOCAL, LocalNum};
  } else {
    // TODO: This should work on a breakpoint at a function with no frame,
    // but probably won't work for traversing up the stack.
    Loc.Location.WasmLoc = {WebAssembly::TI_GLOBAL_RELOC, 0};
  }
  return Loc;
}
