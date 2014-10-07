//===- subzero/src/IceOperand.h - High-level operands -----------*- C++ -*-===//
//
//                        The Subzero Code Generator
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file declares the Operand class and its target-independent
// subclasses.  The main classes are Variable, which represents an
// LLVM variable that is either register- or stack-allocated, and the
// Constant hierarchy, which represents integer, floating-point,
// and/or symbolic constants.
//
//===----------------------------------------------------------------------===//

#ifndef SUBZERO_SRC_ICEOPERAND_H
#define SUBZERO_SRC_ICEOPERAND_H

#include "IceDefs.h"
#include "IceTypes.h"

namespace Ice {

class Operand {
public:
  static const size_t MaxTargetKinds = 10;
  enum OperandKind {
    kConst_Base,
    kConstInteger32,
    kConstInteger64,
    kConstFloat,
    kConstDouble,
    kConstRelocatable,
    kConstUndef,
    kConst_Target, // leave space for target-specific constant kinds
    kConst_Num = kConst_Target + MaxTargetKinds,
    kVariable,
    kVariable_Target, // leave space for target-specific variable kinds
    kVariable_Num = kVariable_Target + MaxTargetKinds,
    // Target-specific operand classes use kTarget as the starting
    // point for their Kind enum space.
    kTarget
  };
  OperandKind getKind() const { return Kind; }
  Type getType() const { return Ty; }

  // Every Operand keeps an array of the Variables referenced in
  // the operand.  This is so that the liveness operations can get
  // quick access to the variables of interest, without having to dig
  // so far into the operand.
  SizeT getNumVars() const { return NumVars; }
  Variable *getVar(SizeT I) const {
    assert(I < getNumVars());
    return Vars[I];
  }
  virtual void emit(const Cfg *Func) const = 0;
  // The dump(Func,Str) implementation must be sure to handle the
  // situation where Func==NULL.
  virtual void dump(const Cfg *Func, Ostream &Str) const = 0;
  void dump(const Cfg *Func) const {
    assert(Func);
    dump(Func, Func->getContext()->getStrDump());
  }
  void dump(Ostream &Str) const { dump(NULL, Str); }

  // Query whether this object was allocated in isolation, or added to
  // some higher-level pool.  This determines whether a containing
  // object's destructor should delete this object.  Generally,
  // constants are pooled globally, variables are pooled per-CFG, and
  // target-specific operands are not pooled.
  virtual bool isPooled() const { return false; }

  virtual ~Operand() {}

protected:
  Operand(OperandKind Kind, Type Ty)
      : Ty(Ty), Kind(Kind), NumVars(0), Vars(NULL) {}
  Operand(Operand &&O) = default;

  const Type Ty;
  const OperandKind Kind;
  // Vars and NumVars are initialized by the derived class.
  SizeT NumVars;
  Variable **Vars;

private:
  Operand(const Operand &) = delete;
  Operand &operator=(const Operand &) = delete;
};

template<class StreamType>
inline StreamType &operator<<(StreamType &Str, const Operand &Op) {
  Op.dump(Str);
  return Str;
}

// Constant is the abstract base class for constants.  All
// constants are allocated from a global arena and are pooled.
class Constant : public Operand {
public:
  uint32_t getPoolEntryID() const { return PoolEntryID; }
  using Operand::dump;
  void emit(const Cfg *Func) const override { emit(Func->getContext()); }
  virtual void emit(GlobalContext *Ctx) const = 0;
  void dump(const Cfg *Func, Ostream &Str) const = 0;

  static bool classof(const Operand *Operand) {
    OperandKind Kind = Operand->getKind();
    return Kind >= kConst_Base && Kind <= kConst_Num;
  }

protected:
  Constant(OperandKind Kind, Type Ty, uint32_t PoolEntryID)
      : Operand(Kind, Ty), PoolEntryID(PoolEntryID) {
    Vars = NULL;
    NumVars = 0;
  }
  ~Constant() override {}
  // PoolEntryID is an integer that uniquely identifies the constant
  // within its constant pool.  It is used for building the constant
  // pool in the object code and for referencing its entries.
  const uint32_t PoolEntryID;

private:
  Constant(const Constant &) = delete;
  Constant &operator=(const Constant &) = delete;
};

// ConstantPrimitive<> wraps a primitive type.
template <typename T, Operand::OperandKind K>
class ConstantPrimitive : public Constant {
public:
  static ConstantPrimitive *create(GlobalContext *Ctx, Type Ty, T Value,
                                   uint32_t PoolEntryID) {
    return new (Ctx->allocate<ConstantPrimitive>())
        ConstantPrimitive(Ty, Value, PoolEntryID);
  }
  T getValue() const { return Value; }
  using Constant::emit;
  // The target needs to implement this for each ConstantPrimitive
  // specialization.
  void emit(GlobalContext *Ctx) const override;
  using Constant::dump;
  void dump(const Cfg *, Ostream &Str) const override { Str << getValue(); }

