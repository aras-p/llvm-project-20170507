//===-- MipsSEISelLowering.cpp - MipsSE DAG Lowering Interface --*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Subclass of MipsTargetLowering specialized for mips32/64.
//
//===----------------------------------------------------------------------===//
#include "MipsSEISelLowering.h"
#include "MipsRegisterInfo.h"
#include "MipsTargetMachine.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Target/TargetInstrInfo.h"

using namespace llvm;

static cl::opt<bool>
EnableMipsTailCalls("enable-mips-tail-calls", cl::Hidden,
                    cl::desc("MIPS: Enable tail calls."), cl::init(false));

static cl::opt<bool> NoDPLoadStore("mno-ldc1-sdc1", cl::init(false),
                                   cl::desc("Expand double precision loads and "
                                            "stores to their single precision "
                                            "counterparts"));

MipsSETargetLowering::MipsSETargetLowering(MipsTargetMachine &TM)
  : MipsTargetLowering(TM) {
  // Set up the register classes

  clearRegisterClasses();

  addRegisterClass(MVT::i32, &Mips::GPR32RegClass);

  if (HasMips64)
    addRegisterClass(MVT::i64, &Mips::GPR64RegClass);

  if (Subtarget->hasDSP() || Subtarget->hasMSA()) {
    // Expand all truncating stores and extending loads.
    unsigned FirstVT = (unsigned)MVT::FIRST_VECTOR_VALUETYPE;
    unsigned LastVT = (unsigned)MVT::LAST_VECTOR_VALUETYPE;

    for (unsigned VT0 = FirstVT; VT0 <= LastVT; ++VT0) {
      for (unsigned VT1 = FirstVT; VT1 <= LastVT; ++VT1)
        setTruncStoreAction((MVT::SimpleValueType)VT0,
                            (MVT::SimpleValueType)VT1, Expand);

      setLoadExtAction(ISD::SEXTLOAD, (MVT::SimpleValueType)VT0, Expand);
      setLoadExtAction(ISD::ZEXTLOAD, (MVT::SimpleValueType)VT0, Expand);
      setLoadExtAction(ISD::EXTLOAD, (MVT::SimpleValueType)VT0, Expand);
    }
  }

  if (Subtarget->hasDSP()) {
    MVT::SimpleValueType VecTys[2] = {MVT::v2i16, MVT::v4i8};

    for (unsigned i = 0; i < array_lengthof(VecTys); ++i) {
      addRegisterClass(VecTys[i], &Mips::DSPRRegClass);

      // Expand all builtin opcodes.
      for (unsigned Opc = 0; Opc < ISD::BUILTIN_OP_END; ++Opc)
        setOperationAction(Opc, VecTys[i], Expand);

      setOperationAction(ISD::ADD, VecTys[i], Legal);
      setOperationAction(ISD::SUB, VecTys[i], Legal);
      setOperationAction(ISD::LOAD, VecTys[i], Legal);
      setOperationAction(ISD::STORE, VecTys[i], Legal);
      setOperationAction(ISD::BITCAST, VecTys[i], Legal);
    }

    setTargetDAGCombine(ISD::SHL);
    setTargetDAGCombine(ISD::SRA);
    setTargetDAGCombine(ISD::SRL);
    setTargetDAGCombine(ISD::SETCC);
    setTargetDAGCombine(ISD::VSELECT);
  }

  if (Subtarget->hasDSPR2())
    setOperationAction(ISD::MUL, MVT::v2i16, Legal);

  if (Subtarget->hasMSA()) {
    addMSAIntType(MVT::v16i8, &Mips::MSA128BRegClass);
    addMSAIntType(MVT::v8i16, &Mips::MSA128HRegClass);
    addMSAIntType(MVT::v4i32, &Mips::MSA128WRegClass);
    addMSAIntType(MVT::v2i64, &Mips::MSA128DRegClass);
    addMSAFloatType(MVT::v8f16, &Mips::MSA128HRegClass);
    addMSAFloatType(MVT::v4f32, &Mips::MSA128WRegClass);
    addMSAFloatType(MVT::v2f64, &Mips::MSA128DRegClass);

    setTargetDAGCombine(ISD::AND);
    setTargetDAGCombine(ISD::SRA);
    setTargetDAGCombine(ISD::VSELECT);
    setTargetDAGCombine(ISD::XOR);
  }

  if (!Subtarget->mipsSEUsesSoftFloat()) {
    addRegisterClass(MVT::f32, &Mips::FGR32RegClass);

    // When dealing with single precision only, use libcalls
    if (!Subtarget->isSingleFloat()) {
      if (Subtarget->isFP64bit())
        addRegisterClass(MVT::f64, &Mips::FGR64RegClass);
      else
        addRegisterClass(MVT::f64, &Mips::AFGR64RegClass);
    }
  }

  setOperationAction(ISD::SMUL_LOHI,          MVT::i32, Custom);
  setOperationAction(ISD::UMUL_LOHI,          MVT::i32, Custom);
  setOperationAction(ISD::MULHS,              MVT::i32, Custom);
  setOperationAction(ISD::MULHU,              MVT::i32, Custom);

  if (HasMips64) {
    setOperationAction(ISD::MULHS,            MVT::i64, Custom);
    setOperationAction(ISD::MULHU,            MVT::i64, Custom);
    setOperationAction(ISD::MUL,              MVT::i64, Custom);
  }

  setOperationAction(ISD::INTRINSIC_WO_CHAIN, MVT::i64, Custom);
  setOperationAction(ISD::INTRINSIC_W_CHAIN,  MVT::i64, Custom);

  setOperationAction(ISD::SDIVREM, MVT::i32, Custom);
  setOperationAction(ISD::UDIVREM, MVT::i32, Custom);
  setOperationAction(ISD::SDIVREM, MVT::i64, Custom);
  setOperationAction(ISD::UDIVREM, MVT::i64, Custom);
  setOperationAction(ISD::ATOMIC_FENCE,       MVT::Other, Custom);
  setOperationAction(ISD::LOAD,               MVT::i32, Custom);
  setOperationAction(ISD::STORE,              MVT::i32, Custom);

  setTargetDAGCombine(ISD::ADDE);
  setTargetDAGCombine(ISD::SUBE);
  setTargetDAGCombine(ISD::MUL);

  setOperationAction(ISD::INTRINSIC_WO_CHAIN, MVT::Other, Custom);
  setOperationAction(ISD::INTRINSIC_W_CHAIN, MVT::Other, Custom);
  setOperationAction(ISD::INTRINSIC_VOID, MVT::Other, Custom);

  if (NoDPLoadStore) {
    setOperationAction(ISD::LOAD, MVT::f64, Custom);
    setOperationAction(ISD::STORE, MVT::f64, Custom);
  }

  computeRegisterProperties();
}

const MipsTargetLowering *
llvm::createMipsSETargetLowering(MipsTargetMachine &TM) {
  return new MipsSETargetLowering(TM);
}

// Enable MSA support for the given integer type and Register class.
void MipsSETargetLowering::
addMSAIntType(MVT::SimpleValueType Ty, const TargetRegisterClass *RC) {
  addRegisterClass(Ty, RC);

  // Expand all builtin opcodes.
  for (unsigned Opc = 0; Opc < ISD::BUILTIN_OP_END; ++Opc)
    setOperationAction(Opc, Ty, Expand);

  setOperationAction(ISD::BITCAST, Ty, Legal);
  setOperationAction(ISD::LOAD, Ty, Legal);
  setOperationAction(ISD::STORE, Ty, Legal);
  setOperationAction(ISD::EXTRACT_VECTOR_ELT, Ty, Custom);
  setOperationAction(ISD::INSERT_VECTOR_ELT, Ty, Legal);
  setOperationAction(ISD::BUILD_VECTOR, Ty, Custom);

  setOperationAction(ISD::ADD, Ty, Legal);
  setOperationAction(ISD::AND, Ty, Legal);
  setOperationAction(ISD::CTLZ, Ty, Legal);
  setOperationAction(ISD::CTPOP, Ty, Legal);
  setOperationAction(ISD::MUL, Ty, Legal);
  setOperationAction(ISD::OR, Ty, Legal);
  setOperationAction(ISD::SDIV, Ty, Legal);
  setOperationAction(ISD::SHL, Ty, Legal);
  setOperationAction(ISD::SRA, Ty, Legal);
  setOperationAction(ISD::SRL, Ty, Legal);
  setOperationAction(ISD::SUB, Ty, Legal);
  setOperationAction(ISD::UDIV, Ty, Legal);
  setOperationAction(ISD::VECTOR_SHUFFLE, Ty, Custom);
  setOperationAction(ISD::VSELECT, Ty, Legal);
  setOperationAction(ISD::XOR, Ty, Legal);

  setOperationAction(ISD::SETCC, Ty, Legal);
  setCondCodeAction(ISD::SETNE, Ty, Expand);
  setCondCodeAction(ISD::SETGE, Ty, Expand);
  setCondCodeAction(ISD::SETGT, Ty, Expand);
  setCondCodeAction(ISD::SETUGE, Ty, Expand);
  setCondCodeAction(ISD::SETUGT, Ty, Expand);
}

// Enable MSA support for the given floating-point type and Register class.
void MipsSETargetLowering::
addMSAFloatType(MVT::SimpleValueType Ty, const TargetRegisterClass *RC) {
  addRegisterClass(Ty, RC);

  // Expand all builtin opcodes.
  for (unsigned Opc = 0; Opc < ISD::BUILTIN_OP_END; ++Opc)
    setOperationAction(Opc, Ty, Expand);

  setOperationAction(ISD::LOAD, Ty, Legal);
  setOperationAction(ISD::STORE, Ty, Legal);
  setOperationAction(ISD::BITCAST, Ty, Legal);
  setOperationAction(ISD::EXTRACT_VECTOR_ELT, Ty, Legal);

  if (Ty != MVT::v8f16) {
    setOperationAction(ISD::FABS,  Ty, Legal);
    setOperationAction(ISD::FADD,  Ty, Legal);
    setOperationAction(ISD::FDIV,  Ty, Legal);
    setOperationAction(ISD::FLOG2, Ty, Legal);
    setOperationAction(ISD::FMUL,  Ty, Legal);
    setOperationAction(ISD::FRINT, Ty, Legal);
    setOperationAction(ISD::FSQRT, Ty, Legal);
    setOperationAction(ISD::FSUB,  Ty, Legal);
    setOperationAction(ISD::VSELECT, Ty, Legal);

    setOperationAction(ISD::SETCC, Ty, Legal);
    setCondCodeAction(ISD::SETOGE, Ty, Expand);
    setCondCodeAction(ISD::SETOGT, Ty, Expand);
    setCondCodeAction(ISD::SETUGE, Ty, Expand);
    setCondCodeAction(ISD::SETUGT, Ty, Expand);
    setCondCodeAction(ISD::SETGE,  Ty, Expand);
    setCondCodeAction(ISD::SETGT,  Ty, Expand);
  }
}

bool
MipsSETargetLowering::allowsUnalignedMemoryAccesses(EVT VT, bool *Fast) const {
  MVT::SimpleValueType SVT = VT.getSimpleVT().SimpleTy;

  switch (SVT) {
  case MVT::i64:
  case MVT::i32:
    if (Fast)
      *Fast = true;
    return true;
  default:
    return false;
  }
}

SDValue MipsSETargetLowering::LowerOperation(SDValue Op,
                                             SelectionDAG &DAG) const {
  switch(Op.getOpcode()) {
  case ISD::LOAD:  return lowerLOAD(Op, DAG);
  case ISD::STORE: return lowerSTORE(Op, DAG);
  case ISD::SMUL_LOHI: return lowerMulDiv(Op, MipsISD::Mult, true, true, DAG);
  case ISD::UMUL_LOHI: return lowerMulDiv(Op, MipsISD::Multu, true, true, DAG);
  case ISD::MULHS:     return lowerMulDiv(Op, MipsISD::Mult, false, true, DAG);
  case ISD::MULHU:     return lowerMulDiv(Op, MipsISD::Multu, false, true, DAG);
  case ISD::MUL:       return lowerMulDiv(Op, MipsISD::Mult, true, false, DAG);
  case ISD::SDIVREM:   return lowerMulDiv(Op, MipsISD::DivRem, true, true, DAG);
  case ISD::UDIVREM:   return lowerMulDiv(Op, MipsISD::DivRemU, true, true,
                                          DAG);
  case ISD::INTRINSIC_WO_CHAIN: return lowerINTRINSIC_WO_CHAIN(Op, DAG);
  case ISD::INTRINSIC_W_CHAIN:  return lowerINTRINSIC_W_CHAIN(Op, DAG);
  case ISD::INTRINSIC_VOID:     return lowerINTRINSIC_VOID(Op, DAG);
  case ISD::EXTRACT_VECTOR_ELT: return lowerEXTRACT_VECTOR_ELT(Op, DAG);
  case ISD::BUILD_VECTOR:       return lowerBUILD_VECTOR(Op, DAG);
  case ISD::VECTOR_SHUFFLE:     return lowerVECTOR_SHUFFLE(Op, DAG);
  }

  return MipsTargetLowering::LowerOperation(Op, DAG);
}

