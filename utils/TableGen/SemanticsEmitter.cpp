//===- SemanticsEmitter.cpp - Generate a Instruction Set Desc. ------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This tablegen backend is responsible for emitting a description of the
// instruction-level semantics of the target instruction set.
//
//===----------------------------------------------------------------------===//

#include "CodeGenDAGPatterns.h"
#include "CodeGenTarget.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Signals.h"
#include "llvm/TableGen/Record.h"
#include "llvm/TableGen/TableGenBackend.h"
#include <algorithm>

using namespace llvm;

namespace {

/// The target we're generating semantics for: keeps around some useful
/// references to the parsed CodeGen target description, and some generation
/// variables.
class SemanticsTarget {
public:
  RecordKeeper &Records;
  CodeGenDAGPatterns CGPatterns;
  CodeGenTarget &CGTarget;

  /// Keep track of the equivalence between target-specific SDNodes and
  /// their target-independent equivalent, as described in definitions
  /// derived of the SDNodeEquiv class.
  typedef std::map<Record *, Record *> SDNodeEquivMap;
  SDNodeEquivMap SDNodeEquiv;

  /// Unique constant integers, and keep track of their index in a table.
  /// This is done so that the generated semantics table is an unsigned[],
  /// with uint64_t constants only being an unsigned index to this table.
  typedef std::map<uint64_t, unsigned> ConstantIdxMap;
  ConstantIdxMap ConstantIdx;
  unsigned CurConstIdx;

  SemanticsTarget(RecordKeeper &Records);
};

/// The semantics of a single SDNode, as an operation, taking operands and
/// producing typed results.
class NodeSemantics {
public:
  /// The opcode for this operation: either an ISD (for SDNodes), or DCINS
  /// (for other operations, like manipulating operands, registers, etc..)
  /// opcode.
  std::string Opcode;

  /// The types of each result generated by this operation.
  std::vector<MVT::SimpleValueType> Types;

  /// All the operands of this instruction.
  std::vector<std::string> Operands;

  void addOperand(StringRef Op) { Operands.push_back(Op); }
};

class InstSemantics {
public:
  std::vector<NodeSemantics> Semantics;

  /// Construct semantics for the pattern \p TP, for instruction \p CGI.
  InstSemantics(SemanticsTarget &Target, const CodeGenInstruction &CGI,
                const TreePattern &TP);
  /// Default constructor for empty semantics.
  InstSemantics();
};

/// The core of the Pattern->Semantics translation, the "flattener".
class Flattener {
public:
  Flattener(SemanticsTarget &Target, const CodeGenInstruction &CGI,
            InstSemantics &I)
      : Target(Target), CGI(CGI), I(I), CurDefNo(0) {}

private:
  SemanticsTarget &Target;
  const CodeGenInstruction &CGI;
  InstSemantics &I;

  typedef SmallPtrSet<Record *, 1> ImplicitRegSet;
  ImplicitRegSet EliminatedImplicitRegs;

  typedef StringMap<unsigned> NameToOperandMap;
  NameToOperandMap OperandByName;

  unsigned CurDefNo;

  /// Get the CodeGenInstruction OperandInfo for \p Name.
  const CGIOperandList::OperandInfo *getNamedOperand(StringRef Name) {
    if (Name.empty())
      return 0;
    // FIXME: This is the slow, stupid, simple way.
    for (unsigned OI = 0, OE = CGI.Operands.size(); OI != OE; ++OI) {
      if (Name == CGI.Operands[OI].Name)
        return &CGI.Operands[OI];
    }
    return 0;
  }

  /// Set the types of \p NS to what was inferred for \p TPN, or MVT::isVoid if
  /// the node has no result.
  void setNSTypeFromNode(NodeSemantics &NS, const TreePatternNode *TPN) {
    if (TPN->getNumTypes()) {
      for (unsigned i = 0, e = TPN->getNumTypes(); i != e; ++i)
        NS.Types.push_back(TPN->getExtType(i).getConcrete());
    } else {
      NS.Types.push_back(MVT::isVoid);
    }
  }