  static bool classof(const Operand *Operand) {
    return Operand->getKind() == K;
  }

private:
  ConstantPrimitive(Type Ty, T Value, uint32_t PoolEntryID)
      : Constant(K, Ty, PoolEntryID), Value(Value) {}
  ConstantPrimitive(const ConstantPrimitive &) = delete;
  ConstantPrimitive &operator=(const ConstantPrimitive &) = delete;
  ~ConstantPrimitive() override {}
  const T Value;
};

typedef ConstantPrimitive<uint32_t, Operand::kConstInteger32> ConstantInteger32;
typedef ConstantPrimitive<uint64_t, Operand::kConstInteger64> ConstantInteger64;
typedef ConstantPrimitive<float, Operand::kConstFloat> ConstantFloat;
typedef ConstantPrimitive<double, Operand::kConstDouble> ConstantDouble;

template <> inline void ConstantInteger32::dump(const Cfg *, Ostream &Str) const {
  if (getType() == IceType_i1)
    Str << (getValue() ? "true" : "false");
  else
    Str << static_cast<int32_t>(getValue());
}

template <> inline void ConstantInteger64::dump(const Cfg *, Ostream &Str) const {
  assert(getType() == IceType_i64);
  Str << static_cast<int64_t>(getValue());
}

// RelocatableTuple bundles the parameters that are used to
// construct an ConstantRelocatable.  It is done this way so that
// ConstantRelocatable can fit into the global constant pool
// template mechanism.
class RelocatableTuple {
  RelocatableTuple &operator=(const RelocatableTuple &) = delete;

public:
  RelocatableTuple(const int64_t Offset, const IceString &Name,
                   bool SuppressMangling)
      : Offset(Offset), Name(Name), SuppressMangling(SuppressMangling) {}
  RelocatableTuple(const RelocatableTuple &Other)
      : Offset(Other.Offset), Name(Other.Name),
        SuppressMangling(Other.SuppressMangling) {}

  const int64_t Offset;
  const IceString Name;
  bool SuppressMangling;
};

bool operator<(const RelocatableTuple &A, const RelocatableTuple &B);

// ConstantRelocatable represents a symbolic constant combined with
// a fixed offset.
class ConstantRelocatable : public Constant {
public:
  static ConstantRelocatable *create(GlobalContext *Ctx, Type Ty,
                                     const RelocatableTuple &Tuple,
                                     uint32_t PoolEntryID) {
    return new (Ctx->allocate<ConstantRelocatable>()) ConstantRelocatable(
        Ty, Tuple.Offset, Tuple.Name, Tuple.SuppressMangling, PoolEntryID);
  }
  int64_t getOffset() const { return Offset; }
  IceString getName() const { return Name; }
  void setSuppressMangling(bool Value) { SuppressMangling = Value; }
  bool getSuppressMangling() const { return SuppressMangling; }
  using Constant::emit;
  using Constant::dump;
  void emit(GlobalContext *Ctx) const override;
  void dump(const Cfg *Func, Ostream &Str) const override;

  static bool classof(const Operand *Operand) {
    OperandKind Kind = Operand->getKind();
    return Kind == kConstRelocatable;
  }

private:
  ConstantRelocatable(Type Ty, int64_t Offset, const IceString &Name,
                      bool SuppressMangling, uint32_t PoolEntryID)
      : Constant(kConstRelocatable, Ty, PoolEntryID), Offset(Offset),
        Name(Name), SuppressMangling(SuppressMangling) {}
  ConstantRelocatable(const ConstantRelocatable &) = delete;
  ConstantRelocatable &operator=(const ConstantRelocatable &) = delete;
  ~ConstantRelocatable() override {}
  const int64_t Offset; // fixed offset to add
  const IceString Name; // optional for debug/dump
  bool SuppressMangling;
};

// ConstantUndef represents an unspecified bit pattern. Although it is
// legal to lower ConstantUndef to any value, backends should try to
// make code generation deterministic by lowering ConstantUndefs to 0.
class ConstantUndef : public Constant {
public:
  static ConstantUndef *create(GlobalContext *Ctx, Type Ty,
                               uint32_t PoolEntryID) {
    return new (Ctx->allocate<ConstantUndef>()) ConstantUndef(Ty, PoolEntryID);
  }