// selectMADD -
// Transforms a subgraph in CurDAG if the following pattern is found:
//  (addc multLo, Lo0), (adde multHi, Hi0),
// where,
//  multHi/Lo: product of multiplication
//  Lo0: initial value of Lo register
//  Hi0: initial value of Hi register
// Return true if pattern matching was successful.
static bool selectMADD(SDNode *ADDENode, SelectionDAG *CurDAG) {
  // ADDENode's second operand must be a flag output of an ADDC node in order
  // for the matching to be successful.
  SDNode *ADDCNode = ADDENode->getOperand(2).getNode();

  if (ADDCNode->getOpcode() != ISD::ADDC)
    return false;

  SDValue MultHi = ADDENode->getOperand(0);
  SDValue MultLo = ADDCNode->getOperand(0);
  SDNode *MultNode = MultHi.getNode();
  unsigned MultOpc = MultHi.getOpcode();

  // MultHi and MultLo must be generated by the same node,
  if (MultLo.getNode() != MultNode)
    return false;

  // and it must be a multiplication.
  if (MultOpc != ISD::SMUL_LOHI && MultOpc != ISD::UMUL_LOHI)
    return false;

  // MultLo amd MultHi must be the first and second output of MultNode
  // respectively.
  if (MultHi.getResNo() != 1 || MultLo.getResNo() != 0)
    return false;

  // Transform this to a MADD only if ADDENode and ADDCNode are the only users
  // of the values of MultNode, in which case MultNode will be removed in later
  // phases.
  // If there exist users other than ADDENode or ADDCNode, this function returns
  // here, which will result in MultNode being mapped to a single MULT
  // instruction node rather than a pair of MULT and MADD instructions being
  // produced.
  if (!MultHi.hasOneUse() || !MultLo.hasOneUse())
    return false;

  SDLoc DL(ADDENode);

  // Initialize accumulator.
  SDValue ACCIn = CurDAG->getNode(MipsISD::InsertLOHI, DL, MVT::Untyped,
                                  ADDCNode->getOperand(1),
                                  ADDENode->getOperand(1));

  // create MipsMAdd(u) node
  MultOpc = MultOpc == ISD::UMUL_LOHI ? MipsISD::MAddu : MipsISD::MAdd;

  SDValue MAdd = CurDAG->getNode(MultOpc, DL, MVT::Untyped,
                                 MultNode->getOperand(0),// Factor 0
                                 MultNode->getOperand(1),// Factor 1
                                 ACCIn);

  // replace uses of adde and addc here
  if (!SDValue(ADDCNode, 0).use_empty()) {
    SDValue LoIdx = CurDAG->getConstant(Mips::sub_lo, MVT::i32);
    SDValue LoOut = CurDAG->getNode(MipsISD::ExtractLOHI, DL, MVT::i32, MAdd,
                                    LoIdx);
    CurDAG->ReplaceAllUsesOfValueWith(SDValue(ADDCNode, 0), LoOut);
  }
  if (!SDValue(ADDENode, 0).use_empty()) {
    SDValue HiIdx = CurDAG->getConstant(Mips::sub_hi, MVT::i32);
    SDValue HiOut = CurDAG->getNode(MipsISD::ExtractLOHI, DL, MVT::i32, MAdd,
                                    HiIdx);
    CurDAG->ReplaceAllUsesOfValueWith(SDValue(ADDENode, 0), HiOut);
  }

  return true;
}

// selectMSUB -
// Transforms a subgraph in CurDAG if the following pattern is found:
//  (addc Lo0, multLo), (sube Hi0, multHi),
// where,
//  multHi/Lo: product of multiplication
//  Lo0: initial value of Lo register
//  Hi0: initial value of Hi register
// Return true if pattern matching was successful.
static bool selectMSUB(SDNode *SUBENode, SelectionDAG *CurDAG) {
  // SUBENode's second operand must be a flag output of an SUBC node in order
  // for the matching to be successful.
  SDNode *SUBCNode = SUBENode->getOperand(2).getNode();

  if (SUBCNode->getOpcode() != ISD::SUBC)
    return false;

  SDValue MultHi = SUBENode->getOperand(1);
  SDValue MultLo = SUBCNode->getOperand(1);
  SDNode *MultNode = MultHi.getNode();
  unsigned MultOpc = MultHi.getOpcode();

  // MultHi and MultLo must be generated by the same node,
  if (MultLo.getNode() != MultNode)
    return false;

  // and it must be a multiplication.
  if (MultOpc != ISD::SMUL_LOHI && MultOpc != ISD::UMUL_LOHI)
    return false;

  // MultLo amd MultHi must be the first and second output of MultNode
  // respectively.
  if (MultHi.getResNo() != 1 || MultLo.getResNo() != 0)
    return false;

  // Transform this to a MSUB only if SUBENode and SUBCNode are the only users
  // of the values of MultNode, in which case MultNode will be removed in later
  // phases.
  // If there exist users other than SUBENode or SUBCNode, this function returns
  // here, which will result in MultNode being mapped to a single MULT
  // instruction node rather than a pair of MULT and MSUB instructions being
  // produced.
  if (!MultHi.hasOneUse() || !MultLo.hasOneUse())
    return false;

  SDLoc DL(SUBENode);

  // Initialize accumulator.
  SDValue ACCIn = CurDAG->getNode(MipsISD::InsertLOHI, DL, MVT::Untyped,
                                  SUBCNode->getOperand(0),
                                  SUBENode->getOperand(0));

  // create MipsSub(u) node
  MultOpc = MultOpc == ISD::UMUL_LOHI ? MipsISD::MSubu : MipsISD::MSub;

  SDValue MSub = CurDAG->getNode(MultOpc, DL, MVT::Glue,
                                 MultNode->getOperand(0),// Factor 0
                                 MultNode->getOperand(1),// Factor 1
                                 ACCIn);

  // replace uses of sube and subc here
  if (!SDValue(SUBCNode, 0).use_empty()) {
    SDValue LoIdx = CurDAG->getConstant(Mips::sub_lo, MVT::i32);
    SDValue LoOut = CurDAG->getNode(MipsISD::ExtractLOHI, DL, MVT::i32, MSub,
                                    LoIdx);
    CurDAG->ReplaceAllUsesOfValueWith(SDValue(SUBCNode, 0), LoOut);
  }
  if (!SDValue(SUBENode, 0).use_empty()) {
    SDValue HiIdx = CurDAG->getConstant(Mips::sub_hi, MVT::i32);
    SDValue HiOut = CurDAG->getNode(MipsISD::ExtractLOHI, DL, MVT::i32, MSub,
                                    HiIdx);
    CurDAG->ReplaceAllUsesOfValueWith(SDValue(SUBENode, 0), HiOut);
  }

  return true;
}

static SDValue performADDECombine(SDNode *N, SelectionDAG &DAG,
                                  TargetLowering::DAGCombinerInfo &DCI,
                                  const MipsSubtarget *Subtarget) {
  if (DCI.isBeforeLegalize())
    return SDValue();

  if (Subtarget->hasMips32() && N->getValueType(0) == MVT::i32 &&
      selectMADD(N, &DAG))
    return SDValue(N, 0);

  return SDValue();
}

// Fold zero extensions into MipsISD::VEXTRACT_[SZ]EXT_ELT
//
// Performs the following transformations:
// - Changes MipsISD::VEXTRACT_[SZ]EXT_ELT to zero extension if its
//   sign/zero-extension is completely overwritten by the new one performed by
//   the ISD::AND.
// - Removes redundant zero extensions performed by an ISD::AND.
static SDValue performANDCombine(SDNode *N, SelectionDAG &DAG,
                                 TargetLowering::DAGCombinerInfo &DCI,
                                 const MipsSubtarget *Subtarget) {
  if (!Subtarget->hasMSA())
    return SDValue();

  SDValue Op0 = N->getOperand(0);
  SDValue Op1 = N->getOperand(1);
  unsigned Op0Opcode = Op0->getOpcode();

  // (and (MipsVExtract[SZ]Ext $a, $b, $c), imm:$d)
  // where $d + 1 == 2^n and n == 32
  // or    $d + 1 == 2^n and n <= 32 and ZExt
  // -> (MipsVExtractZExt $a, $b, $c)
  if (Op0Opcode == MipsISD::VEXTRACT_SEXT_ELT ||
      Op0Opcode == MipsISD::VEXTRACT_ZEXT_ELT) {
    ConstantSDNode *Mask = dyn_cast<ConstantSDNode>(Op1);

    if (!Mask)
      return SDValue();

    int32_t Log2IfPositive = (Mask->getAPIntValue() + 1).exactLogBase2();

    if (Log2IfPositive <= 0)
      return SDValue(); // Mask+1 is not a power of 2

    SDValue Op0Op2 = Op0->getOperand(2);
    EVT ExtendTy = cast<VTSDNode>(Op0Op2)->getVT();
    unsigned ExtendTySize = ExtendTy.getSizeInBits();
    unsigned Log2 = Log2IfPositive;

    if ((Op0Opcode == MipsISD::VEXTRACT_ZEXT_ELT && Log2 >= ExtendTySize) ||
        Log2 == ExtendTySize) {
      SDValue Ops[] = { Op0->getOperand(0), Op0->getOperand(1), Op0Op2 };
      DAG.MorphNodeTo(Op0.getNode(), MipsISD::VEXTRACT_ZEXT_ELT,
                      Op0->getVTList(), Ops, Op0->getNumOperands());
      return Op0;
    }
  }

  return SDValue();
}

static SDValue performSUBECombine(SDNode *N, SelectionDAG &DAG,
                                  TargetLowering::DAGCombinerInfo &DCI,
                                  const MipsSubtarget *Subtarget) {
  if (DCI.isBeforeLegalize())
    return SDValue();

  if (Subtarget->hasMips32() && N->getValueType(0) == MVT::i32 &&
      selectMSUB(N, &DAG))
    return SDValue(N, 0);

  return SDValue();
}

static SDValue genConstMult(SDValue X, uint64_t C, SDLoc DL, EVT VT,
                            EVT ShiftTy, SelectionDAG &DAG) {
  // Clear the upper (64 - VT.sizeInBits) bits.
  C &= ((uint64_t)-1) >> (64 - VT.getSizeInBits());

  // Return 0.
  if (C == 0)
    return DAG.getConstant(0, VT);

  // Return x.
  if (C == 1)
    return X;

  // If c is power of 2, return (shl x, log2(c)).
  if (isPowerOf2_64(C))
    return DAG.getNode(ISD::SHL, DL, VT, X,
                       DAG.getConstant(Log2_64(C), ShiftTy));

  unsigned Log2Ceil = Log2_64_Ceil(C);
  uint64_t Floor = 1LL << Log2_64(C);
  uint64_t Ceil = Log2Ceil == 64 ? 0LL : 1LL << Log2Ceil;

  // If |c - floor_c| <= |c - ceil_c|,
  // where floor_c = pow(2, floor(log2(c))) and ceil_c = pow(2, ceil(log2(c))),
  // return (add constMult(x, floor_c), constMult(x, c - floor_c)).
  if (C - Floor <= Ceil - C) {
    SDValue Op0 = genConstMult(X, Floor, DL, VT, ShiftTy, DAG);
    SDValue Op1 = genConstMult(X, C - Floor, DL, VT, ShiftTy, DAG);
    return DAG.getNode(ISD::ADD, DL, VT, Op0, Op1);
  }

  // If |c - floor_c| > |c - ceil_c|,
  // return (sub constMult(x, ceil_c), constMult(x, ceil_c - c)).
  SDValue Op0 = genConstMult(X, Ceil, DL, VT, ShiftTy, DAG);
  SDValue Op1 = genConstMult(X, Ceil - C, DL, VT, ShiftTy, DAG);
  return DAG.getNode(ISD::SUB, DL, VT, Op0, Op1);
}

static SDValue performMULCombine(SDNode *N, SelectionDAG &DAG,
                                 const TargetLowering::DAGCombinerInfo &DCI,
                                 const MipsSETargetLowering *TL) {
  EVT VT = N->getValueType(0);

  if (ConstantSDNode *C = dyn_cast<ConstantSDNode>(N->getOperand(1)))
    if (!VT.isVector())
      return genConstMult(N->getOperand(0), C->getZExtValue(), SDLoc(N),
                          VT, TL->getScalarShiftAmountTy(VT), DAG);

  return SDValue(N, 0);
}

static SDValue performDSPShiftCombine(unsigned Opc, SDNode *N, EVT Ty,
                                      SelectionDAG &DAG,
                                      const MipsSubtarget *Subtarget) {
  // See if this is a vector splat immediate node.
  APInt SplatValue, SplatUndef;
  unsigned SplatBitSize;
  bool HasAnyUndefs;
  unsigned EltSize = Ty.getVectorElementType().getSizeInBits();
  BuildVectorSDNode *BV = dyn_cast<BuildVectorSDNode>(N->getOperand(1));

  if (!BV ||
      !BV->isConstantSplat(SplatValue, SplatUndef, SplatBitSize, HasAnyUndefs,
                           EltSize, !Subtarget->isLittle()) ||
      (SplatBitSize != EltSize) ||
      (SplatValue.getZExtValue() >= EltSize))
    return SDValue();

  return DAG.getNode(Opc, SDLoc(N), Ty, N->getOperand(0),
                     DAG.getConstant(SplatValue.getZExtValue(), MVT::i32));
}