  /// Add the operation \p NS to the instruction semantics, keeping track of
  /// the defined values.
  void addSemantics(const NodeSemantics &NS) {
    for (unsigned i = 0, e = NS.Types.size(); i != e; ++i) {
      if (NS.Types[i] != MVT::isVoid)
        ++CurDefNo;
    }
    I.Semantics.push_back(NS);
  }

  /// Add the results of \p Prev as operands to \p NS.
  void addResOperand(NodeSemantics &NS, const NodeSemantics &Prev) {
    unsigned FirstDefNo = CurDefNo - (Prev.Types.size() - 1);
    for (unsigned i = 0, e = Prev.Types.size(); i != e; ++i) {
      if (Prev.Types[i] != MVT::isVoid)
        NS.addOperand(utostr(FirstDefNo + i));
    }
    addSemantics(Prev);
  }

  /// Make node semantics from an Operand patern:
  /// - if \p TPN is a RegisterClass or a RegisterOperand, generate:
  ///     DCINS::GET_RC, <inferred type>, <MIOperandNo of the Operand>
  /// - if \p TPN is an Operand, generate:
  ///     DCINS::CUSTOM_OP, <inferred types>,
  ///       <the Operand type as a Target::OpTypes:: enum value>,
  ///       <MIOperandNo of the first MI operand for this Operand>
  /// - if \p TPN is an OPERAND_IMMEDIATE Operand, generate:
  ///     DCINS::CONSTANT_OP, <inferred type>, <MIOperandNo of the Operand>
  ///
  /// Also, add the values defined by this node as operands to \p Parent.
  ///
  void flattenOperand(const TreePatternNode *TPN, NodeSemantics *Parent,
                      const CGIOperandList::OperandInfo *OpInfo) {
    Record *OpRec = OpInfo->Rec;
    NodeSemantics Op;
    setNSTypeFromNode(Op, TPN);

    // RegisterOperands are the same thing as RegisterClasses.
    if (OpRec->isSubClassOf("RegisterOperand"))
      OpRec = OpRec->getValueAsDef("RegClass");

    if (OpRec->isSubClassOf("Operand")) {
      if (OpInfo->OperandType == "OPERAND_IMMEDIATE") {
        Op.Opcode = "DCINS::CONSTANT_OP";
      } else {
        Op.Opcode = "DCINS::CUSTOM_OP";
        Op.addOperand(CGI.Namespace + "::OpTypes::" + OpRec->getName());
        NameToOperandMap::iterator It = OperandByName.find(OpInfo->Name);
        if (It == OperandByName.end()) {
          OperandByName[OpInfo->Name] = CurDefNo;
        } else {
          // If we already found it, no need to generate the operation again.
          Parent->addOperand(utostr(It->getValue()));
          return;
        }
      }
    } else if (OpRec->isSubClassOf("RegisterClass")) {
      Op.Opcode = "DCINS::GET_RC";
    } else {
      llvm_unreachable("Unknown operand type");
    }
    Op.addOperand(utostr(OpInfo->MIOperandNo));
    addResOperand(*Parent, Op);
  }

  /// Make node semantics from a leaf pattern:
  /// - if \p TPN is an explicit Register, generate:
  ///     DCINS::GET_REG, <inferred type>, Target::RegName
  /// - if \p TPN is a compile-time constant, generate:
  ///     DCINS::MOV_CONSTANT, <inferred type>, <Constant index>
  ///   The constant index points in an uint64_t array, where all compile-time
  ///   constants are uniqued (so that the semantics array remains unsigned[].)
  ///
  void flattenLeaf(const TreePatternNode *TPN, NodeSemantics *Parent) {
    DefInit *OpDef = dyn_cast<DefInit>(TPN->getLeafValue());

    if (OpDef == 0) {
      IntInit *OpInt = cast<IntInit>(TPN->getLeafValue());
      NodeSemantics Mov;
      setNSTypeFromNode(Mov, TPN);
      Mov.Opcode = "DCINS::MOV_CONSTANT";
      unsigned &Idx = Target.ConstantIdx[OpInt->getValue()];
      if (Idx == 0)
        Idx = Target.CurConstIdx++;
      Mov.addOperand(utostr(Idx));
      addResOperand(*Parent, Mov);
      return;
    }

    Record *OpRec = OpDef->getDef();
    NodeSemantics Op;
    setNSTypeFromNode(Op, TPN);

    if (OpRec->isSubClassOf("Register")) {
      Op.Opcode = "DCINS::GET_REG";
      Op.addOperand(CGI.Namespace + "::" + OpRec->getName());
    } else {
      llvm_unreachable("Unknown operand type");
    }
    addResOperand(*Parent, Op);
  }