  using Constant::emit;
  using Constant::dump;
  // The target needs to implement this.
  void emit(GlobalContext *Ctx) const override;
  void dump(const Cfg *, Ostream &Str) const override { Str << "undef"; }

  static bool classof(const Operand *Operand) {
    return Operand->getKind() == kConstUndef;
  }

private:
  ConstantUndef(Type Ty, uint32_t PoolEntryID)
      : Constant(kConstUndef, Ty, PoolEntryID) {}
  ConstantUndef(const ConstantUndef &) = delete;
  ConstantUndef &operator=(const ConstantUndef &) = delete;
  ~ConstantUndef() override {}
};

// RegWeight is a wrapper for a uint32_t weight value, with a
// special value that represents infinite weight, and an addWeight()
// method that ensures that W+infinity=infinity.
class RegWeight {
public:
  RegWeight() : Weight(0) {}
  RegWeight(uint32_t Weight) : Weight(Weight) {}
  const static uint32_t Inf = ~0; // Force regalloc to give a register
  const static uint32_t Zero = 0; // Force regalloc NOT to give a register
  void addWeight(uint32_t Delta) {
    if (Delta == Inf)
      Weight = Inf;
    else if (Weight != Inf)
      Weight += Delta;
  }
  void addWeight(const RegWeight &Other) { addWeight(Other.Weight); }
  void setWeight(uint32_t Val) { Weight = Val; }
  uint32_t getWeight() const { return Weight; }
  bool isInf() const { return Weight == Inf; }

private:
  uint32_t Weight;
};
Ostream &operator<<(Ostream &Str, const RegWeight &W);
bool operator<(const RegWeight &A, const RegWeight &B);
bool operator<=(const RegWeight &A, const RegWeight &B);
bool operator==(const RegWeight &A, const RegWeight &B);

// LiveRange is a set of instruction number intervals representing
// a variable's live range.  Generally there is one interval per basic
// block where the variable is live, but adjacent intervals get
// coalesced into a single interval.  LiveRange also includes a
// weight, in case e.g. we want a live range to have higher weight
// inside a loop.
class LiveRange {
public:
  LiveRange() : Weight(0), IsNonpoints(false) {}

  void reset() {
    Range.clear();
    Weight.setWeight(0);
    untrim();
    IsNonpoints = false;
  }
  void addSegment(InstNumberT Start, InstNumberT End);

  bool endsBefore(const LiveRange &Other) const;
  bool overlaps(const LiveRange &Other, bool UseTrimmed = false) const;
  bool overlapsInst(InstNumberT OtherBegin, bool UseTrimmed = false) const;
  bool containsValue(InstNumberT Value) const;
  bool isEmpty() const { return Range.empty(); }
  bool isNonpoints() const { return IsNonpoints; }
  InstNumberT getStart() const {
    return Range.empty() ? -1 : Range.begin()->first;
  }

  void untrim() { TrimmedBegin = Range.begin(); }
  void trim(InstNumberT Lower);

  RegWeight getWeight() const { return Weight; }
  void setWeight(const RegWeight &NewWeight) { Weight = NewWeight; }
  void addWeight(uint32_t Delta) { Weight.addWeight(Delta); }
  void dump(Ostream &Str) const;

  // Defining USE_SET uses std::set to hold the segments instead of
  // std::list.  Using std::list will be slightly faster, but is more
  // restrictive because new segments cannot be added in the middle.

  //#define USE_SET

private:
  typedef std::pair<InstNumberT, InstNumberT> RangeElementType;
#ifdef USE_SET
  typedef std::set<RangeElementType> RangeType;
#else
  typedef std::list<RangeElementType> RangeType;
#endif
  RangeType Range;
  RegWeight Weight;
  // TrimmedBegin is an optimization for the overlaps() computation.
  // Since the linear-scan algorithm always calls it as overlaps(Cur)
  // and Cur advances monotonically according to live range start, we
  // can optimize overlaps() by ignoring all segments that end before
  // the start of Cur's range.  The linear-scan code enables this by
  // calling trim() on the ranges of interest as Cur advances.  Note
  // that linear-scan also has to initialize TrimmedBegin at the
  // beginning by calling untrim().
  RangeType::const_iterator TrimmedBegin;
  // IsNonpoints keeps track of whether the live range contains at
  // least one interval where Start!=End.  If it is empty or has the
  // form [x,x),[y,y),...,[z,z), then overlaps(InstNumberT) is
  // trivially false.
  bool IsNonpoints;
};

Ostream &operator<<(Ostream &Str, const LiveRange &L);

// Variable represents an operand that is register-allocated or
// stack-allocated.  If it is register-allocated, it will ultimately
// have a non-negative RegNum field.
class Variable : public Operand {
  Variable(const Variable &) = delete;
  Variable &operator=(const Variable &) = delete;
  Variable(Variable &&V) = default;

public:
  static Variable *create(Cfg *Func, Type Ty, SizeT Index,
                          const IceString &Name) {
    return new (Func->allocate<Variable>())
        Variable(kVariable, Ty, Index, Name);
  }

