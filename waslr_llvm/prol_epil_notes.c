void WebAssemblyFrameLowering::emitPrologue(MachineFunction &MF,
                                            MachineBasicBlock &MBB) const {
  // TODO: Do ".setMIFlag(MachineInstr::FrameSetup)" on emitted instructions
  auto &MFI = MF.getFrameInfo();
  assert(MFI.getCalleeSavedInfo().empty() &&
         "WebAssembly should not have callee-saved registers");

  if (!needsSP(MF))
    return;
  uint64_t StackSize = MFI.getStackSize();

  auto &ST = MF.getSubtarget<WebAssemblySubtarget>();
  const auto *TII = ST.getInstrInfo();
  auto &MRI = MF.getRegInfo();

  auto InsertPt = MBB.begin();
  while (InsertPt != MBB.end() &&
         WebAssembly::isArgument(InsertPt->getOpcode()))
    ++InsertPt;
  DebugLoc DL;

  const TargetRegisterClass *PtrRC =
      MRI.getTargetRegisterInfo()->getPointerRegClass(MF);
  unsigned SPReg = getSPReg(MF);
  if (StackSize)
    SPReg = MRI.createVirtualRegister(PtrRC);

  // BuildMI ( BasicBlock, InsertionPoint, DebugLoc, Instruction OpCode, Destination Register / Result Register)
  // addExternalSymbol: Specifies Operands of the instruction 
  //
  // This can be confusing when compared to the resulting .wat, but for example this wat:
  // global.get $__stack_pointer 
  // i32.const 32
  // i32.sub 
  //
  // is semantically equivalent to
  // SPReg = __stack_pointer;
  // SPReg = SPReg - 32;
  
  // get __stack_pointer
  const char *ES = "__stack_pointer";
  auto *SPSymbol = MF.createExternalSymbolName(ES);
  // SPReg = GLOBAL_GET __stack_pointer 
  BuildMI(MBB, InsertPt, DL, TII->get(getOpcGlobGet(MF)), SPReg)
      .addExternalSymbol(SPSymbol);
	  
  // functions rarely seem to require a Base pointer in Wasm 
  bool HasBP = hasBP(MF);
  if (HasBP) {
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
  
  // ? 
  if (HasBP) {
    Register BitmaskReg = MRI.createVirtualRegister(PtrRC);
    Align Alignment = MFI.getMaxAlign();
    BuildMI(MBB, InsertPt, DL, TII->get(getOpcConst(MF)), BitmaskReg)
        .addImm((int64_t) ~(Alignment.value() - 1));
    BuildMI(MBB, InsertPt, DL, TII->get(getOpcAnd(MF)), getSPReg(MF))
        .addReg(getSPReg(MF))
        .addReg(BitmaskReg);
  }
  
  // if function has Frame Pointer (?) ... ?
  if (hasFP(MF)) {
    // Unlike most conventional targets (where FP points to the saved FP),
    // FP points to the bottom of the fixed-size locals, so we can use positive
    // offsets in load/store instructions.
	
	// FPReg = SPReg 
    BuildMI(MBB, InsertPt, DL, TII->get(WebAssembly::COPY), getFPReg(MF))
        .addReg(getSPReg(MF));
  }
  
  // if necessary, update __stack_pointer (otherwise its just stored in a local)
  if (StackSize && needsSPWriteback(MF)) {
	// __stack_pointer = SPReg 
    writeSPToGlobal(getSPReg(MF), MF, MBB, InsertPt, DL);
  }
}



void WebAssemblyFrameLowering::emitEpilogue(MachineFunction &MF,
                                            MachineBasicBlock &MBB) const {
  uint64_t StackSize = MF.getFrameInfo().getStackSize();
  if (!needsSP(MF) || !needsSPWriteback(MF))
    return;
  auto &ST = MF.getSubtarget<WebAssemblySubtarget>();
  const auto *TII = ST.getInstrInfo();
  auto &MRI = MF.getRegInfo();
  auto InsertPt = MBB.getFirstTerminator();
  DebugLoc DL;

  if (InsertPt != MBB.end())
    DL = InsertPt->getDebugLoc();

  // Restore the stack pointer. If we had fixed-size locals, add the offset
  // subtracted in the prolog.
  unsigned SPReg = 0;
  unsigned SPFPReg = hasFP(MF) ? getFPReg(MF) : getSPReg(MF);
  if (hasBP(MF)) {
    auto FI = MF.getInfo<WebAssemblyFunctionInfo>();
    SPReg = FI->getBasePointerVreg();
  } else if (StackSize) {
    const TargetRegisterClass *PtrRC =
        MRI.getTargetRegisterInfo()->getPointerRegClass(MF);
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