static SDValue performSHLCombine(SDNode *N, SelectionDAG &DAG,
                                 TargetLowering::DAGCombinerInfo &DCI,
                                 const MipsSubtarget *Subtarget) {
  EVT Ty = N->getValueType(0);

  if ((Ty != MVT::v2i16) && (Ty != MVT::v4i8))
    return SDValue();

  return performDSPShiftCombine(MipsISD::SHLL_DSP, N, Ty, DAG, Subtarget);
}

// Fold sign-extensions into MipsISD::VEXTRACT_[SZ]EXT_ELT for MSA and fold
// constant splats into MipsISD::SHRA_DSP for DSPr2.
//
// Performs the following transformations:
// - Changes MipsISD::VEXTRACT_[SZ]EXT_ELT to sign extension if its
//   sign/zero-extension is completely overwritten by the new one performed by
//   the ISD::SRA and ISD::SHL nodes.
// - Removes redundant sign extensions performed by an ISD::SRA and ISD::SHL
//   sequence.
//
// See performDSPShiftCombine for more information about the transformation
// used for DSPr2.
static SDValue performSRACombine(SDNode *N, SelectionDAG &DAG,
                                 TargetLowering::DAGCombinerInfo &DCI,
                                 const MipsSubtarget *Subtarget) {
  EVT Ty = N->getValueType(0);

  if (Subtarget->hasMSA()) {
    SDValue Op0 = N->getOperand(0);
    SDValue Op1 = N->getOperand(1);

    // (sra (shl (MipsVExtract[SZ]Ext $a, $b, $c), imm:$d), imm:$d)
    // where $d + sizeof($c) == 32
    // or    $d + sizeof($c) <= 32 and SExt
    // -> (MipsVExtractSExt $a, $b, $c)
    if (Op0->getOpcode() == ISD::SHL && Op1 == Op0->getOperand(1)) {
      SDValue Op0Op0 = Op0->getOperand(0);
      ConstantSDNode *ShAmount = dyn_cast<ConstantSDNode>(Op1);

      if (!ShAmount)
        return SDValue();

      if (Op0Op0->getOpcode() != MipsISD::VEXTRACT_SEXT_ELT &&
          Op0Op0->getOpcode() != MipsISD::VEXTRACT_ZEXT_ELT)
        return SDValue();

      EVT ExtendTy = cast<VTSDNode>(Op0Op0->getOperand(2))->getVT();
      unsigned TotalBits = ShAmount->getZExtValue() + ExtendTy.getSizeInBits();

      if (TotalBits == 32 ||
          (Op0Op0->getOpcode() == MipsISD::VEXTRACT_SEXT_ELT &&
           TotalBits <= 32)) {
        SDValue Ops[] = { Op0Op0->getOperand(0), Op0Op0->getOperand(1),
                          Op0Op0->getOperand(2) };
        DAG.MorphNodeTo(Op0Op0.getNode(), MipsISD::VEXTRACT_SEXT_ELT,
                        Op0Op0->getVTList(), Ops, Op0Op0->getNumOperands());
        return Op0Op0;
      }
    }
  }

  if ((Ty != MVT::v2i16) && ((Ty != MVT::v4i8) || !Subtarget->hasDSPR2()))
    return SDValue();

  return performDSPShiftCombine(MipsISD::SHRA_DSP, N, Ty, DAG, Subtarget);
}


static SDValue performSRLCombine(SDNode *N, SelectionDAG &DAG,
                                 TargetLowering::DAGCombinerInfo &DCI,
                                 const MipsSubtarget *Subtarget) {
  EVT Ty = N->getValueType(0);

  if (((Ty != MVT::v2i16) || !Subtarget->hasDSPR2()) && (Ty != MVT::v4i8))
    return SDValue();

  return performDSPShiftCombine(MipsISD::SHRL_DSP, N, Ty, DAG, Subtarget);
}

static bool isLegalDSPCondCode(EVT Ty, ISD::CondCode CC) {
  bool IsV216 = (Ty == MVT::v2i16);

  switch (CC) {
  case ISD::SETEQ:
  case ISD::SETNE:  return true;
  case ISD::SETLT:
  case ISD::SETLE:
  case ISD::SETGT:
  case ISD::SETGE:  return IsV216;
  case ISD::SETULT:
  case ISD::SETULE:
  case ISD::SETUGT:
  case ISD::SETUGE: return !IsV216;
  default:          return false;
  }
}

static SDValue performSETCCCombine(SDNode *N, SelectionDAG &DAG) {
  EVT Ty = N->getValueType(0);

  if ((Ty != MVT::v2i16) && (Ty != MVT::v4i8))
    return SDValue();

  if (!isLegalDSPCondCode(Ty, cast<CondCodeSDNode>(N->getOperand(2))->get()))
    return SDValue();

  return DAG.getNode(MipsISD::SETCC_DSP, SDLoc(N), Ty, N->getOperand(0),
                     N->getOperand(1), N->getOperand(2));
}

static SDValue performVSELECTCombine(SDNode *N, SelectionDAG &DAG) {
  EVT Ty = N->getValueType(0);

  if (Ty.is128BitVector() && Ty.isInteger()) {
    // Try the following combines:
    //   (vselect (setcc $a, $b, SETLT), $b, $a)) -> (vsmax $a, $b)
    //   (vselect (setcc $a, $b, SETLE), $b, $a)) -> (vsmax $a, $b)
    //   (vselect (setcc $a, $b, SETLT), $a, $b)) -> (vsmin $a, $b)
    //   (vselect (setcc $a, $b, SETLE), $a, $b)) -> (vsmin $a, $b)
    //   (vselect (setcc $a, $b, SETULT), $b, $a)) -> (vumax $a, $b)
    //   (vselect (setcc $a, $b, SETULE), $b, $a)) -> (vumax $a, $b)
    //   (vselect (setcc $a, $b, SETULT), $a, $b)) -> (vumin $a, $b)
    //   (vselect (setcc $a, $b, SETULE), $a, $b)) -> (vumin $a, $b)
    // SETGT/SETGE/SETUGT/SETUGE variants of these will show up initially but
    // will be expanded to equivalent SETLT/SETLE/SETULT/SETULE versions by the
    // legalizer.
    SDValue Op0 = N->getOperand(0);

    if (Op0->getOpcode() != ISD::SETCC)
      return SDValue();

    ISD::CondCode CondCode = cast<CondCodeSDNode>(Op0->getOperand(2))->get();
    bool Signed;

    if (CondCode == ISD::SETLT  || CondCode == ISD::SETLE)
      Signed = true;
    else if (CondCode == ISD::SETULT || CondCode == ISD::SETULE)
      Signed = false;
    else
      return SDValue();

    SDValue Op1 = N->getOperand(1);
    SDValue Op2 = N->getOperand(2);
    SDValue Op0Op0 = Op0->getOperand(0);
    SDValue Op0Op1 = Op0->getOperand(1);

    if (Op1 == Op0Op0 && Op2 == Op0Op1)
      return DAG.getNode(Signed ? MipsISD::VSMIN : MipsISD::VUMIN, SDLoc(N),
                         Ty, Op1, Op2);
    else if (Op1 == Op0Op1 && Op2 == Op0Op0)
      return DAG.getNode(Signed ? MipsISD::VSMAX : MipsISD::VUMAX, SDLoc(N),
                         Ty, Op1, Op2);
  } else if ((Ty == MVT::v2i16) || (Ty == MVT::v4i8)) {
    SDValue SetCC = N->getOperand(0);

    if (SetCC.getOpcode() != MipsISD::SETCC_DSP)
      return SDValue();

    return DAG.getNode(MipsISD::SELECT_CC_DSP, SDLoc(N), Ty,
                       SetCC.getOperand(0), SetCC.getOperand(1),
                       N->getOperand(1), N->getOperand(2), SetCC.getOperand(2));
  }

  return SDValue();
}

static SDValue performXORCombine(SDNode *N, SelectionDAG &DAG,
                                 const MipsSubtarget *Subtarget) {
  EVT Ty = N->getValueType(0);

  if (Subtarget->hasMSA() && Ty.is128BitVector() && Ty.isInteger()) {
    // Try the following combines:
    //   (xor (or $a, $b), (build_vector allones))
    //   (xor (or $a, $b), (bitcast (build_vector allones)))
    SDValue Op0 = N->getOperand(0);
    SDValue Op1 = N->getOperand(1);
    SDValue NotOp;

    if (ISD::isBuildVectorAllOnes(Op0.getNode()))
      NotOp = Op1;
    else if (ISD::isBuildVectorAllOnes(Op1.getNode()))
      NotOp = Op0;
    else
      return SDValue();

    if (NotOp->getOpcode() == ISD::OR)
      return DAG.getNode(MipsISD::VNOR, SDLoc(N), Ty, NotOp->getOperand(0),
                         NotOp->getOperand(1));
  }

  return SDValue();
}

SDValue
MipsSETargetLowering::PerformDAGCombine(SDNode *N, DAGCombinerInfo &DCI) const {
  SelectionDAG &DAG = DCI.DAG;
  SDValue Val;

  switch (N->getOpcode()) {
  case ISD::ADDE:
    return performADDECombine(N, DAG, DCI, Subtarget);
  case ISD::AND:
    Val = performANDCombine(N, DAG, DCI, Subtarget);
    break;
  case ISD::SUBE:
    return performSUBECombine(N, DAG, DCI, Subtarget);
  case ISD::MUL:
    return performMULCombine(N, DAG, DCI, this);
  case ISD::SHL:
    return performSHLCombine(N, DAG, DCI, Subtarget);
  case ISD::SRA:
    return performSRACombine(N, DAG, DCI, Subtarget);
  case ISD::SRL:
    return performSRLCombine(N, DAG, DCI, Subtarget);
  case ISD::VSELECT:
    return performVSELECTCombine(N, DAG);
  case ISD::XOR:
    Val = performXORCombine(N, DAG, Subtarget);
    break;
  case ISD::SETCC:
    Val = performSETCCCombine(N, DAG);
    break;
  }

  if (Val.getNode())
    return Val;

  return MipsTargetLowering::PerformDAGCombine(N, DCI);
}

MachineBasicBlock *
MipsSETargetLowering::EmitInstrWithCustomInserter(MachineInstr *MI,
                                                  MachineBasicBlock *BB) const {
  switch (MI->getOpcode()) {
  default:
    return MipsTargetLowering::EmitInstrWithCustomInserter(MI, BB);
  case Mips::BPOSGE32_PSEUDO:
    return emitBPOSGE32(MI, BB);
  case Mips::SNZ_B_PSEUDO:
    return emitMSACBranchPseudo(MI, BB, Mips::BNZ_B);
  case Mips::SNZ_H_PSEUDO:
    return emitMSACBranchPseudo(MI, BB, Mips::BNZ_H);
  case Mips::SNZ_W_PSEUDO:
    return emitMSACBranchPseudo(MI, BB, Mips::BNZ_W);
  case Mips::SNZ_D_PSEUDO:
    return emitMSACBranchPseudo(MI, BB, Mips::BNZ_D);
  case Mips::SNZ_V_PSEUDO:
    return emitMSACBranchPseudo(MI, BB, Mips::BNZ_V);
  case Mips::SZ_B_PSEUDO:
    return emitMSACBranchPseudo(MI, BB, Mips::BZ_B);
  case Mips::SZ_H_PSEUDO:
    return emitMSACBranchPseudo(MI, BB, Mips::BZ_H);
  case Mips::SZ_W_PSEUDO:
    return emitMSACBranchPseudo(MI, BB, Mips::BZ_W);
  case Mips::SZ_D_PSEUDO:
    return emitMSACBranchPseudo(MI, BB, Mips::BZ_D);
  case Mips::SZ_V_PSEUDO:
    return emitMSACBranchPseudo(MI, BB, Mips::BZ_V);
  }
}

bool MipsSETargetLowering::
isEligibleForTailCallOptimization(const MipsCC &MipsCCInfo,
                                  unsigned NextStackOffset,
                                  const MipsFunctionInfo& FI) const {
  if (!EnableMipsTailCalls)
    return false;

  // Return false if either the callee or caller has a byval argument.
  if (MipsCCInfo.hasByValArg() || FI.hasByvalArg())
    return false;

  // Return true if the callee's argument area is no larger than the
  // caller's.
  return NextStackOffset <= FI.getIncomingArgSize();
}

void MipsSETargetLowering::
getOpndList(SmallVectorImpl<SDValue> &Ops,
            std::deque< std::pair<unsigned, SDValue> > &RegsToPass,
            bool IsPICCall, bool GlobalOrExternal, bool InternalLinkage,
            CallLoweringInfo &CLI, SDValue Callee, SDValue Chain) const {
  // T9 should contain the address of the callee function if
  // -reloction-model=pic or it is an indirect call.
  if (IsPICCall || !GlobalOrExternal) {
    unsigned T9Reg = IsN64 ? Mips::T9_64 : Mips::T9;
    RegsToPass.push_front(std::make_pair(T9Reg, Callee));
  } else
    Ops.push_back(Callee);

  MipsTargetLowering::getOpndList(Ops, RegsToPass, IsPICCall, GlobalOrExternal,
                                  InternalLinkage, CLI, Callee, Chain);
}