  /// Make an operation for "implicit" nodes, generate:
  ///   DCINS::IMPLICIT, MVT::isVoid, <imp-def'd Target::Register>
  ///
  void flattenImplicit(const TreePatternNode *TPN, NodeSemantics &NS) {
    NS.Opcode = "DCINS::IMPLICIT";
    for (unsigned i = 0, e = TPN->getNumChildren(); i != e; ++i)
      NS.addOperand(CGI.Namespace + "::" +
                    TPN->getChild(i)->getLeafValue()->getAsString());
  }

  /// Make node semantics for "set" nodes. For all defined values to be set:
  /// - if the destination is a RegisterClass/RegisterOperand, generate:
  ///     DCINS::PUT_RC, <inferred type>, <MIOperandNo of the reg>, <the value>
  /// - if the destination is an explicit Register, generate:
  ///     DCINS::PUT_REG, <inferred type>, <Target::Register name>, <the value>
  ///
  /// Keep track of the results that were dropped from the SDNode child because
  /// of SDNodeEquiv definitions.
  ///
  void flattenSet(const TreePatternNode *TPN) {
    unsigned NumNodeDefs = TPN->getNumChildren() - 1;
    const TreePatternNode *LastChild = TPN->getChild(TPN->getNumChildren() - 1);

      if (NumNodeDefs != LastChild->getNumTypes()) {
          errs() << "\nFlatten\n";
          errs() << NumNodeDefs << " ";
          TPN->dump();
          errs() << "\n";
          errs() << LastChild->getNumTypes() << " ";
          LastChild->dump();
          errs() << "\n";
//          for (unsigned i = 0; i < LastChild->getNumChildren(); ++i) {
//              errs() << "Num Types: " << LastChild->getChild(i)->getNumTypes() << "\n";
//              errs() << "Type: " << LastChild->getChild(i)->getType(0) << "\n";
//          }
//          errs() << LastChild->getNumChildren() << "\n";
//          errs() << "\n" <<LastChild->getChild(2)->getNumTypes() << "\n";
//          errs() << LastChild->getType(0); TPN->getChild(1)->dump();
          return;
      }
    assert(NumNodeDefs == LastChild->getNumTypes() &&
           "Invalid 'set': last child needs to define all the others.");

    // NS will be thrown away, we just use the def indices of the last child.
    NodeSemantics DummyNS;

    flatten(LastChild, &DummyNS);
    // We count what the child defined, because when replacing equivalent
    // SDNodes, it doesn't define all the children.
    unsigned NumDefs = DummyNS.Operands.size();
    unsigned FirstDefNo = CurDefNo - NumDefs;

    for (unsigned i = 0, e = NumDefs; i != e; ++i) {
      const TreePatternNode *Child = TPN->getChild(i);
      Record *OpRec = cast<DefInit>(TPN->getChild(i)->getLeafValue())->getDef();

      NodeSemantics NS;
      NS.Types.push_back(MVT::isVoid);

      // RegisterOperands are the same thing as RegisterClasses.
      if (OpRec->isSubClassOf("RegisterOperand"))
        OpRec = OpRec->getValueAsDef("RegClass");

      if (OpRec->isSubClassOf("RegisterClass")) {
        const CGIOperandList::OperandInfo *OpInfo =
            getNamedOperand(Child->getName());
        assert(OpInfo && "'set' output operand not found in instruction?");
        NS.Opcode = "DCINS::PUT_RC";
        NS.Operands.push_back(utostr(OpInfo->MIOperandNo));
      } else if (OpRec->isSubClassOf("Register")) {
        NS.Opcode = "DCINS::PUT_REG";
        NS.Operands.push_back(CGI.Namespace + "::" + OpRec->getName());
      }
      NS.Operands.push_back(utostr(FirstDefNo + i));
      addSemantics(NS);
    }

    // Keep track of the registers removed from the target-specific SDNode.
    for (unsigned i = NumDefs, e = NumNodeDefs; i != e; ++i) {
      assert(TPN->getChild(i)->isLeaf() &&
             "Invalid SDNode equivalence: dropped non-leaf node!");
      Record *OpRec = cast<DefInit>(TPN->getChild(i)->getLeafValue())->getDef();
      assert(OpRec->isSubClassOf("Register") &&
             "Dropped SDNode result isn't an imp-def'd register.");
      EliminatedImplicitRegs.insert(OpRec);
    }
  }

