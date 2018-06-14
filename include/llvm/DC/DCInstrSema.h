//===-- llvm/DC/DCInstrSema.h - DC Instruction Semantics --------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines DCInstrSema, the main interface that can be used to
// translate machine code (represented by an MCModule, an MC-level CFG) to IR.
//
// DCInstrSema provides various methods - some provided by a Target-specific
// subclassing implementation - that translate MC-level constructs into
// corresponding IR, at several granularities: functions inside a module, basic
// blocks, instructions, and finally instruction operands.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_DC_DCINSTRSEMA_H
#define LLVM_DC_DCINSTRSEMA_H

#include "llvm/DC/DCOpcodes.h"
#include "llvm/DC/DCRegisterSema.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/CodeGen/ValueTypes.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/NoFolder.h"
#include "llvm/MC/MCAnalysis/MCFunction.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/Support/ErrorHandling.h"
#include <vector>

namespace llvm {
class MCContext;
class DCTranslatedInst;

class DCInstrSema {
public:
  virtual ~DCInstrSema();

  bool translateInst(const MCDecodedInst &DecodedInst,
                     DCTranslatedInst &TranslatedInst);

  void SwitchToModule(Module *TheModule);
  void SwitchToFunction(const MCFunction *MCFN);
  void SwitchToBasicBlock(const MCBasicBlock *MCBB);

  void FinalizeModule();
  Function *FinalizeFunction();
  void FinalizeBasicBlock();

  BasicBlock *getOrCreateBasicBlock(uint64_t StartAddress);

  Function *getOrCreateMainFunction(Function *EntryFn);
  Function *getOrCreateInitRegSetFunction();
  Function *getOrCreateFiniRegSetFunction();

  void createExternalWrapperFunction(uint64_t Addr, StringRef Name);
  void createExternalTailCallBB(uint64_t Addr);

        DCRegisterSema &getDRS()       { return DRS; }
  const DCRegisterSema &getDRS() const { return DRS; }

  // Set the callback used for dynamically translating indirect branches/calls.
  // \p A pointer to a function taking an indirect target, and returning an
  // executable translated address.  Used like:
  //   %translated_pc = void(%regset*)* %FnPtr(i8* %new_pc)
  //   call %translated_pc(%regset* %regset_ptr)
  void setDynTranslateAtCallback(void *FnPtr) { DynTranslateAtCBPtr = FnPtr; }

private:
  // Autogenerated by tblgen
  const unsigned *OpcodeToSemaIdx;
  const unsigned *SemanticsArray;
  const uint64_t *ConstantArray;

protected:
  DCInstrSema(const unsigned *OpcodeToSemaIdx, const unsigned *SemanticsArray,
              const uint64_t *ConstantArray, DCRegisterSema &DRS);

  // Following members are always valid.
  void *DynTranslateAtCBPtr;

  // Following members are valid only inside a Module.
  LLVMContext *Ctx;
  Module *TheModule;
  DCRegisterSema &DRS;
  FunctionType *FuncType;

  // Following members are valid only inside a Function
  Function *TheFunction;
  const MCFunction *TheMCFunction;
  std::map<uint64_t, BasicBlock *> BBByAddr;
  BasicBlock *ExitBB;
  std::vector<BasicBlock *> CallBBs;

  // Following members are valid only inside a Basic Block
  BasicBlock *TheBB;
  const MCBasicBlock *TheMCBB;
  typedef IRBuilder<true, NoFolder> DCIRBuilder;
  std::unique_ptr<DCIRBuilder> Builder;

  // translation vars.
  unsigned Idx;
  EVT ResEVT;
  unsigned Opcode;
  SmallVector<Value *, 16> Vals;
  const MCDecodedInst *CurrentInst;
  DCTranslatedInst *CurrentTInst;

  unsigned Next() { return SemanticsArray[Idx++]; }
  EVT NextVT() { return EVT(MVT::SimpleValueType(Next())); }

  Value *getNextOperand() {
    unsigned OpIdx = Next();
    assert(OpIdx < Vals.size() && "Trying to access non-existent operand");
    return Vals[OpIdx];
  }

  void registerResult(Value *ResV) {
    Vals.push_back(ResV);
  }

  uint64_t getImmOp(unsigned Idx) {
    return CurrentInst->Inst.getOperand(Idx).getImm();
  }
  unsigned getRegOp(unsigned Idx) {
    return CurrentInst->Inst.getOperand(Idx).getReg();
  }

  Value *getReg(unsigned RegNo) { return DRS.getReg(RegNo); }
  void setReg(unsigned RegNo, Value *Val) { DRS.setReg(RegNo, Val); }

  Function *getFunction(uint64_t Addr);

  void insertCall(Value *CallTarget);
  Value *insertTranslateAt(Value *OrigTarget);

  void translateOpcode(unsigned Opcode);

  virtual void translateTargetOpcode() = 0;
  virtual void translateCustomOperand(unsigned OperandType,
                                      unsigned MIOperandNo) = 0;
  virtual void translateImplicit(unsigned RegNo) = 0;

  virtual void translateTargetIntrinsic(unsigned IntrinsicID) = 0;

  // Try to do a custom translation of a full instruction.
  // Called before translating an instruction.
  // Return true if the translation shouldn't proceed.
  virtual bool translateTargetInst() { return false; }

  uint64_t getBasicBlockStartAddress() const;
  uint64_t getBasicBlockEndAddress() const;

private:
  void translateOperand(unsigned OperandType, unsigned MIOperandNo);

  void translateBinOp(Instruction::BinaryOps Opc);
  void translateCastOp(Instruction::CastOps Opc);

  BasicBlock *insertCallBB(Value *CallTarget);

  void prepareBasicBlockForInsertion(BasicBlock *BB);

  void SwitchToBasicBlock(uint64_t BeginAddr);
};

DCInstrSema *createDCInstrSema(StringRef Triple, const MCRegisterInfo &MRI,
                               const MCInstrInfo &MII);

} // end namespace llvm

#endif