SDValue MipsSETargetLowering::lowerLOAD(SDValue Op, SelectionDAG &DAG) const {
  LoadSDNode &Nd = *cast<LoadSDNode>(Op);

  if (Nd.getMemoryVT() != MVT::f64 || !NoDPLoadStore)
    return MipsTargetLowering::lowerLOAD(Op, DAG);

  // Replace a double precision load with two i32 loads and a buildpair64.
  SDLoc DL(Op);
  SDValue Ptr = Nd.getBasePtr(), Chain = Nd.getChain();
  EVT PtrVT = Ptr.getValueType();

  // i32 load from lower address.
  SDValue Lo = DAG.getLoad(MVT::i32, DL, Chain, Ptr,
                           MachinePointerInfo(), Nd.isVolatile(),
                           Nd.isNonTemporal(), Nd.isInvariant(),
                           Nd.getAlignment());

  // i32 load from higher address.
  Ptr = DAG.getNode(ISD::ADD, DL, PtrVT, Ptr, DAG.getConstant(4, PtrVT));
  SDValue Hi = DAG.getLoad(MVT::i32, DL, Lo.getValue(1), Ptr,
                           MachinePointerInfo(), Nd.isVolatile(),
                           Nd.isNonTemporal(), Nd.isInvariant(),
                           std::min(Nd.getAlignment(), 4U));

  if (!Subtarget->isLittle())
    std::swap(Lo, Hi);

  SDValue BP = DAG.getNode(MipsISD::BuildPairF64, DL, MVT::f64, Lo, Hi);
  SDValue Ops[2] = {BP, Hi.getValue(1)};
  return DAG.getMergeValues(Ops, 2, DL);
}

SDValue MipsSETargetLowering::lowerSTORE(SDValue Op, SelectionDAG &DAG) const {
  StoreSDNode &Nd = *cast<StoreSDNode>(Op);

  if (Nd.getMemoryVT() != MVT::f64 || !NoDPLoadStore)
    return MipsTargetLowering::lowerSTORE(Op, DAG);

  // Replace a double precision store with two extractelement64s and i32 stores.
  SDLoc DL(Op);
  SDValue Val = Nd.getValue(), Ptr = Nd.getBasePtr(), Chain = Nd.getChain();
  EVT PtrVT = Ptr.getValueType();
  SDValue Lo = DAG.getNode(MipsISD::ExtractElementF64, DL, MVT::i32,
                           Val, DAG.getConstant(0, MVT::i32));
  SDValue Hi = DAG.getNode(MipsISD::ExtractElementF64, DL, MVT::i32,
                           Val, DAG.getConstant(1, MVT::i32));

  if (!Subtarget->isLittle())
    std::swap(Lo, Hi);

  // i32 store to lower address.
  Chain = DAG.getStore(Chain, DL, Lo, Ptr, MachinePointerInfo(),
                       Nd.isVolatile(), Nd.isNonTemporal(), Nd.getAlignment(),
                       Nd.getTBAAInfo());

  // i32 store to higher address.
  Ptr = DAG.getNode(ISD::ADD, DL, PtrVT, Ptr, DAG.getConstant(4, PtrVT));
  return DAG.getStore(Chain, DL, Hi, Ptr, MachinePointerInfo(),
                      Nd.isVolatile(), Nd.isNonTemporal(),
                      std::min(Nd.getAlignment(), 4U), Nd.getTBAAInfo());
}

SDValue MipsSETargetLowering::lowerMulDiv(SDValue Op, unsigned NewOpc,
                                          bool HasLo, bool HasHi,
                                          SelectionDAG &DAG) const {
  EVT Ty = Op.getOperand(0).getValueType();
  SDLoc DL(Op);
  SDValue Mult = DAG.getNode(NewOpc, DL, MVT::Untyped,
                             Op.getOperand(0), Op.getOperand(1));
  SDValue Lo, Hi;

  if (HasLo)
    Lo = DAG.getNode(MipsISD::ExtractLOHI, DL, Ty, Mult,
                     DAG.getConstant(Mips::sub_lo, MVT::i32));
  if (HasHi)
    Hi = DAG.getNode(MipsISD::ExtractLOHI, DL, Ty, Mult,
                     DAG.getConstant(Mips::sub_hi, MVT::i32));

  if (!HasLo || !HasHi)
    return HasLo ? Lo : Hi;

  SDValue Vals[] = { Lo, Hi };
  return DAG.getMergeValues(Vals, 2, DL);
}


static SDValue initAccumulator(SDValue In, SDLoc DL, SelectionDAG &DAG) {
  SDValue InLo = DAG.getNode(ISD::EXTRACT_ELEMENT, DL, MVT::i32, In,
                             DAG.getConstant(0, MVT::i32));
  SDValue InHi = DAG.getNode(ISD::EXTRACT_ELEMENT, DL, MVT::i32, In,
                             DAG.getConstant(1, MVT::i32));
  return DAG.getNode(MipsISD::InsertLOHI, DL, MVT::Untyped, InLo, InHi);
}

static SDValue extractLOHI(SDValue Op, SDLoc DL, SelectionDAG &DAG) {
  SDValue Lo = DAG.getNode(MipsISD::ExtractLOHI, DL, MVT::i32, Op,
                           DAG.getConstant(Mips::sub_lo, MVT::i32));
  SDValue Hi = DAG.getNode(MipsISD::ExtractLOHI, DL, MVT::i32, Op,
                           DAG.getConstant(Mips::sub_hi, MVT::i32));
  return DAG.getNode(ISD::BUILD_PAIR, DL, MVT::i64, Lo, Hi);
}

// This function expands mips intrinsic nodes which have 64-bit input operands
// or output values.
//
// out64 = intrinsic-node in64
// =>
// lo = copy (extract-element (in64, 0))
// hi = copy (extract-element (in64, 1))
// mips-specific-node
// v0 = copy lo
// v1 = copy hi
// out64 = merge-values (v0, v1)
//
static SDValue lowerDSPIntr(SDValue Op, SelectionDAG &DAG, unsigned Opc) {
  SDLoc DL(Op);
  bool HasChainIn = Op->getOperand(0).getValueType() == MVT::Other;
  SmallVector<SDValue, 3> Ops;
  unsigned OpNo = 0;

  // See if Op has a chain input.
  if (HasChainIn)
    Ops.push_back(Op->getOperand(OpNo++));

  // The next operand is the intrinsic opcode.
  assert(Op->getOperand(OpNo).getOpcode() == ISD::TargetConstant);

  // See if the next operand has type i64.
  SDValue Opnd = Op->getOperand(++OpNo), In64;

  if (Opnd.getValueType() == MVT::i64)
    In64 = initAccumulator(Opnd, DL, DAG);
  else
    Ops.push_back(Opnd);

  // Push the remaining operands.
  for (++OpNo ; OpNo < Op->getNumOperands(); ++OpNo)
    Ops.push_back(Op->getOperand(OpNo));

  // Add In64 to the end of the list.
  if (In64.getNode())
    Ops.push_back(In64);

  // Scan output.
  SmallVector<EVT, 2> ResTys;

  for (SDNode::value_iterator I = Op->value_begin(), E = Op->value_end();
       I != E; ++I)
    ResTys.push_back((*I == MVT::i64) ? MVT::Untyped : *I);

  // Create node.
  SDValue Val = DAG.getNode(Opc, DL, ResTys, &Ops[0], Ops.size());
  SDValue Out = (ResTys[0] == MVT::Untyped) ? extractLOHI(Val, DL, DAG) : Val;

  if (!HasChainIn)
    return Out;

  assert(Val->getValueType(1) == MVT::Other);
  SDValue Vals[] = { Out, SDValue(Val.getNode(), 1) };
  return DAG.getMergeValues(Vals, 2, DL);
}

static SDValue lowerMSABinaryIntr(SDValue Op, SelectionDAG &DAG, unsigned Opc) {
  SDLoc DL(Op);
  SDValue LHS = Op->getOperand(1);
  SDValue RHS = Op->getOperand(2);
  EVT ResTy = Op->getValueType(0);

  SDValue Result = DAG.getNode(Opc, DL, ResTy, LHS, RHS);

  return Result;
}

static SDValue lowerMSABinaryImmIntr(SDValue Op, SelectionDAG &DAG,
                                     unsigned Opc, SDValue RHS) {
  SDValue LHS = Op->getOperand(1);
  EVT ResTy = Op->getValueType(0);

  return DAG.getNode(Opc, SDLoc(Op), ResTy, LHS, RHS);
}

static SDValue lowerMSABranchIntr(SDValue Op, SelectionDAG &DAG, unsigned Opc) {
  SDLoc DL(Op);
  SDValue Value = Op->getOperand(1);
  EVT ResTy = Op->getValueType(0);

  SDValue Result = DAG.getNode(Opc, DL, ResTy, Value);

  return Result;
}

// Lower an MSA copy intrinsic into the specified SelectionDAG node
static SDValue lowerMSACopyIntr(SDValue Op, SelectionDAG &DAG, unsigned Opc) {
  SDLoc DL(Op);
  SDValue Vec = Op->getOperand(1);
  SDValue Idx = Op->getOperand(2);
  EVT ResTy = Op->getValueType(0);
  EVT EltTy = Vec->getValueType(0).getVectorElementType();

  SDValue Result = DAG.getNode(Opc, DL, ResTy, Vec, Idx,
                               DAG.getValueType(EltTy));

  return Result;
}

// Lower an MSA insert intrinsic into the specified SelectionDAG node
static SDValue lowerMSAInsertIntr(SDValue Op, SelectionDAG &DAG, unsigned Opc) {
  SDLoc DL(Op);
  SDValue Op0 = Op->getOperand(1);
  SDValue Op1 = Op->getOperand(2);
  SDValue Op2 = Op->getOperand(3);
  EVT ResTy = Op->getValueType(0);

  SDValue Result = DAG.getNode(Opc, DL, ResTy, Op0, Op2, Op1);

  return Result;
}

static SDValue lowerMSASplatImm(SDValue Op, SDValue ImmOp, SelectionDAG &DAG) {
  EVT ResTy = Op->getValueType(0);
  EVT ViaVecTy = ResTy;
  SmallVector<SDValue, 16> Ops;
  SDValue ImmHiOp;
  SDLoc DL(Op);

  if (ViaVecTy == MVT::v2i64) {
    ImmHiOp = DAG.getNode(ISD::SRA, DL, MVT::i32, ImmOp,
                          DAG.getConstant(31, MVT::i32));
    for (unsigned i = 0; i < ViaVecTy.getVectorNumElements(); ++i) {
      Ops.push_back(ImmHiOp);
      Ops.push_back(ImmOp);
    }
    ViaVecTy = MVT::v4i32;
  } else {
    for (unsigned i = 0; i < ResTy.getVectorNumElements(); ++i)
      Ops.push_back(ImmOp);
  }

  SDValue Result = DAG.getNode(ISD::BUILD_VECTOR, DL, ViaVecTy, &Ops[0],
                               Ops.size());

  if (ResTy != ViaVecTy)
    Result = DAG.getNode(ISD::BITCAST, DL, ResTy, Result);

  return Result;
}

static SDValue
lowerMSASplatImm(SDValue Op, unsigned ImmOp, SelectionDAG &DAG) {
  return lowerMSASplatImm(Op, Op->getOperand(ImmOp), DAG);
}

static SDValue lowerMSAUnaryIntr(SDValue Op, SelectionDAG &DAG, unsigned Opc) {
  SDLoc DL(Op);
  SDValue Value = Op->getOperand(1);
  EVT ResTy = Op->getValueType(0);

  SDValue Result = DAG.getNode(Opc, DL, ResTy, Value);

  return Result;
}