  /// Make node semantics from SDNodes, generate:
  ///   <SDNode opcode>, <inferred types>, <value indices for the operands>
  ///
  /// Also, try to make an equivalence between the SDNode's operator and
  /// one with less results, as defined by the SDNodeEquiv tablegen definitions.
  ///
  void flattenSDNode(const TreePatternNode *TPN, NodeSemantics &NS) {
    Record *Operator = TPN->getOperator();
    SemanticsTarget::SDNodeEquivMap::const_iterator It =
        Target.SDNodeEquiv.find(Operator);
    NS.Opcode = Operator->getValueAsString("Opcode");
    if (It != Target.SDNodeEquiv.end()) {
      Record *EquivNode = It->second;
      const SDNodeInfo &SDNI = Target.CGPatterns.getSDNodeInfo(EquivNode);
      NS.Opcode = SDNI.getEnumName();
      for (unsigned i = 0, e = TPN->getNumTypes() - SDNI.getNumResults();
           i != e; ++i)
        NS.Types.pop_back();
    }
    for (unsigned i = 0, e = TPN->getNumChildren(); i != e; ++i)
      flatten(TPN->getChild(i), &NS);
  }

  /// Make node semantics for the whole tree \p TPN.
  void flatten(const TreePatternNode *TPN, NodeSemantics *Parent) {
    if (const CGIOperandList::OperandInfo *OpInfo =
            getNamedOperand(TPN->getName())) {
      flattenOperand(TPN, Parent, OpInfo);
      return;
    } else if (TPN->isLeaf()) {
      flattenLeaf(TPN, Parent);
      return;
    }
    Record *Operator = TPN->getOperator();
    NodeSemantics NS;

    setNSTypeFromNode(NS, TPN);

    if (Operator->getName() == "set") {
      assert(Parent == 0 && "A 'set' node wasn't at the top-level?");
      flattenSet(TPN);
      return;
    }

    if (Operator->getName() == "implicit") {
      assert(Parent == 0 && "An 'implicit' node wasn't at the top-level?");
      flattenImplicit(TPN, NS);
    } else if (Operator->isSubClassOf("SDNode")) {
      flattenSDNode(TPN, NS);
    } else {
      llvm_unreachable("Unable to handle operator.");
    }
    if (Parent)
      addResOperand(*Parent, NS);
    else
      addSemantics(NS);
  }

public:
  void flatten(const TreePatternNode *TPN) {
    flatten(TPN, 0);

    // For all the implicit register definitions we dropped because of
    // SDNode equivalences, add an 'implicit' node.
    NodeSemantics NS;
    NS.Opcode = "DCINS::IMPLICIT";
    NS.Types.push_back(MVT::isVoid);
    for (ImplicitRegSet::const_iterator IRI = EliminatedImplicitRegs.begin(),
                                        IRE = EliminatedImplicitRegs.end();
         IRI != IRE; ++IRI) {
      NS.Operands.clear();
      NS.addOperand(CGI.Namespace + "::" + (*IRI)->getName());
      I.Semantics.push_back(NS);
    }
  }
};

class SemanticsEmitter {
  typedef std::vector<InstSemantics> InstSemaList;