  SizeT getIndex() const { return Number; }
  IceString getName() const;
  void setName(IceString &NewName) {
    // Make sure that the name can only be set once.
    assert(Name.empty());
    Name = NewName;
  }

  bool getIsArg() const { return IsArgument; }
  void setIsArg(bool Val = true) { IsArgument = Val; }
  bool getIsImplicitArg() const { return IsImplicitArgument; }
  void setIsImplicitArg(bool Val = true) { IsImplicitArgument = Val; }

  int32_t getStackOffset() const { return StackOffset; }
  void setStackOffset(int32_t Offset) { StackOffset = Offset; }

  static const int32_t NoRegister = -1;
  bool hasReg() const { return getRegNum() != NoRegister; }
  int32_t getRegNum() const { return RegNum; }
  void setRegNum(int32_t NewRegNum) {
    // Regnum shouldn't be set more than once.
    assert(!hasReg() || RegNum == NewRegNum);
    RegNum = NewRegNum;
  }
  bool hasRegTmp() const { return getRegNumTmp() != NoRegister; }
  int32_t getRegNumTmp() const { return RegNumTmp; }
  void setRegNumTmp(int32_t NewRegNum) { RegNumTmp = NewRegNum; }

  RegWeight getWeight() const { return Weight; }
  void setWeight(uint32_t NewWeight) { Weight = NewWeight; }
  void setWeightInfinite() { Weight = RegWeight::Inf; }

  const LiveRange &getLiveRange() const { return Live; }
  void setLiveRange(const LiveRange &Range) { Live = Range; }
  void resetLiveRange() { Live.reset(); }
  void addLiveRange(InstNumberT Start, InstNumberT End, uint32_t WeightDelta) {
    assert(WeightDelta != RegWeight::Inf);
    Live.addSegment(Start, End);
    if (Weight.isInf())
      Live.setWeight(RegWeight::Inf);
    else
      Live.addWeight(WeightDelta * Weight.getWeight());
  }
  void setLiveRangeInfiniteWeight() { Live.setWeight(RegWeight::Inf); }
  void trimLiveRange(InstNumberT Start) { Live.trim(Start); }
  void untrimLiveRange() { Live.untrim(); }

  Variable *getLo() const { return LoVar; }
  Variable *getHi() const { return HiVar; }
  void setLoHi(Variable *Lo, Variable *Hi) {
    assert(LoVar == NULL);
    assert(HiVar == NULL);
    LoVar = Lo;
    HiVar = Hi;
  }
  // Creates a temporary copy of the variable with a different type.
  // Used primarily for syntactic correctness of textual assembly
  // emission.  Note that only basic information is copied, in
  // particular not DefInst, IsArgument, Weight, LoVar, HiVar,
  // VarsReal.
  Variable asType(Type Ty);

  void emit(const Cfg *Func) const override;
  using Operand::dump;
  void dump(const Cfg *Func, Ostream &Str) const override;

  static bool classof(const Operand *Operand) {
    OperandKind Kind = Operand->getKind();
    return Kind >= kVariable && Kind <= kVariable_Num;
  }