SDValue MipsSETargetLowering::lowerINTRINSIC_WO_CHAIN(SDValue Op,
                                                      SelectionDAG &DAG) const {
  switch (cast<ConstantSDNode>(Op->getOperand(0))->getZExtValue()) {
  default:
    return SDValue();
  case Intrinsic::mips_shilo:
    return lowerDSPIntr(Op, DAG, MipsISD::SHILO);
  case Intrinsic::mips_dpau_h_qbl:
    return lowerDSPIntr(Op, DAG, MipsISD::DPAU_H_QBL);
  case Intrinsic::mips_dpau_h_qbr:
    return lowerDSPIntr(Op, DAG, MipsISD::DPAU_H_QBR);
  case Intrinsic::mips_dpsu_h_qbl:
    return lowerDSPIntr(Op, DAG, MipsISD::DPSU_H_QBL);
  case Intrinsic::mips_dpsu_h_qbr:
    return lowerDSPIntr(Op, DAG, MipsISD::DPSU_H_QBR);
  case Intrinsic::mips_dpa_w_ph:
    return lowerDSPIntr(Op, DAG, MipsISD::DPA_W_PH);
  case Intrinsic::mips_dps_w_ph:
    return lowerDSPIntr(Op, DAG, MipsISD::DPS_W_PH);
  case Intrinsic::mips_dpax_w_ph:
    return lowerDSPIntr(Op, DAG, MipsISD::DPAX_W_PH);
  case Intrinsic::mips_dpsx_w_ph:
    return lowerDSPIntr(Op, DAG, MipsISD::DPSX_W_PH);
  case Intrinsic::mips_mulsa_w_ph:
    return lowerDSPIntr(Op, DAG, MipsISD::MULSA_W_PH);
  case Intrinsic::mips_mult:
    return lowerDSPIntr(Op, DAG, MipsISD::Mult);
  case Intrinsic::mips_multu:
    return lowerDSPIntr(Op, DAG, MipsISD::Multu);
  case Intrinsic::mips_madd:
    return lowerDSPIntr(Op, DAG, MipsISD::MAdd);
  case Intrinsic::mips_maddu:
    return lowerDSPIntr(Op, DAG, MipsISD::MAddu);
  case Intrinsic::mips_msub:
    return lowerDSPIntr(Op, DAG, MipsISD::MSub);
  case Intrinsic::mips_msubu:
    return lowerDSPIntr(Op, DAG, MipsISD::MSubu);
  case Intrinsic::mips_addv_b:
  case Intrinsic::mips_addv_h:
  case Intrinsic::mips_addv_w:
  case Intrinsic::mips_addv_d:
    return lowerMSABinaryIntr(Op, DAG, ISD::ADD);
  case Intrinsic::mips_addvi_b:
  case Intrinsic::mips_addvi_h:
  case Intrinsic::mips_addvi_w:
  case Intrinsic::mips_addvi_d:
    return lowerMSABinaryImmIntr(Op, DAG, ISD::ADD,
                                 lowerMSASplatImm(Op, 2, DAG));
  case Intrinsic::mips_and_v:
    return lowerMSABinaryIntr(Op, DAG, ISD::AND);
  case Intrinsic::mips_andi_b:
    return lowerMSABinaryImmIntr(Op, DAG, ISD::AND,
                                 lowerMSASplatImm(Op, 2, DAG));
  case Intrinsic::mips_bnz_b:
  case Intrinsic::mips_bnz_h:
  case Intrinsic::mips_bnz_w:
  case Intrinsic::mips_bnz_d:
    return lowerMSABranchIntr(Op, DAG, MipsISD::VALL_NONZERO);
  case Intrinsic::mips_bnz_v:
    return lowerMSABranchIntr(Op, DAG, MipsISD::VANY_NONZERO);
  case Intrinsic::mips_bsel_v:
    return DAG.getNode(ISD::VSELECT, SDLoc(Op), Op->getValueType(0),
                       Op->getOperand(1), Op->getOperand(2),
                       Op->getOperand(3));
  case Intrinsic::mips_bseli_b:
    return DAG.getNode(ISD::VSELECT, SDLoc(Op), Op->getValueType(0),
                       Op->getOperand(1), Op->getOperand(2),
                       lowerMSASplatImm(Op, 3, DAG));
  case Intrinsic::mips_bz_b:
  case Intrinsic::mips_bz_h:
  case Intrinsic::mips_bz_w:
  case Intrinsic::mips_bz_d:
    return lowerMSABranchIntr(Op, DAG, MipsISD::VALL_ZERO);
  case Intrinsic::mips_bz_v:
    return lowerMSABranchIntr(Op, DAG, MipsISD::VANY_ZERO);
  case Intrinsic::mips_ceq_b:
  case Intrinsic::mips_ceq_h:
  case Intrinsic::mips_ceq_w:
  case Intrinsic::mips_ceq_d:
    return DAG.getSetCC(SDLoc(Op), Op->getValueType(0), Op->getOperand(1),
                        Op->getOperand(2), ISD::SETEQ);
  case Intrinsic::mips_ceqi_b:
  case Intrinsic::mips_ceqi_h:
  case Intrinsic::mips_ceqi_w:
  case Intrinsic::mips_ceqi_d:
    return DAG.getSetCC(SDLoc(Op), Op->getValueType(0), Op->getOperand(1),
                        lowerMSASplatImm(Op, 2, DAG), ISD::SETEQ);
  case Intrinsic::mips_cle_s_b:
  case Intrinsic::mips_cle_s_h:
  case Intrinsic::mips_cle_s_w:
  case Intrinsic::mips_cle_s_d:
    return DAG.getSetCC(SDLoc(Op), Op->getValueType(0), Op->getOperand(1),
                        Op->getOperand(2), ISD::SETLE);
  case Intrinsic::mips_clei_s_b:
  case Intrinsic::mips_clei_s_h:
  case Intrinsic::mips_clei_s_w:
  case Intrinsic::mips_clei_s_d:
    return DAG.getSetCC(SDLoc(Op), Op->getValueType(0), Op->getOperand(1),
                        lowerMSASplatImm(Op, 2, DAG), ISD::SETLE);
  case Intrinsic::mips_cle_u_b:
  case Intrinsic::mips_cle_u_h:
  case Intrinsic::mips_cle_u_w:
  case Intrinsic::mips_cle_u_d:
    return DAG.getSetCC(SDLoc(Op), Op->getValueType(0), Op->getOperand(1),
                        Op->getOperand(2), ISD::SETULE);
  case Intrinsic::mips_clei_u_b:
  case Intrinsic::mips_clei_u_h:
  case Intrinsic::mips_clei_u_w:
  case Intrinsic::mips_clei_u_d:
    return DAG.getSetCC(SDLoc(Op), Op->getValueType(0), Op->getOperand(1),
                        lowerMSASplatImm(Op, 2, DAG), ISD::SETULE);
  case Intrinsic::mips_clt_s_b:
  case Intrinsic::mips_clt_s_h:
  case Intrinsic::mips_clt_s_w:
  case Intrinsic::mips_clt_s_d:
    return DAG.getSetCC(SDLoc(Op), Op->getValueType(0), Op->getOperand(1),
                        Op->getOperand(2), ISD::SETLT);
  case Intrinsic::mips_clti_s_b:
  case Intrinsic::mips_clti_s_h:
  case Intrinsic::mips_clti_s_w:
  case Intrinsic::mips_clti_s_d:
    return DAG.getSetCC(SDLoc(Op), Op->getValueType(0), Op->getOperand(1),
                        lowerMSASplatImm(Op, 2, DAG), ISD::SETLT);
  case Intrinsic::mips_clt_u_b:
  case Intrinsic::mips_clt_u_h:
  case Intrinsic::mips_clt_u_w:
  case Intrinsic::mips_clt_u_d:
    return DAG.getSetCC(SDLoc(Op), Op->getValueType(0), Op->getOperand(1),
                        Op->getOperand(2), ISD::SETULT);
  case Intrinsic::mips_clti_u_b:
  case Intrinsic::mips_clti_u_h:
  case Intrinsic::mips_clti_u_w:
  case Intrinsic::mips_clti_u_d:
    return DAG.getSetCC(SDLoc(Op), Op->getValueType(0), Op->getOperand(1),
                        lowerMSASplatImm(Op, 2, DAG), ISD::SETULT);
  case Intrinsic::mips_copy_s_b:
  case Intrinsic::mips_copy_s_h:
  case Intrinsic::mips_copy_s_w:
    return lowerMSACopyIntr(Op, DAG, MipsISD::VEXTRACT_SEXT_ELT);
  case Intrinsic::mips_copy_u_b:
  case Intrinsic::mips_copy_u_h:
  case Intrinsic::mips_copy_u_w:
    return lowerMSACopyIntr(Op, DAG, MipsISD::VEXTRACT_ZEXT_ELT);
  case Intrinsic::mips_div_s_b:
  case Intrinsic::mips_div_s_h:
  case Intrinsic::mips_div_s_w:
  case Intrinsic::mips_div_s_d:
    return lowerMSABinaryIntr(Op, DAG, ISD::SDIV);
  case Intrinsic::mips_div_u_b:
  case Intrinsic::mips_div_u_h:
  case Intrinsic::mips_div_u_w:
  case Intrinsic::mips_div_u_d:
    return lowerMSABinaryIntr(Op, DAG, ISD::UDIV);
  case Intrinsic::mips_fadd_w:
  case Intrinsic::mips_fadd_d:
    return lowerMSABinaryIntr(Op, DAG, ISD::FADD);
  // Don't lower mips_fcaf_[wd] since LLVM folds SETFALSE condcodes away
  case Intrinsic::mips_fceq_w:
  case Intrinsic::mips_fceq_d:
    return DAG.getSetCC(SDLoc(Op), Op->getValueType(0), Op->getOperand(1),
                        Op->getOperand(2), ISD::SETOEQ);
  case Intrinsic::mips_fcle_w:
  case Intrinsic::mips_fcle_d:
    return DAG.getSetCC(SDLoc(Op), Op->getValueType(0), Op->getOperand(1),
                        Op->getOperand(2), ISD::SETOLE);
  case Intrinsic::mips_fclt_w:
  case Intrinsic::mips_fclt_d:
    return DAG.getSetCC(SDLoc(Op), Op->getValueType(0), Op->getOperand(1),
                        Op->getOperand(2), ISD::SETOLT);
  case Intrinsic::mips_fcne_w:
  case Intrinsic::mips_fcne_d:
    return DAG.getSetCC(SDLoc(Op), Op->getValueType(0), Op->getOperand(1),
                        Op->getOperand(2), ISD::SETONE);
  case Intrinsic::mips_fcor_w:
  case Intrinsic::mips_fcor_d:
    return DAG.getSetCC(SDLoc(Op), Op->getValueType(0), Op->getOperand(1),
                        Op->getOperand(2), ISD::SETO);
  case Intrinsic::mips_fcueq_w:
  case Intrinsic::mips_fcueq_d:
    return DAG.getSetCC(SDLoc(Op), Op->getValueType(0), Op->getOperand(1),
                        Op->getOperand(2), ISD::SETUEQ);
  case Intrinsic::mips_fcule_w:
  case Intrinsic::mips_fcule_d:
    return DAG.getSetCC(SDLoc(Op), Op->getValueType(0), Op->getOperand(1),
                        Op->getOperand(2), ISD::SETULE);
  case Intrinsic::mips_fcult_w:
  case Intrinsic::mips_fcult_d:
    return DAG.getSetCC(SDLoc(Op), Op->getValueType(0), Op->getOperand(1),
                        Op->getOperand(2), ISD::SETULT);
  case Intrinsic::mips_fcun_w:
  case Intrinsic::mips_fcun_d:
    return DAG.getSetCC(SDLoc(Op), Op->getValueType(0), Op->getOperand(1),
                        Op->getOperand(2), ISD::SETUO);
  case Intrinsic::mips_fcune_w:
  case Intrinsic::mips_fcune_d:
    return DAG.getSetCC(SDLoc(Op), Op->getValueType(0), Op->getOperand(1),
                        Op->getOperand(2), ISD::SETUNE);
  case Intrinsic::mips_fdiv_w:
  case Intrinsic::mips_fdiv_d:
    return lowerMSABinaryIntr(Op, DAG, ISD::FDIV);
  case Intrinsic::mips_fill_b:
  case Intrinsic::mips_fill_h:
  case Intrinsic::mips_fill_w: {
    SmallVector<SDValue, 16> Ops;
    EVT ResTy = Op->getValueType(0);

    for (unsigned i = 0; i < ResTy.getVectorNumElements(); ++i)
      Ops.push_back(Op->getOperand(1));

    return DAG.getNode(ISD::BUILD_VECTOR, SDLoc(Op), ResTy, &Ops[0],
                       Ops.size());
  }
  case Intrinsic::mips_flog2_w:
  case Intrinsic::mips_flog2_d:
    return lowerMSAUnaryIntr(Op, DAG, ISD::FLOG2);
  case Intrinsic::mips_fmul_w:
  case Intrinsic::mips_fmul_d:
    return lowerMSABinaryIntr(Op, DAG, ISD::FMUL);
  case Intrinsic::mips_frint_w:
  case Intrinsic::mips_frint_d:
    return lowerMSAUnaryIntr(Op, DAG, ISD::FRINT);
  case Intrinsic::mips_fsqrt_w:
  case Intrinsic::mips_fsqrt_d:
    return lowerMSAUnaryIntr(Op, DAG, ISD::FSQRT);
  case Intrinsic::mips_fsub_w:
  case Intrinsic::mips_fsub_d:
    return lowerMSABinaryIntr(Op, DAG, ISD::FSUB);
  case Intrinsic::mips_ilvev_b:
  case Intrinsic::mips_ilvev_h:
  case Intrinsic::mips_ilvev_w:
  case Intrinsic::mips_ilvev_d:
    return DAG.getNode(MipsISD::ILVEV, SDLoc(Op), Op->getValueType(0),
                       Op->getOperand(1), Op->getOperand(2));
  case Intrinsic::mips_ilvl_b:
  case Intrinsic::mips_ilvl_h:
  case Intrinsic::mips_ilvl_w:
  case Intrinsic::mips_ilvl_d:
    return DAG.getNode(MipsISD::ILVL, SDLoc(Op), Op->getValueType(0),
                       Op->getOperand(1), Op->getOperand(2));
  case Intrinsic::mips_ilvod_b:
  case Intrinsic::mips_ilvod_h:
  case Intrinsic::mips_ilvod_w:
  case Intrinsic::mips_ilvod_d:
    return DAG.getNode(MipsISD::ILVOD, SDLoc(Op), Op->getValueType(0),
                       Op->getOperand(1), Op->getOperand(2));
  case Intrinsic::mips_ilvr_b:
  case Intrinsic::mips_ilvr_h:
  case Intrinsic::mips_ilvr_w:
  case Intrinsic::mips_ilvr_d:
    return DAG.getNode(MipsISD::ILVR, SDLoc(Op), Op->getValueType(0),
                       Op->getOperand(1), Op->getOperand(2));
  case Intrinsic::mips_insert_b:
  case Intrinsic::mips_insert_h:
  case Intrinsic::mips_insert_w:
    return lowerMSAInsertIntr(Op, DAG, ISD::INSERT_VECTOR_ELT);
  case Intrinsic::mips_ldi_b:
  case Intrinsic::mips_ldi_h:
  case Intrinsic::mips_ldi_w:
  case Intrinsic::mips_ldi_d:
    return lowerMSASplatImm(Op, 1, DAG);
  case Intrinsic::mips_max_s_b:
  case Intrinsic::mips_max_s_h:
  case Intrinsic::mips_max_s_w:
  case Intrinsic::mips_max_s_d:
    return lowerMSABinaryIntr(Op, DAG, MipsISD::VSMAX);
  case Intrinsic::mips_max_u_b:
  case Intrinsic::mips_max_u_h:
  case Intrinsic::mips_max_u_w:
  case Intrinsic::mips_max_u_d:
    return lowerMSABinaryIntr(Op, DAG, MipsISD::VUMAX);
  case Intrinsic::mips_maxi_s_b:
  case Intrinsic::mips_maxi_s_h:
  case Intrinsic::mips_maxi_s_w:
  case Intrinsic::mips_maxi_s_d:
    return lowerMSABinaryImmIntr(Op, DAG, MipsISD::VSMAX,
                                 lowerMSASplatImm(Op, 2, DAG));
  case Intrinsic::mips_maxi_u_b:
  case Intrinsic::mips_maxi_u_h:
  case Intrinsic::mips_maxi_u_w:
  case Intrinsic::mips_maxi_u_d:
    return lowerMSABinaryImmIntr(Op, DAG, MipsISD::VUMAX,
                                 lowerMSASplatImm(Op, 2, DAG));
  case Intrinsic::mips_min_s_b:
  case Intrinsic::mips_min_s_h:
  case Intrinsic::mips_min_s_w:
  case Intrinsic::mips_min_s_d:
    return lowerMSABinaryIntr(Op, DAG, MipsISD::VSMIN);
  case Intrinsic::mips_min_u_b:
  case Intrinsic::mips_min_u_h:
  case Intrinsic::mips_min_u_w:
  case Intrinsic::mips_min_u_d:
    return lowerMSABinaryIntr(Op, DAG, MipsISD::VUMIN);
  case Intrinsic::mips_mini_s_b:
  case Intrinsic::mips_mini_s_h:
  case Intrinsic::mips_mini_s_w:
  case Intrinsic::mips_mini_s_d:
    return lowerMSABinaryImmIntr(Op, DAG, MipsISD::VSMIN,
                                 lowerMSASplatImm(Op, 2, DAG));
  case Intrinsic::mips_mini_u_b:
  case Intrinsic::mips_mini_u_h:
  case Intrinsic::mips_mini_u_w:
  case Intrinsic::mips_mini_u_d:
    return lowerMSABinaryImmIntr(Op, DAG, MipsISD::VUMIN,
                                 lowerMSASplatImm(Op, 2, DAG));
  case Intrinsic::mips_mulv_b:
  case Intrinsic::mips_mulv_h:
  case Intrinsic::mips_mulv_w:
  case Intrinsic::mips_mulv_d:
    return lowerMSABinaryIntr(Op, DAG, ISD::MUL);
  case Intrinsic::mips_nlzc_b:
  case Intrinsic::mips_nlzc_h:
  case Intrinsic::mips_nlzc_w:
  case Intrinsic::mips_nlzc_d:
    return lowerMSAUnaryIntr(Op, DAG, ISD::CTLZ);
  case Intrinsic::mips_nor_v: {
    SDValue Res = lowerMSABinaryIntr(Op, DAG, ISD::OR);
    return DAG.getNOT(SDLoc(Op), Res, Res->getValueType(0));
  }
  case Intrinsic::mips_nori_b: {
    SDValue Res = lowerMSABinaryImmIntr(Op, DAG, ISD::OR,
                                        lowerMSASplatImm(Op, 2, DAG));
    return DAG.getNOT(SDLoc(Op), Res, Res->getValueType(0));
  }
  case Intrinsic::mips_or_v:
    return lowerMSABinaryIntr(Op, DAG, ISD::OR);
  case Intrinsic::mips_ori_b:
    return lowerMSABinaryImmIntr(Op, DAG, ISD::OR,
                                 lowerMSASplatImm(Op, 2, DAG));
  case Intrinsic::mips_pckev_b:
  case Intrinsic::mips_pckev_h:
  case Intrinsic::mips_pckev_w:
  case Intrinsic::mips_pckev_d:
    return DAG.getNode(MipsISD::PCKEV, SDLoc(Op), Op->getValueType(0),
                       Op->getOperand(1), Op->getOperand(2));
  case Intrinsic::mips_pckod_b:
  case Intrinsic::mips_pckod_h:
  case Intrinsic::mips_pckod_w:
  case Intrinsic::mips_pckod_d:
    return DAG.getNode(MipsISD::PCKOD, SDLoc(Op), Op->getValueType(0),
                       Op->getOperand(1), Op->getOperand(2));
  case Intrinsic::mips_pcnt_b:
  case Intrinsic::mips_pcnt_h:
  case Intrinsic::mips_pcnt_w:
  case Intrinsic::mips_pcnt_d:
    return lowerMSAUnaryIntr(Op, DAG, ISD::CTPOP);
  case Intrinsic::mips_shf_b:
  case Intrinsic::mips_shf_h:
  case Intrinsic::mips_shf_w:
    return DAG.getNode(MipsISD::SHF, SDLoc(Op), Op->getValueType(0),
                       Op->getOperand(2), Op->getOperand(1));
  case Intrinsic::mips_sll_b:
  case Intrinsic::mips_sll_h:
  case Intrinsic::mips_sll_w:
  case Intrinsic::mips_sll_d:
    return lowerMSABinaryIntr(Op, DAG, ISD::SHL);
  case Intrinsic::mips_slli_b:
  case Intrinsic::mips_slli_h:
  case Intrinsic::mips_slli_w:
  case Intrinsic::mips_slli_d:
    return lowerMSABinaryImmIntr(Op, DAG, ISD::SHL,
                                 lowerMSASplatImm(Op, 2, DAG));
  case Intrinsic::mips_sra_b:
  case Intrinsic::mips_sra_h:
  case Intrinsic::mips_sra_w:
  case Intrinsic::mips_sra_d:
    return lowerMSABinaryIntr(Op, DAG, ISD::SRA);
  case Intrinsic::mips_srai_b:
  case Intrinsic::mips_srai_h:
  case Intrinsic::mips_srai_w:
  case Intrinsic::mips_srai_d:
    return lowerMSABinaryImmIntr(Op, DAG, ISD::SRA,
                                 lowerMSASplatImm(Op, 2, DAG));
  case Intrinsic::mips_srl_b:
  case Intrinsic::mips_srl_h:
  case Intrinsic::mips_srl_w:
  case Intrinsic::mips_srl_d:
    return lowerMSABinaryIntr(Op, DAG, ISD::SRL);
  case Intrinsic::mips_srli_b:
  case Intrinsic::mips_srli_h:
  case Intrinsic::mips_srli_w:
  case Intrinsic::mips_srli_d:
    return lowerMSABinaryImmIntr(Op, DAG, ISD::SRL,
                                 lowerMSASplatImm(Op, 2, DAG));
  case Intrinsic::mips_subv_b:
  case Intrinsic::mips_subv_h:
  case Intrinsic::mips_subv_w:
  case Intrinsic::mips_subv_d:
    return lowerMSABinaryIntr(Op, DAG, ISD::SUB);
  case Intrinsic::mips_subvi_b:
  case Intrinsic::mips_subvi_h:
  case Intrinsic::mips_subvi_w:
  case Intrinsic::mips_subvi_d:
    return lowerMSABinaryImmIntr(Op, DAG, ISD::SUB,
                                 lowerMSASplatImm(Op, 2, DAG));
  case Intrinsic::mips_vshf_b:
  case Intrinsic::mips_vshf_h:
  case Intrinsic::mips_vshf_w:
  case Intrinsic::mips_vshf_d:
    return DAG.getNode(MipsISD::VSHF, SDLoc(Op), Op->getValueType(0),
                       Op->getOperand(1), Op->getOperand(2), Op->getOperand(3));
  case Intrinsic::mips_xor_v:
    return lowerMSABinaryIntr(Op, DAG, ISD::XOR);
  case Intrinsic::mips_xori_b:
    return lowerMSABinaryImmIntr(Op, DAG, ISD::XOR,
                                 lowerMSASplatImm(Op, 2, DAG));
  }
}