  // List mapping Instruction enum values to indices:
  // - first, index of the semantics in InstSemas
  // - replaced by the start offset in the generated array
  typedef std::vector<unsigned> InstToIdxMap;

  InstSemaList InstSemas;

  InstToIdxMap InstIdx;
  unsigned CurSemaOffset;

  void addInstSemantics(unsigned InstEnumValue, const InstSemantics &Sema);

public:
  SemanticsEmitter(RecordKeeper &Records);

  // run - Output the semantics.
  void run(raw_ostream &OS);

  void ParseSemantics();

  RecordKeeper &Records;
  SemanticsTarget SemaTarget;
  CodeGenDAGPatterns &CGPatterns;
  CodeGenTarget &Target;
};

SemanticsEmitter::SemanticsEmitter(RecordKeeper &Records)
    : InstSemas(), InstIdx(), CurSemaOffset(0), Records(Records),
      SemaTarget(Records), CGPatterns(SemaTarget.CGPatterns),
      Target(SemaTarget.CGTarget) {

  const std::vector<const CodeGenInstruction *> &CGIByEnum =
      Target.getInstructionsByEnumValue();
  InstIdx.resize(CGIByEnum.size());

  // Add dummy semantics.
  addInstSemantics(0, InstSemantics());

  // First, look for Semantics instances.
  ParseSemantics();

  // For the rest, try to use the patterns that are in Instruction instances.
  for (unsigned i = 0, e = CGIByEnum.size(); i != e; ++i) {
    const CodeGenInstruction &CGI = *CGIByEnum[i];
    Record *TheDef = CGI.TheDef;
    const DAGInstruction &DI = CGPatterns.getInstruction(TheDef);
    if (InstIdx[i])
      continue;
    if (DI.getPattern() && !CGI.isCodeGenOnly) {
      addInstSemantics(i, InstSemantics(SemaTarget, CGI, *DI.getPattern()));
    }
  }
}

void SemanticsEmitter::addInstSemantics(unsigned InstEnumValue,
                                        const InstSemantics &Sema) {
  InstIdx[InstEnumValue] = InstSemas.size();
  InstSemas.push_back(Sema);
}

void SemanticsEmitter::ParseSemantics() {
  std::vector<Record *> Instrs = Records.getAllDerivedDefinitions("Semantics");
  const std::vector<const CodeGenInstruction *> &CGIByEnum =
      Target.getInstructionsByEnumValue();

  std::map<Record *, DAGInstruction, LessRecordByID> DAGInsts;
  for (unsigned i = 0, e = Instrs.size(); i != e; ++i) {
    ListInit *LI = 0;

    if (isa<ListInit>(Instrs[i]->getValueInit("Pattern")))
      LI = Instrs[i]->getValueAsListInit("Pattern");

    Record *InstDef = Instrs[i]->getValueAsDef("Inst");

    CodeGenInstruction &CGI = Target.getInstruction(InstDef);
    const DAGInstruction &TheInst = CGPatterns.parseInstructionPattern(
        CGI, LI, DAGInsts, /*CanUseOutputOps=*/true);

    // FIXME: Instead of looking for the instruction *every* time, what about:
    // - iterating on InstructionsByEnumValue, and mapping CGI->Semantics before
    // - adding EnumValue to CGI

    std::vector<const CodeGenInstruction *>::const_iterator It =
        std::find(CGIByEnum.begin(), CGIByEnum.end(), &CGI);
    assert(It != CGIByEnum.end() && *It == &CGI);

    addInstSemantics(std::distance(CGIByEnum.begin(), It),
                     InstSemantics(SemaTarget, CGI, *TheInst.getPattern()));
  }
}

void SemanticsEmitter::run(raw_ostream &OS) {
  emitSourceFileHeader("Target Instruction Semantics", OS);

  StringRef TGName = Target.getName();
  const std::vector<const CodeGenInstruction *> &CGIByEnum =
      Target.getInstructionsByEnumValue();
  assert(CGIByEnum.size() == InstIdx.size());

  OS << "namespace llvm {\n";

  OS << "namespace " << TGName << " {\n";
  OS << "namespace {\n\n";

  OS << "const unsigned InstSemantics[] = {\n";
  OS << "  DCINS::END_OF_INSTRUCTION,\n";
  CurSemaOffset = 1;
  for (unsigned I = 0, E = InstIdx.size(); I != E; ++I) {
    if (InstIdx[I] == 0)
      continue;
    InstSemantics &Sema = InstSemas[InstIdx[I]];
    InstIdx[I] = CurSemaOffset++;
    OS << "  // " << CGIByEnum[I]->TheDef->getName() << "\n";
    for (std::vector<NodeSemantics>::const_iterator SI = Sema.Semantics.begin(),
                                                    SE = Sema.Semantics.end();
         SI != SE; ++SI) {
      ++CurSemaOffset;
      OS.indent(2) << SI->Opcode;
      for (unsigned ti = 0, te = SI->Types.size(); ti != te; ++ti)
        OS << ", " << llvm::getEnumName(SI->Types[ti]);
      CurSemaOffset += SI->Types.size();
      for (unsigned oi = 0, oe = SI->Operands.size(); oi != oe; ++oi)
        OS << ", " << SI->Operands[oi];
      CurSemaOffset += SI->Operands.size();
      OS << ",\n";
    }
    OS << "  DCINS::END_OF_INSTRUCTION,\n";
  }
  OS << "};\n\n";

  OS << "const unsigned OpcodeToSemaIdx[] = {\n";
  for (unsigned I = 0, E = InstIdx.size(); I != E; ++I)
    OS << InstIdx[I] << ", \t// " << CGIByEnum[I]->TheDef->getName() << "\n";
  OS << "};\n\n";

  std::vector<uint64_t> Constants(SemaTarget.ConstantIdx.size() + 1);
  for (SemanticsTarget::ConstantIdxMap::const_iterator
           CI = SemaTarget.ConstantIdx.begin(),
           CE = SemaTarget.ConstantIdx.end();
       CI != CE; ++CI)
    Constants[CI->second] = CI->first;
  OS << "const uint64_t ConstantArray[] = {\n";
  for (unsigned I = 0, E = Constants.size(); I != E; ++I) {
    OS.indent(2) << Constants[I] << "U,\n";
  }
  OS << "};\n\n";

  OS << "\n} // end anonymous namespace\n";
  OS << "} // end namespace " << TGName << "\n";
  OS << "} // end namespace llvm\n";
}

} // end anonymous namespace