  // The destructor is public because of the asType() method.
  ~Variable() override {}

protected:
  Variable(OperandKind K, Type Ty, SizeT Index, const IceString &Name)
      : Operand(K, Ty), Number(Index), Name(Name), IsArgument(false),
        IsImplicitArgument(false), StackOffset(0), RegNum(NoRegister),
        RegNumTmp(NoRegister), Weight(1), LoVar(NULL), HiVar(NULL) {
    Vars = VarsReal;
    Vars[0] = this;
    NumVars = 1;
  }
  // Number is unique across all variables, and is used as a
  // (bit)vector index for liveness analysis.
  const SizeT Number;
  // Name is optional.
  IceString Name;
  bool IsArgument;
  bool IsImplicitArgument;
  // StackOffset is the canonical location on stack (only if
  // RegNum==NoRegister || IsArgument).
  int32_t StackOffset;
  // RegNum is the allocated register, or NoRegister if it isn't
  // register-allocated.
  int32_t RegNum;
  // RegNumTmp is the tentative assignment during register allocation.
  int32_t RegNumTmp;
  RegWeight Weight; // Register allocation priority
  LiveRange Live;
  // LoVar and HiVar are needed for lowering from 64 to 32 bits.  When
  // lowering from I64 to I32 on a 32-bit architecture, we split the
  // variable into two machine-size pieces.  LoVar is the low-order
  // machine-size portion, and HiVar is the remaining high-order
  // portion.  TODO: It's wasteful to penalize all variables on all
  // targets this way; use a sparser representation.  It's also
  // wasteful for a 64-bit target.
  Variable *LoVar;
  Variable *HiVar;
  // VarsReal (and Operand::Vars) are set up such that Vars[0] ==
  // this.
  Variable *VarsReal[1];
};

typedef std::vector<const Inst *> InstDefList;

// VariableTracking tracks the metadata for a single variable.  It is
// only meant to be used internally by VariablesMetadata.
class VariableTracking {
public:
  enum MultiDefState {
    // TODO(stichnot): Consider using just a simple counter.
    MDS_Unknown,
    MDS_SingleDef,
    MDS_MultiDefSingleBlock,
    MDS_MultiDefMultiBlock
  };
  enum MultiBlockState {
    MBS_Unknown,
    MBS_SingleBlock,
    MBS_MultiBlock
  };
  VariableTracking()
      : MultiDef(MDS_Unknown), MultiBlock(MBS_Unknown), SingleUseNode(NULL),
        SingleDefNode(NULL) {}
  MultiDefState getMultiDef() const { return MultiDef; }
  MultiBlockState getMultiBlock() const { return MultiBlock; }
  const Inst *getFirstDefinition() const;
  const Inst *getSingleDefinition() const;
  const InstDefList &getDefinitions() const { return Definitions; }
  const CfgNode *getNode() const { return SingleUseNode; }
  void markUse(const Inst *Instr, const CfgNode *Node, bool IsFromDef,
               bool IsImplicit);
  void markDef(const Inst *Instr, const CfgNode *Node);

private:
  VariableTracking &operator=(const VariableTracking &) = delete;
  MultiDefState MultiDef;
  MultiBlockState MultiBlock;
  const CfgNode *SingleUseNode;
  const CfgNode *SingleDefNode;
  // All definitions of the variable are collected here, in increasing
  // order of instruction number.
  InstDefList Definitions;
};

// VariablesMetadata analyzes and summarizes the metadata for the
// complete set of Variables.
class VariablesMetadata {
public:
  VariablesMetadata(const Cfg *Func) : Func(Func) {}
  // Initialize the state by traversing all instructions/variables in
  // the CFG.
  void init();
  // Returns whether the given Variable is tracked in this object.  It
  // should only return false if changes were made to the CFG after
  // running init(), in which case the state is stale and the results
  // shouldn't be trusted (but it may be OK e.g. for dumping).
  bool isTracked(const Variable *Var) const {
    return Var->getIndex() < Metadata.size();
  }

  // Returns whether the given Variable has multiple definitions.
  bool isMultiDef(const Variable *Var) const;
  // Returns the first definition instruction of the given Variable.
  // This is only valid for variables whose definitions are all within
  // the same block, e.g. T after the lowered sequence "T=B; T+=C;
  // A=T", for which getFirstDefinition(T) would return the "T=B"
  // instruction.  For variables with definitions span multiple
  // blocks, NULL is returned.
  const Inst *getFirstDefinition(const Variable *Var) const;
  // Returns the definition instruction of the given Variable, when
  // the variable has exactly one definition.  Otherwise, NULL is
  // returned.
  const Inst *getSingleDefinition(const Variable *Var) const;
  // Returns the list of all definition instructions of the given
  // Variable.
  const InstDefList &getDefinitions(const Variable *Var) const;

  // Returns whether the given Variable is live across multiple
  // blocks.  Mainly, this is used to partition Variables into
  // single-block versus multi-block sets for leveraging sparsity in
  // liveness analysis, and for implementing simple stack slot
  // coalescing.  As a special case, function arguments are always
  // considered multi-block because they are live coming into the
  // entry block.
  bool isMultiBlock(const Variable *Var) const;
  // Returns the node that the given Variable is used in, assuming
  // isMultiBlock() returns false.  Otherwise, NULL is returned.
  const CfgNode *getLocalUseNode(const Variable *Var) const;

private:
  const Cfg *Func;
  std::vector<VariableTracking> Metadata;
  const static InstDefList NoDefinitions;
  VariablesMetadata(const VariablesMetadata &) = delete;
  VariablesMetadata &operator=(const VariablesMetadata &) = delete;
};

} // end of namespace Ice

#endif // SUBZERO_SRC_ICEOPERAND_H