static SDValue lowerMSALoadIntr(SDValue Op, SelectionDAG &DAG, unsigned Intr) {
  SDLoc DL(Op);
  SDValue ChainIn = Op->getOperand(0);
  SDValue Address = Op->getOperand(2);
  SDValue Offset  = Op->getOperand(3);
  EVT ResTy = Op->getValueType(0);
  EVT PtrTy = Address->getValueType(0);

  Address = DAG.getNode(ISD::ADD, DL, PtrTy, Address, Offset);

  return DAG.getLoad(ResTy, DL, ChainIn, Address, MachinePointerInfo(), false,
                     false, false, 16);
}

SDValue MipsSETargetLowering::lowerINTRINSIC_W_CHAIN(SDValue Op,
                                                     SelectionDAG &DAG) const {
  unsigned Intr = cast<ConstantSDNode>(Op->getOperand(1))->getZExtValue();
  switch (Intr) {
  default:
    return SDValue();
  case Intrinsic::mips_extp:
    return lowerDSPIntr(Op, DAG, MipsISD::EXTP);
  case Intrinsic::mips_extpdp:
    return lowerDSPIntr(Op, DAG, MipsISD::EXTPDP);
  case Intrinsic::mips_extr_w:
    return lowerDSPIntr(Op, DAG, MipsISD::EXTR_W);
  case Intrinsic::mips_extr_r_w:
    return lowerDSPIntr(Op, DAG, MipsISD::EXTR_R_W);
  case Intrinsic::mips_extr_rs_w:
    return lowerDSPIntr(Op, DAG, MipsISD::EXTR_RS_W);
  case Intrinsic::mips_extr_s_h:
    return lowerDSPIntr(Op, DAG, MipsISD::EXTR_S_H);
  case Intrinsic::mips_mthlip:
    return lowerDSPIntr(Op, DAG, MipsISD::MTHLIP);
  case Intrinsic::mips_mulsaq_s_w_ph:
    return lowerDSPIntr(Op, DAG, MipsISD::MULSAQ_S_W_PH);
  case Intrinsic::mips_maq_s_w_phl:
    return lowerDSPIntr(Op, DAG, MipsISD::MAQ_S_W_PHL);
  case Intrinsic::mips_maq_s_w_phr:
    return lowerDSPIntr(Op, DAG, MipsISD::MAQ_S_W_PHR);
  case Intrinsic::mips_maq_sa_w_phl:
    return lowerDSPIntr(Op, DAG, MipsISD::MAQ_SA_W_PHL);
  case Intrinsic::mips_maq_sa_w_phr:
    return lowerDSPIntr(Op, DAG, MipsISD::MAQ_SA_W_PHR);
  case Intrinsic::mips_dpaq_s_w_ph:
    return lowerDSPIntr(Op, DAG, MipsISD::DPAQ_S_W_PH);
  case Intrinsic::mips_dpsq_s_w_ph:
    return lowerDSPIntr(Op, DAG, MipsISD::DPSQ_S_W_PH);
  case Intrinsic::mips_dpaq_sa_l_w:
    return lowerDSPIntr(Op, DAG, MipsISD::DPAQ_SA_L_W);
  case Intrinsic::mips_dpsq_sa_l_w:
    return lowerDSPIntr(Op, DAG, MipsISD::DPSQ_SA_L_W);
  case Intrinsic::mips_dpaqx_s_w_ph:
    return lowerDSPIntr(Op, DAG, MipsISD::DPAQX_S_W_PH);
  case Intrinsic::mips_dpaqx_sa_w_ph:
    return lowerDSPIntr(Op, DAG, MipsISD::DPAQX_SA_W_PH);
  case Intrinsic::mips_dpsqx_s_w_ph:
    return lowerDSPIntr(Op, DAG, MipsISD::DPSQX_S_W_PH);
  case Intrinsic::mips_dpsqx_sa_w_ph:
    return lowerDSPIntr(Op, DAG, MipsISD::DPSQX_SA_W_PH);
  case Intrinsic::mips_ld_b:
  case Intrinsic::mips_ld_h:
  case Intrinsic::mips_ld_w:
  case Intrinsic::mips_ld_d:
  case Intrinsic::mips_ldx_b:
  case Intrinsic::mips_ldx_h:
  case Intrinsic::mips_ldx_w:
  case Intrinsic::mips_ldx_d:
   return lowerMSALoadIntr(Op, DAG, Intr);
  }
}