SemanticsTarget::SemanticsTarget(RecordKeeper &Records)
    : Records(Records), CGPatterns(Records),
      CGTarget(CGPatterns.getTargetInfo()), SDNodeEquiv(), ConstantIdx(),
      CurConstIdx(1) {
  std::vector<Record *> Equivs =
      Records.getAllDerivedDefinitions("SDNodeEquiv");
  for (unsigned i = 0, e = Equivs.size(); i != e; ++i) {
    SDNodeEquiv[Equivs[i]->getValueAsDef("TargetSpecific")] =
        Equivs[i]->getValueAsDef("TargetIndependent");
  }
}

InstSemantics::InstSemantics(SemanticsTarget &Target,
                             const CodeGenInstruction &CGI,
                             const TreePattern &TP) {
  Flattener Flat(Target, CGI, *this);
  for (unsigned i = 0, e = TP.getNumTrees(); i != e; ++i)
    Flat.flatten(TP.getTree(i));
}

InstSemantics::InstSemantics() {
  NodeSemantics NS;
  NS.Opcode = "DCINS::END_OF_INSTRUCTION";
  Semantics.push_back(NS);
}

namespace llvm {

bool EmitSemantics(RecordKeeper &Records, raw_ostream &OS) {
  SemanticsEmitter(Records).run(OS);
  return false;
}

} // end namespace llvm