static SDValue lowerMSAStoreIntr(SDValue Op, SelectionDAG &DAG, unsigned Intr) {
  SDLoc DL(Op);
  SDValue ChainIn = Op->getOperand(0);
  SDValue Value   = Op->getOperand(2);
  SDValue Address = Op->getOperand(3);
  SDValue Offset  = Op->getOperand(4);
  EVT PtrTy = Address->getValueType(0);

  Address = DAG.getNode(ISD::ADD, DL, PtrTy, Address, Offset);

  return DAG.getStore(ChainIn, DL, Value, Address, MachinePointerInfo(), false,
                      false, 16);
}

SDValue MipsSETargetLowering::lowerINTRINSIC_VOID(SDValue Op,
                                                  SelectionDAG &DAG) const {
  unsigned Intr = cast<ConstantSDNode>(Op->getOperand(1))->getZExtValue();
  switch (Intr) {
  default:
    return SDValue();
  case Intrinsic::mips_st_b:
  case Intrinsic::mips_st_h:
  case Intrinsic::mips_st_w:
  case Intrinsic::mips_st_d:
  case Intrinsic::mips_stx_b:
  case Intrinsic::mips_stx_h:
  case Intrinsic::mips_stx_w:
  case Intrinsic::mips_stx_d:
    return lowerMSAStoreIntr(Op, DAG, Intr);
  }
}

/// \brief Check if the given BuildVectorSDNode is a splat.
/// This method currently relies on DAG nodes being reused when equivalent,
/// so it's possible for this to return false even when isConstantSplat returns
/// true.
static bool isSplatVector(const BuildVectorSDNode *N) {
  unsigned int nOps = N->getNumOperands();
  assert(nOps > 1 && "isSplat has 0 or 1 sized build vector");

  SDValue Operand0 = N->getOperand(0);

  for (unsigned int i = 1; i < nOps; ++i) {
    if (N->getOperand(i) != Operand0)
      return false;
  }

  return true;
}

// Lower ISD::EXTRACT_VECTOR_ELT into MipsISD::VEXTRACT_SEXT_ELT.
//
// The non-value bits resulting from ISD::EXTRACT_VECTOR_ELT are undefined. We
// choose to sign-extend but we could have equally chosen zero-extend. The
// DAGCombiner will fold any sign/zero extension of the ISD::EXTRACT_VECTOR_ELT
// result into this node later (possibly changing it to a zero-extend in the
// process).
SDValue MipsSETargetLowering::
lowerEXTRACT_VECTOR_ELT(SDValue Op, SelectionDAG &DAG) const {
  SDLoc DL(Op);
  EVT ResTy = Op->getValueType(0);
  SDValue Op0 = Op->getOperand(0);
  SDValue Op1 = Op->getOperand(1);
  EVT EltTy = Op0->getValueType(0).getVectorElementType();
  return DAG.getNode(MipsISD::VEXTRACT_SEXT_ELT, DL, ResTy, Op0, Op1,
                     DAG.getValueType(EltTy));
}

static bool isConstantOrUndef(const SDValue Op) {
  if (Op->getOpcode() == ISD::UNDEF)
    return true;
  if (dyn_cast<ConstantSDNode>(Op))
    return true;
  if (dyn_cast<ConstantFPSDNode>(Op))
    return true;
  return false;
}

static bool isConstantOrUndefBUILD_VECTOR(const BuildVectorSDNode *Op) {
  for (unsigned i = 0; i < Op->getNumOperands(); ++i)
    if (isConstantOrUndef(Op->getOperand(i)))
      return true;
  return false;
}

// Lowers ISD::BUILD_VECTOR into appropriate SelectionDAG nodes for the
// backend.
//
// Lowers according to the following rules:
// - Constant splats are legal as-is as long as the SplatBitSize is a power of
//   2 less than or equal to 64 and the value fits into a signed 10-bit
//   immediate
// - Constant splats are lowered to bitconverted BUILD_VECTORs if SplatBitSize
//   is a power of 2 less than or equal to 64 and the value does not fit into a
//   signed 10-bit immediate
// - Non-constant splats are legal as-is.
// - Non-constant non-splats are lowered to sequences of INSERT_VECTOR_ELT.
// - All others are illegal and must be expanded.
SDValue MipsSETargetLowering::lowerBUILD_VECTOR(SDValue Op,
                                                SelectionDAG &DAG) const {
  BuildVectorSDNode *Node = cast<BuildVectorSDNode>(Op);
  EVT ResTy = Op->getValueType(0);
  SDLoc DL(Op);
  APInt SplatValue, SplatUndef;
  unsigned SplatBitSize;
  bool HasAnyUndefs;

  if (!Subtarget->hasMSA() || !ResTy.is128BitVector())
    return SDValue();

  if (Node->isConstantSplat(SplatValue, SplatUndef, SplatBitSize,
                            HasAnyUndefs, 8,
                            !Subtarget->isLittle()) && SplatBitSize <= 64) {
    // We can only cope with 8, 16, 32, or 64-bit elements
    if (SplatBitSize != 8 && SplatBitSize != 16 && SplatBitSize != 32 &&
        SplatBitSize != 64)
      return SDValue();

    // If the value fits into a simm10 then we can use ldi.[bhwd]
    if (SplatValue.isSignedIntN(10))
      return Op;

    EVT ViaVecTy;

    switch (SplatBitSize) {
    default:
      return SDValue();
    case 8:
      ViaVecTy = MVT::v16i8;
      break;
    case 16:
      ViaVecTy = MVT::v8i16;
      break;
    case 32:
      ViaVecTy = MVT::v4i32;
      break;
    case 64:
      // There's no fill.d to fall back on for 64-bit values
      return SDValue();
    }

    SmallVector<SDValue, 16> Ops;
    SDValue Constant = DAG.getConstant(SplatValue.sextOrSelf(32), MVT::i32);

    for (unsigned i = 0; i < ViaVecTy.getVectorNumElements(); ++i)
      Ops.push_back(Constant);

    SDValue Result = DAG.getNode(ISD::BUILD_VECTOR, SDLoc(Node), ViaVecTy,
                                 &Ops[0], Ops.size());

    if (ViaVecTy != ResTy)
      Result = DAG.getNode(ISD::BITCAST, SDLoc(Node), ResTy, Result);

    return Result;
  } else if (isSplatVector(Node))
    return Op;
  else if (!isConstantOrUndefBUILD_VECTOR(Node)) {
    // Use INSERT_VECTOR_ELT operations rather than expand to stores.
    // The resulting code is the same length as the expansion, but it doesn't
    // use memory operations
    EVT ResTy = Node->getValueType(0);

    assert(ResTy.isVector());

    unsigned NumElts = ResTy.getVectorNumElements();
    SDValue Vector = DAG.getUNDEF(ResTy);
    for (unsigned i = 0; i < NumElts; ++i) {
      Vector = DAG.getNode(ISD::INSERT_VECTOR_ELT, DL, ResTy, Vector,
                           Node->getOperand(i),
                           DAG.getConstant(i, MVT::i32));
    }
    return Vector;
  }

  return SDValue();
}

// Lower VECTOR_SHUFFLE into SHF (if possible).
//
// SHF splits the vector into blocks of four elements, then shuffles these
// elements according to a <4 x i2> constant (encoded as an integer immediate).
//
// It is therefore possible to lower into SHF when the mask takes the form:
//   <a, b, c, d, a+4, b+4, c+4, d+4, a+8, b+8, c+8, d+8, ...>
// When undef's appear they are treated as if they were whatever value is
// necessary in order to fit the above form.
//
// For example:
//   %2 = shufflevector <8 x i16> %0, <8 x i16> undef,
//                      <8 x i32> <i32 3, i32 2, i32 1, i32 0,
//                                 i32 7, i32 6, i32 5, i32 4>
// is lowered to:
//   (SHF_H $w0, $w1, 27)
// where the 27 comes from:
//   3 + (2 << 2) + (1 << 4) + (0 << 6)
static SDValue lowerVECTOR_SHUFFLE_SHF(SDValue Op, EVT ResTy,
                                       SmallVector<int, 16> Indices,
                                       SelectionDAG &DAG) {
  int SHFIndices[4] = { -1, -1, -1, -1 };

  if (Indices.size() < 4)
    return SDValue();

  for (unsigned i = 0; i < 4; ++i) {
    for (unsigned j = i; j < Indices.size(); j += 4) {
      int Idx = Indices[j];

      // Convert from vector index to 4-element subvector index
      // If an index refers to an element outside of the subvector then give up
      if (Idx != -1) {
        Idx -= 4 * (j / 4);
        if (Idx < 0 || Idx >= 4)
          return SDValue();
      }

      // If the mask has an undef, replace it with the current index.
      // Note that it might still be undef if the current index is also undef
      if (SHFIndices[i] == -1)
        SHFIndices[i] = Idx;

      // Check that non-undef values are the same as in the mask. If they
      // aren't then give up
      if (!(Idx == -1 || Idx == SHFIndices[i]))
        return SDValue();
    }
  }

  // Calculate the immediate. Replace any remaining undefs with zero
  APInt Imm(32, 0);
  for (int i = 3; i >= 0; --i) {
    int Idx = SHFIndices[i];

    if (Idx == -1)
      Idx = 0;

    Imm <<= 2;
    Imm |= Idx & 0x3;
  }

  return DAG.getNode(MipsISD::SHF, SDLoc(Op), ResTy,
                     DAG.getConstant(Imm, MVT::i32), Op->getOperand(0));
}

// Lower VECTOR_SHUFFLE into ILVEV (if possible).
//
// ILVEV interleaves the even elements from each vector.
//
// It is possible to lower into ILVEV when the mask takes the form:
//   <0, n, 2, n+2, 4, n+4, ...>
// where n is the number of elements in the vector.
//
// When undef's appear in the mask they are treated as if they were whatever
// value is necessary in order to fit the above form.
static SDValue lowerVECTOR_SHUFFLE_ILVEV(SDValue Op, EVT ResTy,
                                         SmallVector<int, 16> Indices,
                                         SelectionDAG &DAG) {
  assert ((Indices.size() % 2) == 0);
  int WsIdx = 0;
  int WtIdx = ResTy.getVectorNumElements();

  for (unsigned i = 0; i < Indices.size(); i += 2) {
    if (Indices[i] != -1 && Indices[i] != WsIdx)
      return SDValue();
    if (Indices[i+1] != -1 && Indices[i+1] != WtIdx)
      return SDValue();
    WsIdx += 2;
    WtIdx += 2;
  }

  return DAG.getNode(MipsISD::ILVEV, SDLoc(Op), ResTy, Op->getOperand(0),
                     Op->getOperand(1));
}

// Lower VECTOR_SHUFFLE into ILVOD (if possible).
//
// ILVOD interleaves the odd elements from each vector.
//
// It is possible to lower into ILVOD when the mask takes the form:
//   <1, n+1, 3, n+3, 5, n+5, ...>
// where n is the number of elements in the vector.
//
// When undef's appear in the mask they are treated as if they were whatever
// value is necessary in order to fit the above form.
static SDValue lowerVECTOR_SHUFFLE_ILVOD(SDValue Op, EVT ResTy,
                                         SmallVector<int, 16> Indices,
                                         SelectionDAG &DAG) {
  assert ((Indices.size() % 2) == 0);
  int WsIdx = 1;
  int WtIdx = ResTy.getVectorNumElements() + 1;

  for (unsigned i = 0; i < Indices.size(); i += 2) {
    if (Indices[i] != -1 && Indices[i] != WsIdx)
      return SDValue();
    if (Indices[i+1] != -1 && Indices[i+1] != WtIdx)
      return SDValue();
    WsIdx += 2;
    WtIdx += 2;
  }

  return DAG.getNode(MipsISD::ILVOD, SDLoc(Op), ResTy, Op->getOperand(0),
                     Op->getOperand(1));
}

// Lower VECTOR_SHUFFLE into ILVL (if possible).
//
// ILVL interleaves consecutive elements from the left half of each vector.
//
// It is possible to lower into ILVL when the mask takes the form:
//   <0, n, 1, n+1, 2, n+2, ...>
// where n is the number of elements in the vector.
//
// When undef's appear in the mask they are treated as if they were whatever
// value is necessary in order to fit the above form.
static SDValue lowerVECTOR_SHUFFLE_ILVL(SDValue Op, EVT ResTy,
                                        SmallVector<int, 16> Indices,
                                        SelectionDAG &DAG) {
  assert ((Indices.size() % 2) == 0);
  int WsIdx = 0;
  int WtIdx = ResTy.getVectorNumElements();

  for (unsigned i = 0; i < Indices.size(); i += 2) {
    if (Indices[i] != -1 && Indices[i] != WsIdx)
      return SDValue();
    if (Indices[i+1] != -1 && Indices[i+1] != WtIdx)
      return SDValue();
    WsIdx ++;
    WtIdx ++;
  }

  return DAG.getNode(MipsISD::ILVL, SDLoc(Op), ResTy, Op->getOperand(0),
                     Op->getOperand(1));
}

// Lower VECTOR_SHUFFLE into ILVR (if possible).
//
// ILVR interleaves consecutive elements from the right half of each vector.
//
// It is possible to lower into ILVR when the mask takes the form:
//   <x, n+x, x+1, n+x+1, x+2, n+x+2, ...>
// where n is the number of elements in the vector and x is half n.
//
// When undef's appear in the mask they are treated as if they were whatever
// value is necessary in order to fit the above form.
static SDValue lowerVECTOR_SHUFFLE_ILVR(SDValue Op, EVT ResTy,
                                        SmallVector<int, 16> Indices,
                                        SelectionDAG &DAG) {
  assert ((Indices.size() % 2) == 0);
  unsigned NumElts = ResTy.getVectorNumElements();
  int WsIdx = NumElts / 2;
  int WtIdx = NumElts + NumElts / 2;

  for (unsigned i = 0; i < Indices.size(); i += 2) {
    if (Indices[i] != -1 && Indices[i] != WsIdx)
      return SDValue();
    if (Indices[i+1] != -1 && Indices[i+1] != WtIdx)
      return SDValue();
    WsIdx ++;
    WtIdx ++;
  }

  return DAG.getNode(MipsISD::ILVR, SDLoc(Op), ResTy, Op->getOperand(0),
                     Op->getOperand(1));
}

// Lower VECTOR_SHUFFLE into PCKEV (if possible).
//
// PCKEV copies the even elements of each vector into the result vector.
//
// It is possible to lower into PCKEV when the mask takes the form:
//   <0, 2, 4, ..., n, n+2, n+4, ...>
// where n is the number of elements in the vector.
//
// When undef's appear in the mask they are treated as if they were whatever
// value is necessary in order to fit the above form.
static SDValue lowerVECTOR_SHUFFLE_PCKEV(SDValue Op, EVT ResTy,
                                         SmallVector<int, 16> Indices,
                                         SelectionDAG &DAG) {
  assert ((Indices.size() % 2) == 0);
  int Idx = 0;

  for (unsigned i = 0; i < Indices.size(); ++i) {
    if (Indices[i] != -1 && Indices[i] != Idx)
      return SDValue();
    Idx += 2;
  }

  return DAG.getNode(MipsISD::PCKEV, SDLoc(Op), ResTy, Op->getOperand(0),
                     Op->getOperand(1));
}

// Lower VECTOR_SHUFFLE into PCKOD (if possible).
//
// PCKOD copies the odd elements of each vector into the result vector.
//
// It is possible to lower into PCKOD when the mask takes the form:
//   <1, 3, 5, ..., n+1, n+3, n+5, ...>
// where n is the number of elements in the vector.
//
// When undef's appear in the mask they are treated as if they were whatever
// value is necessary in order to fit the above form.
static SDValue lowerVECTOR_SHUFFLE_PCKOD(SDValue Op, EVT ResTy,
                                         SmallVector<int, 16> Indices,
                                         SelectionDAG &DAG) {
  assert ((Indices.size() % 2) == 0);
  int Idx = 1;

  for (unsigned i = 0; i < Indices.size(); ++i) {
    if (Indices[i] != -1 && Indices[i] != Idx)
      return SDValue();
    Idx += 2;
  }

  return DAG.getNode(MipsISD::PCKOD, SDLoc(Op), ResTy, Op->getOperand(0),
                     Op->getOperand(1));
}

// Lower VECTOR_SHUFFLE into VSHF.
//
// This mostly consists of converting the shuffle indices in Indices into a
// BUILD_VECTOR and adding it as an operand to the resulting VSHF. There is
// also code to eliminate unused operands of the VECTOR_SHUFFLE. For example,
// if the type is v8i16 and all the indices are less than 8 then the second
// operand is unused and can be replaced with anything. We choose to replace it
// with the used operand since this reduces the number of instructions overall.
static SDValue lowerVECTOR_SHUFFLE_VSHF(SDValue Op, EVT ResTy,
                                        SmallVector<int, 16> Indices,
                                        SelectionDAG &DAG) {
  SmallVector<SDValue, 16> Ops;
  SDValue Op0;
  SDValue Op1;
  EVT MaskVecTy = ResTy.changeVectorElementTypeToInteger();
  EVT MaskEltTy = MaskVecTy.getVectorElementType();
  bool Using1stVec = false;
  bool Using2ndVec = false;
  SDLoc DL(Op);
  int ResTyNumElts = ResTy.getVectorNumElements();

  for (int i = 0; i < ResTyNumElts; ++i) {
    // Idx == -1 means UNDEF
    int Idx = Indices[i];

    if (0 <= Idx && Idx < ResTyNumElts)
      Using1stVec = true;
    if (ResTyNumElts <= Idx && Idx < ResTyNumElts * 2)
      Using2ndVec = true;
  }

  for (SmallVector<int, 16>::iterator I = Indices.begin(); I != Indices.end();
       ++I)
    Ops.push_back(DAG.getTargetConstant(*I, MaskEltTy));

  SDValue MaskVec = DAG.getNode(ISD::BUILD_VECTOR, DL, MaskVecTy, &Ops[0],
                                Ops.size());

  if (Using1stVec && Using2ndVec) {
    Op0 = Op->getOperand(0);
    Op1 = Op->getOperand(1);
  } else if (Using1stVec)
    Op0 = Op1 = Op->getOperand(0);
  else if (Using2ndVec)
    Op0 = Op1 = Op->getOperand(1);
  else
    llvm_unreachable("shuffle vector mask references neither vector operand?");

  return DAG.getNode(MipsISD::VSHF, DL, ResTy, MaskVec, Op0, Op1);
}

// Lower VECTOR_SHUFFLE into one of a number of instructions depending on the
// indices in the shuffle.
SDValue MipsSETargetLowering::lowerVECTOR_SHUFFLE(SDValue Op,
                                                  SelectionDAG &DAG) const {
  ShuffleVectorSDNode *Node = cast<ShuffleVectorSDNode>(Op);
  EVT ResTy = Op->getValueType(0);

  if (!ResTy.is128BitVector())
    return SDValue();

  int ResTyNumElts = ResTy.getVectorNumElements();
  SmallVector<int, 16> Indices;

  for (int i = 0; i < ResTyNumElts; ++i)
    Indices.push_back(Node->getMaskElt(i));

  SDValue Result = lowerVECTOR_SHUFFLE_SHF(Op, ResTy, Indices, DAG);
  if (Result.getNode())
    return Result;
  Result = lowerVECTOR_SHUFFLE_ILVEV(Op, ResTy, Indices, DAG);
  if (Result.getNode())
    return Result;
  Result = lowerVECTOR_SHUFFLE_ILVOD(Op, ResTy, Indices, DAG);
  if (Result.getNode())
    return Result;
  Result = lowerVECTOR_SHUFFLE_ILVL(Op, ResTy, Indices, DAG);
  if (Result.getNode())
    return Result;
  Result = lowerVECTOR_SHUFFLE_ILVR(Op, ResTy, Indices, DAG);
  if (Result.getNode())
    return Result;
  Result = lowerVECTOR_SHUFFLE_PCKEV(Op, ResTy, Indices, DAG);
  if (Result.getNode())
    return Result;
  Result = lowerVECTOR_SHUFFLE_PCKOD(Op, ResTy, Indices, DAG);
  if (Result.getNode())
    return Result;
  return lowerVECTOR_SHUFFLE_VSHF(Op, ResTy, Indices, DAG);
}

MachineBasicBlock * MipsSETargetLowering::
emitBPOSGE32(MachineInstr *MI, MachineBasicBlock *BB) const{
  // $bb:
  //  bposge32_pseudo $vr0
  //  =>
  // $bb:
  //  bposge32 $tbb
  // $fbb:
  //  li $vr2, 0
  //  b $sink
  // $tbb:
  //  li $vr1, 1
  // $sink:
  //  $vr0 = phi($vr2, $fbb, $vr1, $tbb)

  MachineRegisterInfo &RegInfo = BB->getParent()->getRegInfo();
  const TargetInstrInfo *TII = getTargetMachine().getInstrInfo();
  const TargetRegisterClass *RC = &Mips::GPR32RegClass;
  DebugLoc DL = MI->getDebugLoc();
  const BasicBlock *LLVM_BB = BB->getBasicBlock();
  MachineFunction::iterator It = llvm::next(MachineFunction::iterator(BB));
  MachineFunction *F = BB->getParent();
  MachineBasicBlock *FBB = F->CreateMachineBasicBlock(LLVM_BB);
  MachineBasicBlock *TBB = F->CreateMachineBasicBlock(LLVM_BB);
  MachineBasicBlock *Sink  = F->CreateMachineBasicBlock(LLVM_BB);
  F->insert(It, FBB);
  F->insert(It, TBB);
  F->insert(It, Sink);

  // Transfer the remainder of BB and its successor edges to Sink.
  Sink->splice(Sink->begin(), BB, llvm::next(MachineBasicBlock::iterator(MI)),
               BB->end());
  Sink->transferSuccessorsAndUpdatePHIs(BB);

  // Add successors.
  BB->addSuccessor(FBB);
  BB->addSuccessor(TBB);
  FBB->addSuccessor(Sink);
  TBB->addSuccessor(Sink);

  // Insert the real bposge32 instruction to $BB.
  BuildMI(BB, DL, TII->get(Mips::BPOSGE32)).addMBB(TBB);

  // Fill $FBB.
  unsigned VR2 = RegInfo.createVirtualRegister(RC);
  BuildMI(*FBB, FBB->end(), DL, TII->get(Mips::ADDiu), VR2)
    .addReg(Mips::ZERO).addImm(0);
  BuildMI(*FBB, FBB->end(), DL, TII->get(Mips::B)).addMBB(Sink);

  // Fill $TBB.
  unsigned VR1 = RegInfo.createVirtualRegister(RC);
  BuildMI(*TBB, TBB->end(), DL, TII->get(Mips::ADDiu), VR1)
    .addReg(Mips::ZERO).addImm(1);

  // Insert phi function to $Sink.
  BuildMI(*Sink, Sink->begin(), DL, TII->get(Mips::PHI),
          MI->getOperand(0).getReg())
    .addReg(VR2).addMBB(FBB).addReg(VR1).addMBB(TBB);

  MI->eraseFromParent();   // The pseudo instruction is gone now.
  return Sink;
}

MachineBasicBlock * MipsSETargetLowering::
emitMSACBranchPseudo(MachineInstr *MI, MachineBasicBlock *BB,
                     unsigned BranchOp) const{
  // $bb:
  //  vany_nonzero $rd, $ws
  //  =>
  // $bb:
  //  bnz.b $ws, $tbb
  //  b $fbb
  // $fbb:
  //  li $rd1, 0
  //  b $sink
  // $tbb:
  //  li $rd2, 1
  // $sink:
  //  $rd = phi($rd1, $fbb, $rd2, $tbb)

  MachineRegisterInfo &RegInfo = BB->getParent()->getRegInfo();
  const TargetInstrInfo *TII = getTargetMachine().getInstrInfo();
  const TargetRegisterClass *RC = &Mips::GPR32RegClass;
  DebugLoc DL = MI->getDebugLoc();
  const BasicBlock *LLVM_BB = BB->getBasicBlock();
  MachineFunction::iterator It = llvm::next(MachineFunction::iterator(BB));
  MachineFunction *F = BB->getParent();
  MachineBasicBlock *FBB = F->CreateMachineBasicBlock(LLVM_BB);
  MachineBasicBlock *TBB = F->CreateMachineBasicBlock(LLVM_BB);
  MachineBasicBlock *Sink  = F->CreateMachineBasicBlock(LLVM_BB);
  F->insert(It, FBB);
  F->insert(It, TBB);
  F->insert(It, Sink);

  // Transfer the remainder of BB and its successor edges to Sink.
  Sink->splice(Sink->begin(), BB, llvm::next(MachineBasicBlock::iterator(MI)),
               BB->end());
  Sink->transferSuccessorsAndUpdatePHIs(BB);

  // Add successors.
  BB->addSuccessor(FBB);
  BB->addSuccessor(TBB);
  FBB->addSuccessor(Sink);
  TBB->addSuccessor(Sink);

  // Insert the real bnz.b instruction to $BB.
  BuildMI(BB, DL, TII->get(BranchOp))
    .addReg(MI->getOperand(1).getReg())
    .addMBB(TBB);

  // Fill $FBB.
  unsigned RD1 = RegInfo.createVirtualRegister(RC);
  BuildMI(*FBB, FBB->end(), DL, TII->get(Mips::ADDiu), RD1)
    .addReg(Mips::ZERO).addImm(0);
  BuildMI(*FBB, FBB->end(), DL, TII->get(Mips::B)).addMBB(Sink);

  // Fill $TBB.
  unsigned RD2 = RegInfo.createVirtualRegister(RC);
  BuildMI(*TBB, TBB->end(), DL, TII->get(Mips::ADDiu), RD2)
    .addReg(Mips::ZERO).addImm(1);

  // Insert phi function to $Sink.
  BuildMI(*Sink, Sink->begin(), DL, TII->get(Mips::PHI),
          MI->getOperand(0).getReg())
    .addReg(RD1).addMBB(FBB).addReg(RD2).addMBB(TBB);

  MI->eraseFromParent();   // The pseudo instruction is gone now.
  return Sink;
}
