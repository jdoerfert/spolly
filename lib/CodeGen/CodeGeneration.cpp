//===------ CodeGeneration.cpp - Code generate the Scops. -----------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// The CodeGeneration pass takes a Scop created by ScopInfo and translates it
// back to LLVM-IR using Cloog.
//
// The Scop describes the high level memory behaviour of a control flow region.
// Transformation passes can update the schedule (execution order) of statements
// in the Scop. Cloog is used to generate an abstract syntax tree (clast) that
// reflects the updated execution order. This clast is used to create new
// LLVM-IR that is computational equivalent to the original control flow region,
// but executes its code in the new execution order defined by the changed
// scattering.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "polly-codegen"

#include "polly/Cloog.h"
#include "polly/CodeGeneration.h"
#include "polly/Dependences.h"
#include "polly/LinkAllPasses.h"
#include "polly/ScopInfo.h"
#include "polly/TempScopInfo.h"
#include "polly/Support/GICHelper.h"
#include "polly/LoopGenerators.h"

#include "llvm/Module.h"

#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/PostOrderIterator.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolutionExpander.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/IRBuilder.h"

#include "llvm/Target/TargetData.h"

#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/FunctionUtils.h"
#include "llvm/Transforms/Utils/Cloning.h" 

#define CLOOG_INT_GMP 1
#include "cloog/cloog.h"
#include "cloog/isl/cloog.h"

#include "isl/aff.h"

#include <vector>
#include <utility>

using namespace polly;
using namespace llvm;

struct isl_set;

namespace polly {

bool EnablePollyVector;
bool EnablePollyOpenMP;
bool EnablePollyForkJoin;
bool EnablePollyForkJoinInline;
 
unsigned PollyForks;

static cl::opt<bool, true>
Vector("enable-polly-vector",
       cl::desc("Enable polly vector code generation"), cl::Hidden,
       cl::location(EnablePollyVector), cl::init(false), cl::ZeroOrMore);

static cl::opt<bool, true>
OpenMP("enable-polly-openmp",
       cl::desc("Generate OpenMP parallel code"), cl::Hidden,
       cl::location(EnablePollyOpenMP),
       cl::value_desc("OpenMP code generation enabled if true"),
       cl::init(false));

static cl::opt<bool>
AtLeastOnce("enable-polly-atLeastOnce",
       cl::desc("Give polly the hint, that every loop is executed at least"
                "once"), cl::Hidden,
       cl::value_desc("OpenMP code generation enabled if true"),
       cl::init(false), cl::ZeroOrMore);

static cl::opt<bool>
Aligned("enable-polly-aligned",
       cl::desc("Assumed aligned memory accesses."), cl::Hidden,
       cl::value_desc("OpenMP code generation enabled if true"),
       cl::init(false), cl::ZeroOrMore);

static cl::opt<bool>
GroupedUnrolling("enable-polly-grouped-unroll",
                 cl::desc("Perform grouped unrolling, but don't generate SIMD "
                          "instuctions"), cl::Hidden, cl::init(false),
                 cl::ZeroOrMore);

static cl::opt<bool, true>
ForkJoin("enable-polly-fork-join",
       cl::desc("Generate fork-join parallelizable code"), cl::Hidden,
       cl::location(EnablePollyForkJoin),
       cl::value_desc("This will unroll parallelizable outer loops"),
       cl::init(false));

static cl::opt<bool, true>
ForkJoinInline("polly-inline-forks",
       cl::desc(""), cl::Hidden,
       cl::location(EnablePollyForkJoinInline),
       cl::value_desc(""),
       cl::init(false));

static cl::opt<unsigned, true>
Forks("polly-forks",
       cl::desc("Set the number of forks"), cl::Hidden,
       cl::value_desc("How many iterations should be unrolled"),
       cl::location(PollyForks),
       cl::ValueRequired, cl::init(0));


typedef DenseMap<const Value*, Value*> ValueMapT;
typedef DenseMap<const char*, Value*> CharMapT;
typedef std::vector<ValueMapT> VectorValueMapT;

namespace {

  /// @brief Clones Master, ... 
  BasicBlock *cloneLinkAndUpdate(BasicBlock *Master, Value *From, Value *To,
                                 BasicBlock *ToLink, unsigned Successor,
                                 Function *F, DominatorTree &DT) {
    ValueToValueMapTy VMap;

    BasicBlock *Clone = CloneBasicBlock(Master, VMap, "", F, 0);

    ToLink->getTerminator()->setSuccessor(Successor, Clone);
    DT.addNewBlock(Clone, ToLink);

    for (BasicBlock::iterator BI = Clone->begin(), BE = Clone->end(); 
         BI != BE; BI++) {
      BI->replaceUsesOfWith(From, To);
    }

    return Clone;
  }

  void inlineSingleCall(BasicBlock *BB, Function *ExpectedFP) {
    for (BasicBlock::iterator BI = BB->begin(), BE = BB->end(); 
         BI != BE; BI++) {
      if (CallInst *CI = dyn_cast<CallInst>(BI)) {
         assert(CI->getCalledFunction() == ExpectedFP 
                && "Unexpected call in cloned block");
        
        InlineFunctionInfo IFI;
        bool InlineSuccess = InlineFunction(CI, IFI);
        assert(InlineSuccess && "Inlining failed in fork code generation");
        break;
      }
    }
  }

}


class IslGenerator {
public:
  IslGenerator(IRBuilder<> &Builder, std::vector<Value *> &IVS) :
    Builder(Builder), IVS(IVS) {}
  Value *generateIslInt(__isl_take isl_int Int);
  Value *generateIslAff(__isl_take isl_aff *Aff);
  Value *generateIslPwAff(__isl_take isl_pw_aff *PwAff);

private:
  typedef struct {
    Value *Result;
    class IslGenerator *Generator;
  } IslGenInfo;

  IRBuilder<> &Builder;
  std::vector<Value *> &IVS;
  static int mergeIslAffValues(__isl_take isl_set *Set,
                               __isl_take isl_aff *Aff, void *User);
};

Value *IslGenerator::generateIslInt(isl_int Int) {
  mpz_t IntMPZ;
  mpz_init(IntMPZ);
  isl_int_get_gmp(Int, IntMPZ);
  Value *IntValue = Builder.getInt(APInt_from_MPZ(IntMPZ));
  mpz_clear(IntMPZ);
  return IntValue;
}

Value *IslGenerator::generateIslAff(__isl_take isl_aff *Aff) {
  Value *Result;
  Value *ConstValue;
  isl_int ConstIsl;

  isl_int_init(ConstIsl);
  isl_aff_get_constant(Aff, &ConstIsl);
  ConstValue = generateIslInt(ConstIsl);
  Type *Ty = Builder.getInt64Ty();

  // FIXME: We should give the constant and coefficients the right type. Here
  // we force it into i64.
  Result = Builder.CreateSExtOrBitCast(ConstValue, Ty);

  unsigned int NbInputDims = isl_aff_dim(Aff, isl_dim_in);

  assert((IVS.size() == NbInputDims) && "The Dimension of Induction Variables"
         "must match the dimension of the affine space.");

  isl_int CoefficientIsl;
  isl_int_init(CoefficientIsl);

  for (unsigned int i = 0; i < NbInputDims; ++i) {
    Value *CoefficientValue;
    isl_aff_get_coefficient(Aff, isl_dim_in, i, &CoefficientIsl);

    if (isl_int_is_zero(CoefficientIsl))
      continue;

    CoefficientValue = generateIslInt(CoefficientIsl);
    CoefficientValue = Builder.CreateIntCast(CoefficientValue, Ty, true);
    Value *IV = Builder.CreateIntCast(IVS[i], Ty, true);
    Value *PAdd = Builder.CreateMul(CoefficientValue, IV, "p_mul_coeff");
    Result = Builder.CreateAdd(Result, PAdd, "p_sum_coeff");
  }

  isl_int_clear(CoefficientIsl);
  isl_int_clear(ConstIsl);
  isl_aff_free(Aff);

  return Result;
}

int IslGenerator::mergeIslAffValues(__isl_take isl_set *Set,
                                    __isl_take isl_aff *Aff, void *User) {
  IslGenInfo *GenInfo = (IslGenInfo *)User;

  assert((GenInfo->Result == NULL) && "Result is already set."
         "Currently only single isl_aff is supported");
  assert(isl_set_plain_is_universe(Set)
         && "Code generation failed because the set is not universe");

  GenInfo->Result = GenInfo->Generator->generateIslAff(Aff);

  isl_set_free(Set);
  return 0;
}

Value *IslGenerator::generateIslPwAff(__isl_take isl_pw_aff *PwAff) {
  IslGenInfo User;
  User.Result = NULL;
  User.Generator = this;
  isl_pw_aff_foreach_piece(PwAff, mergeIslAffValues, &User);
  assert(User.Result && "Code generation for isl_pw_aff failed");

  isl_pw_aff_free(PwAff);
  return User.Result;
}

/// @brief Generate a new basic block for a polyhedral statement.
///
/// The only public function exposed is generate().
class BlockGenerator {
public:
  /// @brief Generate a new BasicBlock for a ScopStmt.
  ///
  /// @param Builder   The LLVM-IR Builder used to generate the statement. The
  ///                  code is generated at the location, the Builder points to.
  /// @param Stmt      The statement to code generate.
  /// @param GlobalMap A map that defines for certain Values referenced from the
  ///                  original code new Values they should be replaced with.
  /// @param P         A reference to the pass this function is called from.
  ///                  The pass is needed to update other analysis.
  static void generate(IRBuilder<> &Builder, ScopStmt &Stmt,
                       ValueMapT &GlobalMap, Pass *P) {
    BlockGenerator Generator(Builder, Stmt, P);
    Generator.copyBB(GlobalMap);
  }

  /// @brief Generate a new BasicBlock for a ScopStmt.
  ///
  /// @param Builder   The LLVM-IR Builder used to generate the statement. The
  ///                  code is generated at the location, the Builder points to.
  /// @param Stmt      The statement to code generate.
  /// @param GlobalMap A map that defines for certain Values referenced from the
  ///                  original code new Values they should be replaced with.
  /// @param P         A reference to the pass this function is called from.
  ///                  The pass is needed to update other analysis.
  static void generateBlock(IRBuilder<> &Builder, ScopStmt &Stmt,
                            ValueMapT &GlobalMap, Pass *P) {
    BlockGenerator Generator(Builder, Stmt, P);

    BasicBlock *BB = Generator.Statement.getBasicBlock();
    BasicBlock *CopyBB = SplitBlock(Builder.GetInsertBlock(),
                                    Builder.GetInsertPoint(), P);
    CopyBB->setName("polly.stmt." + BB->getName());
    Builder.SetInsertPoint(CopyBB->begin());

    ValueMapT BBMap;

    // To generate code for finder grain statement, we need to remeber the
    // underlying instruction of the memory access.
    // typedef ScopStmt::memacc_iterator acc_it;
    // for (acc_it I = Stmt.memacc_begin(), E = Stmt.memacc_end(); I != E; ++I)
    //  Generate code for access I.

    typedef BasicBlock::const_iterator it;
    for (it II = BB->begin(), IE = BB->end(); II != IE; ++II) {
      const Instruction *Inst = II;
      if (MemoryAccess *MA = Stmt.lookupAccessFor(Inst))
        Generator.generateMemoryAccess(MA, Inst, GlobalMap, BBMap);
    }
  }


  void generateMemoryAccess(MemoryAccess *MA, const Instruction *Inst,
                            ValueMapT &GlobalMap, ValueMapT &BBMap) {
    isl_map *CurrentAccessRelation = MA->getAccessRelation();
    isl_map *NewAccessRelation = MA->getNewAccessRelation();

    assert(isl_map_has_equal_space(CurrentAccessRelation, NewAccessRelation)
           && "Current and new access function use different spaces");

    const Value *PointerOperand = MA->isRead() ?
                                  cast<LoadInst>(Inst)->getPointerOperand() :
                                  cast<StoreInst>(Inst)->getPointerOperand();

    // Do we need to generate the address from new access relation?
    if (NewAccessRelation) {
      Value *BaseAddress = const_cast<Value*>(MA->getBaseAddr());
      Value *NewPointer = getNewAccessOperand(NewAccessRelation, BaseAddress,
                                              BBMap, GlobalMap);
      BBMap[PointerOperand] = NewPointer;
    }

    cloneInstrTree(Inst, BBMap, GlobalMap);

    // Free the context.
    isl_map_free(CurrentAccessRelation);
    if (NewAccessRelation) {
      BBMap.erase(PointerOperand);
      isl_map_free(NewAccessRelation);
    }
  }

protected:
  IRBuilder<> &Builder;
  ScopStmt &Statement;
  Pass *P;

  BlockGenerator(IRBuilder<> &B, ScopStmt &Stmt, Pass *P);

  /// @brief Get the new version of a Value.
  ///
  /// @param Old       The old Value.
  /// @param BBMap     A mapping from old values to their new values
  ///                  (for values recalculated within this basic block).
  /// @param GlobalMap A mapping from old values to their new values
  ///                  (for values recalculated in the new ScoP, but not
  ///                   within this basic block).
  ///
  /// @returns  o The old value, if it is still valid.
  ///           o The new value, if available.
  ///           o NULL, if no value is found.
  Value *getNewValue(const Value *Old, ValueMapT &BBMap, ValueMapT &GlobalMap);

  void copyInstScalar(const Instruction *Inst, ValueMapT &BBMap,
                      ValueMapT &GlobalMap);

  /// @brief Get the memory access offset to be added to the base address
  std::vector<Value*> getMemoryAccessIndex(__isl_keep isl_map *AccessRelation,
                                           Value *BaseAddress, ValueMapT &BBMap,
                                           ValueMapT &GlobalMap);

  /// @brief Get the new operand address according to the changed access in
  ///        JSCOP file.
  Value *getNewAccessOperand(__isl_keep isl_map *NewAccessRelation,
                             Value *BaseAddress, ValueMapT &BBMap,
                             ValueMapT &GlobalMap);

  /// @brief Generate the operand address
  Value *generateLocationAccessed(const Instruction *Inst,
                                  const Value *Pointer, ValueMapT &BBMap,
                                  ValueMapT &GlobalMap);

  Value *generateScalarLoad(const LoadInst *load, ValueMapT &BBMap,
                            ValueMapT &GlobalMap);

  Value *generateScalarStore(const StoreInst *store, ValueMapT &BBMap,
                             ValueMapT &GlobalMap);

  /// @brief Copy a single Instruction.
  ///
  /// This copies a single Instruction and updates references to old values
  /// with references to new values, as defined by GlobalMap and BBMap.
  ///
  /// @param BBMap     A mapping from old values to their new values
  ///                  (for values recalculated within this basic block).
  /// @param GlobalMap A mapping from old values to their new values
  ///                  (for values recalculated in the new ScoP, but not
  ///                  within this basic block).
  void copyInstruction(const Instruction *Inst, ValueMapT &BBMap,
                       ValueMapT &GlobalMap);

  /// @brief Copy the basic block.
  ///
  /// This copies the entire basic block and updates references to old values
  /// with references to new values, as defined by GlobalMap.
  ///
  /// @param GlobalMap A mapping from old values to their new values
  ///                  (for values recalculated in the new ScoP, but not
  ///                  within this basic block).
  void copyBB(ValueMapT &GlobalMap);

  static Value *castOperandType(Value *NewOperand, Type *DstTy,
                                Instruction *InsertPos) {
    unsigned OldSize = DstTy->getScalarSizeInBits();
    unsigned NewSize = NewOperand->getType()->getScalarSizeInBits();
    // Insert a cast if types are different
    // FIXME: Use IRBuilder, so we have chance to fold the cast.
    if (OldSize < NewSize)
      return CastInst::CreateTruncOrBitCast(NewOperand, DstTy,
                                            NewOperand->getName() + "_c",
                                            InsertPos);

    return NewOperand;
  }

  /// @brief Clone the Instruction and its dependents recursively until the
  ///        instuction appears in the value map.
  ///
  /// @param Root The instruction need to be cloned.
  /// @param BB   The BasicBlock to hold the instruction.
  ///
  /// @return The newly cloned instruction.
  Instruction *cloneInstrTree(const Instruction *Root, ValueMapT &BBMap,
                              ValueMapT &GlobalMap, const Twine &NameSuffix="");
};

BlockGenerator::BlockGenerator(IRBuilder<> &B, ScopStmt &Stmt, Pass *P):
  Builder(B), Statement(Stmt), P(P) {}

Instruction *BlockGenerator::cloneInstrTree(const Instruction *Root,
                                            ValueMapT &BBMap,
                                            ValueMapT &GlobalMap,
                                            const Twine &NameSuffix) {
  // Clone the incoming instruction.
  Instruction *NewI = Root->clone();
  const BasicBlock *OldBB = Root->getParent();
  if (!NewI->getType()->isVoidTy()) NewI->setName(Root->getName() + NameSuffix);
  // Insert it to the current bb and remember we already copy this instruction.
  Builder.Insert(NewI);
  // Remember we cloned this instruction.
  BBMap[Root] = NewI;

  // Depth first traverse the operand tree (or operand dag, because we will
  // stop at PHINodes, so there are no cycle).
  typedef Instruction::op_iterator ChildIt;
  std::vector<std::pair<Instruction*, ChildIt> > WorkStack;

  WorkStack.push_back(std::make_pair(NewI, NewI->op_begin()));

  while (!WorkStack.empty()) {
    Instruction *CurInst = WorkStack.back().first;
    ChildIt It = WorkStack.back().second;
    DEBUG(dbgs() << "Checking Operand of Node:\n" << *CurInst << "\n------>\n");
    if (It == CurInst->op_end()) {
      DEBUG(dbgs() << "Going to insert new inst: " << *CurInst << '\n');
      // Insert the new instructions in topological order by insert the operand
      // tree before the root.
      // FIXME: Use IRBuilder?
      if (!CurInst->getParent())
        CurInst->insertBefore(NewI);

      WorkStack.pop_back();
    } else {
      // for each node N,
      Value *Operand = *It;
      ++WorkStack.back().second;

      DEBUG(dbgs() << "For Operand:\n" << *Operand << ' ' << Operand << "\n--->");

      if (Value *NewParam = GlobalMap.lookup(Operand)) {
        assert(Operand->getType() == NewParam->getType()
               && "The type of parameter not match!");
        DEBUG(dbgs() << "Replace parameter" << *Operand << " by "
                     << *NewParam << ".\n");
        It->set(NewParam);
        // If a value appeared in parameter map, it should not appeared in local
        // map
        assert(!BBMap.count(Operand) && "Parameter in local map!");
        continue;
      }

      Instruction *OpInst = dyn_cast<Instruction>(Operand);
      // Do not clone the constant or parameters not in the parameters map.
      if (OpInst == 0) continue;

      // Now we need to clone Operand to CurBB.
      // Check if we already clone it.
      if (Value *Cloned = BBMap.lookup(OpInst)) {
        DEBUG(dbgs() << "already cloned: " << *Cloned << ".\n");
        It->set(castOperandType(Cloned, OpInst->getType(), NewI));
        // Skip all its children as we already processed them.
        continue;
      }

      // Ignore the parameter not in the subsitution map.
      // FIXME: This is incorrect after IndependentBlocks pass is removed.
      if (OldBB != OpInst->getParent()) continue;

      // Else just clone the instruction.
      // Note that NewOp is not inserted in any BB now, we will insert it when
      // it popped form the work stack, so it will be inserted in topological
      // order.
      Instruction *NewOp = OpInst->clone();
      NewOp->setName("p_" + OpInst->getName());
      DEBUG(dbgs() << "Move to " << *NewOp << "\n");
      It->set(NewOp);
      // Insert it to the subsitiuation table.
      BBMap[OpInst] = NewOp;
      // Process its operands.
      WorkStack.push_back(std::make_pair(NewOp, NewOp->op_begin()));
    }
  }

  return NewI;
}

Value *BlockGenerator::getNewValue(const Value *Old, ValueMapT &BBMap,
                                   ValueMapT &GlobalMap) {
  dbgs() << "get value " << *Old << " " << GlobalMap.count(Old) << "\n";
  // We assume constants never change.
  // This avoids map lookups for many calls to this function.
  if (isa<Constant>(Old))
    return const_cast<Value*>(Old);

  if (GlobalMap.count(Old)) {
    Value *New = GlobalMap[Old];

    if (Old->getType()->getScalarSizeInBits()
        < New->getType()->getScalarSizeInBits())
      New = Builder.CreateTruncOrBitCast(New, Old->getType());

    return New;
  }

  if (BBMap.count(Old)) {
    return BBMap[Old];
  }

  // 'Old' is within the original SCoP, but was not rewritten.
  //
  // Such values appear, if they only calculate information already available in
  // the polyhedral description (e.g.  an induction variable increment). They
  // can be safely ignored.
  if (const Instruction *Inst = dyn_cast<Instruction>(Old))
    if (Statement.getParent()->getRegion().contains(Inst->getParent()))
      return NULL;

  // Everything else is probably a scop-constant value defined as global,
  // function parameter or an instruction not within the scop.
  return const_cast<Value*>(Old);
}

void BlockGenerator::copyInstScalar(const Instruction *Inst, ValueMapT &BBMap,
                                    ValueMapT &GlobalMap) {
  Instruction *NewInst = Inst->clone();

  // Replace old operands with the new ones.
  for (Instruction::const_op_iterator OI = Inst->op_begin(),
       OE = Inst->op_end(); OI != OE; ++OI) {
    Value *OldOperand = *OI;
    Value *NewOperand = getNewValue(OldOperand, BBMap, GlobalMap);

    if (!NewOperand) {
      assert(!isa<StoreInst>(NewInst)
             && "Store instructions are always needed!");
      delete NewInst;
      return;
    }

    NewInst->replaceUsesOfWith(OldOperand, NewOperand);
  }

  Builder.Insert(NewInst);
  BBMap[Inst] = NewInst;

  if (!NewInst->getType()->isVoidTy())
    NewInst->setName("p_" + Inst->getName());
}

std::vector<Value*> BlockGenerator::getMemoryAccessIndex(
  __isl_keep isl_map *AccessRelation, Value *BaseAddress,
  ValueMapT &BBMap, ValueMapT &GlobalMap) {

  assert((isl_map_dim(AccessRelation, isl_dim_out) == 1)
         && "Only single dimensional access functions supported");

  std::vector<Value *> IVS;
  for (unsigned i = 0; i < Statement.getNumIterators(); ++i) {
    const Value *OriginalIV = Statement.getInductionVariableForDimension(i);
    Value *NewIV = getNewValue(OriginalIV, BBMap, GlobalMap);
    IVS.push_back(NewIV);
  }

  isl_pw_aff *PwAff = isl_map_dim_max(isl_map_copy(AccessRelation), 0);
  IslGenerator IslGen(Builder, IVS);
  Value *OffsetValue = IslGen.generateIslPwAff(PwAff);

  Type *Ty = Builder.getInt64Ty();
  OffsetValue = Builder.CreateIntCast(OffsetValue, Ty, true);

  std::vector<Value*> IndexArray;
  Value *NullValue = Constant::getNullValue(Ty);
  IndexArray.push_back(NullValue);
  IndexArray.push_back(OffsetValue);
  return IndexArray;
}

Value *BlockGenerator::getNewAccessOperand(
  __isl_keep isl_map *NewAccessRelation, Value *BaseAddress,
  ValueMapT &BBMap, ValueMapT &GlobalMap) {
  std::vector<Value*> IndexArray = getMemoryAccessIndex(NewAccessRelation,
                                                        BaseAddress,
                                                        BBMap, GlobalMap);
  Value *NewOperand = Builder.CreateGEP(BaseAddress, IndexArray,
                                        "p_newarrayidx_");
  return NewOperand;
}

Value *BlockGenerator::generateLocationAccessed(const Instruction *Inst,
                                                const Value *Pointer,
                                                ValueMapT &BBMap,
                                                ValueMapT &GlobalMap) {
  MemoryAccess &Access = Statement.getAccessFor(Inst);
  isl_map *CurrentAccessRelation = Access.getAccessRelation();
  isl_map *NewAccessRelation = Access.getNewAccessRelation();

  assert(isl_map_has_equal_space(CurrentAccessRelation, NewAccessRelation)
         && "Current and new access function use different spaces");

  Value *NewPointer;

  if (!NewAccessRelation) {
    NewPointer = getNewValue(Pointer, BBMap, GlobalMap);
  } else {
    Value *BaseAddress = const_cast<Value*>(Access.getBaseAddr());
    NewPointer = getNewAccessOperand(NewAccessRelation, BaseAddress,
                                     BBMap, GlobalMap);
  }

  isl_map_free(CurrentAccessRelation);
  isl_map_free(NewAccessRelation);
  return NewPointer;
}

Value *BlockGenerator::generateScalarLoad(const LoadInst *Load,
                                          ValueMapT &BBMap,
                                          ValueMapT &GlobalMap) {
  const Value *Pointer = Load->getPointerOperand();
  const Instruction *Inst = dyn_cast<Instruction>(Load);
  Value *NewPointer = generateLocationAccessed(Inst, Pointer, BBMap, GlobalMap);
  Value *ScalarLoad = Builder.CreateLoad(NewPointer,
                                         Load->getName() + "_p_scalar_");
  return ScalarLoad;
}

Value *BlockGenerator::generateScalarStore(const StoreInst *Store,
                                           ValueMapT &BBMap,
                                           ValueMapT &GlobalMap) {
  const Value *Pointer = Store->getPointerOperand();
  Value *NewPointer = generateLocationAccessed(Store, Pointer, BBMap,
                                               GlobalMap);
  Value *ValueOperand = getNewValue(Store->getValueOperand(), BBMap, GlobalMap);

  return Builder.CreateStore(ValueOperand, NewPointer);
}

void BlockGenerator::copyInstruction(const Instruction *Inst,
                                     ValueMapT &BBMap, ValueMapT &GlobalMap) {
  // Terminator instructions control the control flow. They are explicitly
  // expressed in the clast and do not need to be copied.
  if (Inst->isTerminator())
    return;

  if (const LoadInst *Load = dyn_cast<LoadInst>(Inst)) {
    BBMap[Load] = generateScalarLoad(Load, BBMap, GlobalMap);
    return;
  }

  if (const StoreInst *Store = dyn_cast<StoreInst>(Inst)) {
    BBMap[Store] = generateScalarStore(Store, BBMap, GlobalMap);
    return;
  }

  copyInstScalar(Inst, BBMap, GlobalMap);
}


void BlockGenerator::copyBB(ValueMapT &GlobalMap) {
  BasicBlock *BB = Statement.getBasicBlock();
  BasicBlock *CopyBB = SplitBlock(Builder.GetInsertBlock(),
                                  Builder.GetInsertPoint(), P);
  CopyBB->setName("polly.stmt." + BB->getName());
  Builder.SetInsertPoint(CopyBB->begin());

  ValueMapT BBMap;

  for (BasicBlock::const_iterator II = BB->begin(), IE = BB->end(); II != IE;
       ++II)
      copyInstruction(II, BBMap, GlobalMap);
}

/// @brief Generate a new vector basic block for a polyhedral statement.
///
/// The only public function exposed is generate().
class VectorBlockGenerator : BlockGenerator {
public:
  /// @brief Generate a new vector basic block for a ScoPStmt.
  ///
  /// This code generation is similar to the normal, scalar code generation,
  /// except that each instruction is code generated for several vector lanes
  /// at a time. If possible instructions are issued as actual vector
  /// instructions, but e.g. for address calculation instructions we currently
  /// generate scalar instructions for each vector lane.
  ///
  /// @param Builder    The LLVM-IR Builder used to generate the statement. The
  ///                   code is generated at the location, the builder points
  ///                   to.
  /// @param Stmt       The statement to code generate.
  /// @param GlobalMaps A vector of maps that define for certain Values
  ///                   referenced from the original code new Values they should
  ///                   be replaced with. Each map in the vector of maps is
  ///                   used for one vector lane. The number of elements in the
  ///                   vector defines the width of the generated vector
  ///                   instructions.
  /// @param P          A reference to the pass this function is called from.
  ///                   The pass is needed to update other analysis.
  static void generate(IRBuilder<> &B, ScopStmt &Stmt,
                       VectorValueMapT &GlobalMaps, __isl_keep isl_set *Domain,
                       Pass *P) {
    VectorBlockGenerator Generator(B, GlobalMaps, Stmt, Domain, P);
    Generator.copyBB();
  }

private:
  // This is a vector of global value maps.  The first map is used for the first
  // vector lane, ...
  // Each map, contains information about Instructions in the old ScoP, which
  // are recalculated in the new SCoP. When copying the basic block, we replace
  // all referenes to the old instructions with their recalculated values.
  VectorValueMapT &GlobalMaps;

  isl_set *Domain;

  VectorBlockGenerator(IRBuilder<> &B, VectorValueMapT &GlobalMaps,
                       ScopStmt &Stmt, __isl_keep isl_set *Domain, Pass *P);

  int getVectorWidth();

  Value *getVectorValue(const Value *Old, ValueMapT &VectorMap,
                        VectorValueMapT &ScalarMaps);

  Type *getVectorPtrTy(const Value *V, int Width);

  /// @brief Load a vector from a set of adjacent scalars
  ///
  /// In case a set of scalars is known to be next to each other in memory,
  /// create a vector load that loads those scalars
  ///
  /// %vector_ptr= bitcast double* %p to <4 x double>*
  /// %vec_full = load <4 x double>* %vector_ptr
  ///
  Value *generateStrideOneLoad(const LoadInst *Load, ValueMapT &BBMap);

  /// @brief Load a vector initialized from a single scalar in memory
  ///
  /// In case all elements of a vector are initialized to the same
  /// scalar value, this value is loaded and shuffeled into all elements
  /// of the vector.
  ///
  /// %splat_one = load <1 x double>* %p
  /// %splat = shufflevector <1 x double> %splat_one, <1 x
  ///       double> %splat_one, <4 x i32> zeroinitializer
  ///
  Value *generateStrideZeroLoad(const LoadInst *Load, ValueMapT &BBMap);

  /// @Load a vector from scalars distributed in memory
  ///
  /// In case some scalars a distributed randomly in memory. Create a vector
  /// by loading each scalar and by inserting one after the other into the
  /// vector.
  ///
  /// %scalar_1= load double* %p_1
  /// %vec_1 = insertelement <2 x double> undef, double %scalar_1, i32 0
  /// %scalar 2 = load double* %p_2
  /// %vec_2 = insertelement <2 x double> %vec_1, double %scalar_1, i32 1
  ///
  Value *generateUnknownStrideLoad(const LoadInst *Load,
                                   VectorValueMapT &ScalarMaps);

  void generateLoad(const LoadInst *Load, ValueMapT &VectorMap,
                    VectorValueMapT &ScalarMaps);

  void copyUnaryInst(const UnaryInstruction *Inst, ValueMapT &VectorMap,
                     VectorValueMapT &ScalarMaps);

  void copyBinaryInst(const BinaryOperator *Inst, ValueMapT &VectorMap,
                      VectorValueMapT &ScalarMaps);

  void copyStore(const StoreInst *Store, ValueMapT &VectorMap,
                 VectorValueMapT &ScalarMaps);

  void copyInstScalarized(const Instruction *Inst, ValueMapT &VectorMap,
                          VectorValueMapT &ScalarMaps);

  bool extractScalarValues(const Instruction *Inst, ValueMapT &VectorMap,
                           VectorValueMapT &ScalarMaps);

  bool hasVectorOperands(const Instruction *Inst, ValueMapT &VectorMap);

  void copyInstruction(const Instruction *Inst, ValueMapT &VectorMap,
                       VectorValueMapT &ScalarMaps);

  void copyBB();
};

VectorBlockGenerator::VectorBlockGenerator(IRBuilder<> &B,
  VectorValueMapT &GlobalMaps, ScopStmt &Stmt, __isl_keep isl_set *Domain,
  Pass *P) : BlockGenerator(B, Stmt, P), GlobalMaps(GlobalMaps),
  Domain(Domain) {
    assert(GlobalMaps.size() > 1 && "Only one vector lane found");
    assert(Domain && "No statement domain provided");
  }

Value *VectorBlockGenerator::getVectorValue(const Value *Old,
                                            ValueMapT &VectorMap,
                                            VectorValueMapT &ScalarMaps) {
  if (VectorMap.count(Old))
    return VectorMap[Old];

  int Width = getVectorWidth();

  Value *Vector = UndefValue::get(VectorType::get(Old->getType(), Width));

  for (int Lane = 0; Lane < Width; Lane++)
    Vector = Builder.CreateInsertElement(Vector,
                                         getNewValue(Old,
                                                     ScalarMaps[Lane],
                                                     GlobalMaps[Lane]),
                                         Builder.getInt32(Lane));

  VectorMap[Old] = Vector;

  return Vector;
}

Type *VectorBlockGenerator::getVectorPtrTy(const Value *Val, int Width) {
  PointerType *PointerTy = dyn_cast<PointerType>(Val->getType());
  assert(PointerTy && "PointerType expected");

  Type *ScalarType = PointerTy->getElementType();
  VectorType *VectorType = VectorType::get(ScalarType, Width);

  return PointerType::getUnqual(VectorType);
}

Value *VectorBlockGenerator::generateStrideOneLoad(const LoadInst *Load,
                                                   ValueMapT &BBMap) {
  const Value *Pointer = Load->getPointerOperand();
  Type *VectorPtrType = getVectorPtrTy(Pointer, getVectorWidth());
  Value *NewPointer = getNewValue(Pointer, BBMap, GlobalMaps[0]);
  Value *VectorPtr = Builder.CreateBitCast(NewPointer, VectorPtrType,
                                           "vector_ptr");
  LoadInst *VecLoad = Builder.CreateLoad(VectorPtr,
                                         Load->getName() + "_p_vec_full");
  if (!Aligned)
    VecLoad->setAlignment(8);

  return VecLoad;
}

Value *VectorBlockGenerator::generateStrideZeroLoad(const LoadInst *Load,
                                                    ValueMapT &BBMap) {
  const Value *Pointer = Load->getPointerOperand();
  Type *VectorPtrType = getVectorPtrTy(Pointer, 1);
  Value *NewPointer = getNewValue(Pointer, BBMap, GlobalMaps[0]);
  Value *VectorPtr = Builder.CreateBitCast(NewPointer, VectorPtrType,
                                           Load->getName() + "_p_vec_p");
  LoadInst *ScalarLoad= Builder.CreateLoad(VectorPtr,
                                           Load->getName() + "_p_splat_one");

  if (!Aligned)
    ScalarLoad->setAlignment(8);

  Constant *SplatVector =
    Constant::getNullValue(VectorType::get(Builder.getInt32Ty(),
                                           getVectorWidth()));

  Value *VectorLoad = Builder.CreateShuffleVector(ScalarLoad, ScalarLoad,
                                                  SplatVector,
                                                  Load->getName()
                                                  + "_p_splat");
  return VectorLoad;
}

Value *VectorBlockGenerator::generateUnknownStrideLoad(const LoadInst *Load,
  VectorValueMapT &ScalarMaps) {
  int VectorWidth = getVectorWidth();
  const Value *Pointer = Load->getPointerOperand();
  VectorType *VectorType = VectorType::get(
    dyn_cast<PointerType>(Pointer->getType())->getElementType(), VectorWidth);

  Value *Vector = UndefValue::get(VectorType);

  for (int i = 0; i < VectorWidth; i++) {
    Value *NewPointer = getNewValue(Pointer, ScalarMaps[i], GlobalMaps[i]);
    Value *ScalarLoad = Builder.CreateLoad(NewPointer,
                                           Load->getName() + "_p_scalar_");
    Vector = Builder.CreateInsertElement(Vector, ScalarLoad,
                                         Builder.getInt32(i),
                                         Load->getName() + "_p_vec_");
  }

  return Vector;
}

void VectorBlockGenerator::generateLoad(const LoadInst *Load,
                                        ValueMapT &VectorMap,
                                        VectorValueMapT &ScalarMaps) {
  if (GroupedUnrolling || !VectorType::isValidElementType(Load->getType())) {
    for (int i = 0; i < getVectorWidth(); i++)
      ScalarMaps[i][Load] = generateScalarLoad(Load, ScalarMaps[i],
                                               GlobalMaps[i]);
    return;
  }

  MemoryAccess &Access = Statement.getAccessFor(Load);

  Value *NewLoad;
  if (Access.isStrideZero(isl_set_copy(Domain)))
    NewLoad = generateStrideZeroLoad(Load, ScalarMaps[0]);
  else if (Access.isStrideOne(isl_set_copy(Domain)))
    NewLoad = generateStrideOneLoad(Load, ScalarMaps[0]);
  else
    NewLoad = generateUnknownStrideLoad(Load, ScalarMaps);

  VectorMap[Load] = NewLoad;
}

void VectorBlockGenerator::copyUnaryInst(const UnaryInstruction *Inst,
                                         ValueMapT &VectorMap,
                                         VectorValueMapT &ScalarMaps) {
  int VectorWidth = getVectorWidth();
  Value *NewOperand = getVectorValue(Inst->getOperand(0), VectorMap,
                                     ScalarMaps);

  assert(isa<CastInst>(Inst) && "Can not generate vector code for instruction");

  const CastInst *Cast = dyn_cast<CastInst>(Inst);
  VectorType *DestType = VectorType::get(Inst->getType(), VectorWidth);
  VectorMap[Inst] = Builder.CreateCast(Cast->getOpcode(), NewOperand, DestType);
}

void VectorBlockGenerator::copyBinaryInst(const BinaryOperator *Inst,
                                          ValueMapT &VectorMap,
                                          VectorValueMapT &ScalarMaps) {
  Value *OpZero = Inst->getOperand(0);
  Value *OpOne = Inst->getOperand(1);

  Value *NewOpZero, *NewOpOne;
  NewOpZero = getVectorValue(OpZero, VectorMap, ScalarMaps);
  NewOpOne = getVectorValue(OpOne, VectorMap, ScalarMaps);

  Value *NewInst = Builder.CreateBinOp(Inst->getOpcode(), NewOpZero,
                                       NewOpOne,
                                       Inst->getName() + "p_vec");
  VectorMap[Inst] = NewInst;
}

void VectorBlockGenerator::copyStore(const StoreInst *Store,
                                     ValueMapT &VectorMap,
                                     VectorValueMapT &ScalarMaps) {
  int VectorWidth = getVectorWidth();

  MemoryAccess &Access = Statement.getAccessFor(Store);

  const Value *Pointer = Store->getPointerOperand();
  Value *Vector = getVectorValue(Store->getValueOperand(), VectorMap,
                                   ScalarMaps);

  if (Access.isStrideOne(isl_set_copy(Domain))) {
    Type *VectorPtrType = getVectorPtrTy(Pointer, VectorWidth);
    Value *NewPointer = getNewValue(Pointer, ScalarMaps[0], GlobalMaps[0]);

    Value *VectorPtr = Builder.CreateBitCast(NewPointer, VectorPtrType,
                                             "vector_ptr");
    StoreInst *Store = Builder.CreateStore(Vector, VectorPtr);

    if (!Aligned)
      Store->setAlignment(8);
  } else {
    for (unsigned i = 0; i < ScalarMaps.size(); i++) {
      Value *Scalar = Builder.CreateExtractElement(Vector,
                                                   Builder.getInt32(i));
      Value *NewPointer = getNewValue(Pointer, ScalarMaps[i], GlobalMaps[i]);
      Builder.CreateStore(Scalar, NewPointer);
    }
  }
}

bool VectorBlockGenerator::hasVectorOperands(const Instruction *Inst,
                                             ValueMapT &VectorMap) {
  for (Instruction::const_op_iterator OI = Inst->op_begin(),
       OE = Inst->op_end(); OI != OE; ++OI)
    if (VectorMap.count(*OI))
      return true;
  return false;
}

bool VectorBlockGenerator::extractScalarValues(const Instruction *Inst,
                                               ValueMapT &VectorMap,
                                               VectorValueMapT &ScalarMaps) {
  bool HasVectorOperand = false;
  int VectorWidth = getVectorWidth();

  for (Instruction::const_op_iterator OI = Inst->op_begin(),
       OE = Inst->op_end(); OI != OE; ++OI) {
    ValueMapT::iterator VecOp = VectorMap.find(*OI);

    if (VecOp == VectorMap.end())
      continue;

    HasVectorOperand = true;
    Value *NewVector = VecOp->second;

    for (int i = 0; i < VectorWidth; ++i) {
      ValueMapT &SM = ScalarMaps[i];

      // If there is one scalar extracted, all scalar elements should have
      // already been extracted by the code here. So no need to check for the
      // existance of all of them.
      if (SM.count(*OI))
        break;

      SM[*OI] = Builder.CreateExtractElement(NewVector, Builder.getInt32(i));
    }
  }

  return HasVectorOperand;
}

void VectorBlockGenerator::copyInstScalarized(const Instruction *Inst,
                                              ValueMapT &VectorMap,
                                              VectorValueMapT &ScalarMaps) {
  bool HasVectorOperand;
  int VectorWidth = getVectorWidth();

  HasVectorOperand = extractScalarValues(Inst, VectorMap, ScalarMaps);

  for (int VectorLane = 0; VectorLane < getVectorWidth(); VectorLane++)
    copyInstScalar(Inst, ScalarMaps[VectorLane], GlobalMaps[VectorLane]);

  if (!VectorType::isValidElementType(Inst->getType()) || !HasVectorOperand)
    return;

  // Make the result available as vector value.
  VectorType *VectorType = VectorType::get(Inst->getType(), VectorWidth);
  Value *Vector = UndefValue::get(VectorType);

  for (int i = 0; i < VectorWidth; i++)
    Vector = Builder.CreateInsertElement(Vector, ScalarMaps[i][Inst],
                                         Builder.getInt32(i));

  VectorMap[Inst] = Vector;
}

int VectorBlockGenerator::getVectorWidth() {
  return GlobalMaps.size();
}

void VectorBlockGenerator::copyInstruction(const Instruction *Inst,
                                           ValueMapT &VectorMap,
                                           VectorValueMapT &ScalarMaps) {
  // Terminator instructions control the control flow. They are explicitly
  // expressed in the clast and do not need to be copied.
  if (Inst->isTerminator())
    return;

  if (const LoadInst *Load = dyn_cast<LoadInst>(Inst)) {
    generateLoad(Load, VectorMap, ScalarMaps);
    return;
  }

  if (hasVectorOperands(Inst, VectorMap)) {
    if (const StoreInst *Store = dyn_cast<StoreInst>(Inst)) {
      copyStore(Store, VectorMap, ScalarMaps);
      return;
    }

    if (const UnaryInstruction *Unary = dyn_cast<UnaryInstruction>(Inst)) {
      copyUnaryInst(Unary, VectorMap, ScalarMaps);
      return;
    }

    if (const BinaryOperator *Binary = dyn_cast<BinaryOperator>(Inst)) {
      copyBinaryInst(Binary, VectorMap, ScalarMaps);
      return;
    }

    // Falltrough: We generate scalar instructions, if we don't know how to
    // generate vector code.
  }

  copyInstScalarized(Inst, VectorMap, ScalarMaps);
}

void VectorBlockGenerator::copyBB() {
  BasicBlock *BB = Statement.getBasicBlock();
  BasicBlock *CopyBB = SplitBlock(Builder.GetInsertBlock(),
                                  Builder.GetInsertPoint(), P);
  CopyBB->setName("polly.stmt." + BB->getName());
  Builder.SetInsertPoint(CopyBB->begin());

  // Create two maps that store the mapping from the original instructions of
  // the old basic block to their copies in the new basic block. Those maps
  // are basic block local.
  //
  // As vector code generation is supported there is one map for scalar values
  // and one for vector values.
  //
  // In case we just do scalar code generation, the vectorMap is not used and
  // the scalarMap has just one dimension, which contains the mapping.
  //
  // In case vector code generation is done, an instruction may either appear
  // in the vector map once (as it is calculating >vectorwidth< values at a
  // time. Or (if the values are calculated using scalar operations), it
  // appears once in every dimension of the scalarMap.
  VectorValueMapT ScalarBlockMap(getVectorWidth());
  ValueMapT VectorBlockMap;

  for (BasicBlock::const_iterator II = BB->begin(), IE = BB->end();
       II != IE; ++II)
      copyInstruction(II, VectorBlockMap, ScalarBlockMap);
}

/// Class to generate LLVM-IR that calculates the value of a clast_expr.
class ClastExpCodeGen {
  IRBuilder<> &Builder;
  const CharMapT &IVS;

  Value *codegen(const clast_name *e, Type *Ty);
  Value *codegen(const clast_term *e, Type *Ty);
  Value *codegen(const clast_binary *e, Type *Ty);
  Value *codegen(const clast_reduction *r, Type *Ty);
public:

  // A generator for clast expressions.
  //
  // @param B The IRBuilder that defines where the code to calculate the
  //          clast expressions should be inserted.
  // @param IVMAP A Map that translates strings describing the induction
  //              variables to the Values* that represent these variables
  //              on the LLVM side.
  ClastExpCodeGen(IRBuilder<> &B, CharMapT &IVMap);

  // Generates code to calculate a given clast expression.
  //
  // @param e The expression to calculate.
  // @return The Value that holds the result.
  Value *codegen(const clast_expr *e, Type *Ty);
};

Value *ClastExpCodeGen::codegen(const clast_name *e, Type *Ty) {
  CharMapT::const_iterator I = IVS.find(e->name);

  assert(I != IVS.end() && "Clast name not found");

  return Builder.CreateSExtOrBitCast(I->second, Ty);
}

Value *ClastExpCodeGen::codegen(const clast_term *e, Type *Ty) {
  APInt a = APInt_from_MPZ(e->val);

  Value *ConstOne = ConstantInt::get(Builder.getContext(), a);
  ConstOne = Builder.CreateSExtOrBitCast(ConstOne, Ty);

  if (!e->var)
    return ConstOne;

  Value *var = codegen(e->var, Ty);
  return Builder.CreateMul(ConstOne, var);
}

Value *ClastExpCodeGen::codegen(const clast_binary *e, Type *Ty) {
  Value *LHS = codegen(e->LHS, Ty);

  APInt RHS_AP = APInt_from_MPZ(e->RHS);

  Value *RHS = ConstantInt::get(Builder.getContext(), RHS_AP);
  RHS = Builder.CreateSExtOrBitCast(RHS, Ty);

  switch (e->type) {
  case clast_bin_mod:
    return Builder.CreateSRem(LHS, RHS);
  case clast_bin_fdiv:
    {
      // floord(n,d) ((n < 0) ? (n - d + 1) : n) / d
      Value *One = ConstantInt::get(Ty, 1);
      Value *Zero = ConstantInt::get(Ty, 0);
      Value *Sum1 = Builder.CreateSub(LHS, RHS);
      Value *Sum2 = Builder.CreateAdd(Sum1, One);
      Value *isNegative = Builder.CreateICmpSLT(LHS, Zero);
      Value *Dividend = Builder.CreateSelect(isNegative, Sum2, LHS);
      return Builder.CreateSDiv(Dividend, RHS);
    }
  case clast_bin_cdiv:
    {
      // ceild(n,d) ((n < 0) ? n : (n + d - 1)) / d
      Value *One = ConstantInt::get(Ty, 1);
      Value *Zero = ConstantInt::get(Ty, 0);
      Value *Sum1 = Builder.CreateAdd(LHS, RHS);
      Value *Sum2 = Builder.CreateSub(Sum1, One);
      Value *isNegative = Builder.CreateICmpSLT(LHS, Zero);
      Value *Dividend = Builder.CreateSelect(isNegative, LHS, Sum2);
      return Builder.CreateSDiv(Dividend, RHS);
    }
  case clast_bin_div:
    return Builder.CreateSDiv(LHS, RHS);
  };

  llvm_unreachable("Unknown clast binary expression type");
}

Value *ClastExpCodeGen::codegen(const clast_reduction *r, Type *Ty) {
  assert((   r->type == clast_red_min
             || r->type == clast_red_max
             || r->type == clast_red_sum)
         && "Clast reduction type not supported");
  Value *old = codegen(r->elts[0], Ty);

  for (int i=1; i < r->n; ++i) {
    Value *exprValue = codegen(r->elts[i], Ty);

    switch (r->type) {
    case clast_red_min:
      {
        Value *cmp = Builder.CreateICmpSLT(old, exprValue);
        old = Builder.CreateSelect(cmp, old, exprValue);
        break;
      }
    case clast_red_max:
      {
        Value *cmp = Builder.CreateICmpSGT(old, exprValue);
        old = Builder.CreateSelect(cmp, old, exprValue);
        break;
      }
    case clast_red_sum:
      old = Builder.CreateAdd(old, exprValue);
      break;
    }
  }

  return old;
}

ClastExpCodeGen::ClastExpCodeGen(IRBuilder<> &B, CharMapT &IVMap)
  : Builder(B), IVS(IVMap) {}

Value *ClastExpCodeGen::codegen(const clast_expr *e, Type *Ty) {
  switch(e->type) {
  case clast_expr_name:
    return codegen((const clast_name *)e, Ty);
  case clast_expr_term:
    return codegen((const clast_term *)e, Ty);
  case clast_expr_bin:
    return codegen((const clast_binary *)e, Ty);
  case clast_expr_red:
    return codegen((const clast_reduction *)e, Ty);
  }

  llvm_unreachable("Unknown clast expression!");
}

class ClastStmtCodeGen {
public:
  const std::vector<std::string> &getParallelLoops();

private:
  // The Scop we code generate.
  Scop *S;
  Pass *P;

  // The Builder specifies the current location to code generate at.
  IRBuilder<> &Builder;

  // Map the Values from the old code to their counterparts in the new code.
  ValueMapT ValueMap;

  // clastVars maps from the textual representation of a clast variable to its
  // current *Value. clast variables are scheduling variables, original
  // induction variables or parameters. They are used either in loop bounds or
  // to define the statement instance that is executed.
  //
  //   for (s = 0; s < n + 3; ++i)
  //     for (t = s; t < m; ++j)
  //       Stmt(i = s + 3 * m, j = t);
  //
  // {s,t,i,j,n,m} is the set of clast variables in this clast.
  CharMapT ClastVars;

  // Codegenerator for clast expressions.
  ClastExpCodeGen ExpGen;

  // Do we currently generate parallel code?
  bool parallelCodeGeneration;

  std::vector<std::string> parallelLoops;

  void codegen(const clast_assignment *a);

  void codegen(const clast_assignment *a, ScopStmt *Statement,
               unsigned Dimension, int vectorDim,
               std::vector<ValueMapT> *VectorVMap = 0);

  void codegenSubstitutions(const clast_stmt *Assignment,
                            ScopStmt *Statement, int vectorDim = 0,
                            std::vector<ValueMapT> *VectorVMap = 0);

  void codegen(const clast_user_stmt *u, std::vector<Value*> *IVS = NULL,
               const char *iterator = NULL, isl_set *scatteringDomain = 0);

  void codegen(const clast_block *b);

  /// @brief Create a classical sequential loop.
  void codegenForSequential(const clast_for *f);

  /// @brief Create OpenMP structure values.
  ///
  /// Create a list of values that has to be stored into the OpenMP subfuncition
  /// structure.
  SetVector<Value*> getOMPValues();

  /// @brief Update the internal structures according to a Value Map.
  ///
  /// @param VMap     A map from old to new values.
  /// @param Reverse  If true, we assume the update should be reversed.
  void updateWithValueMap(OMPGenerator::ValueToValueMapTy &VMap,
                          bool Reverse);

  /// @brief Create an OpenMP parallel for loop.
  ///
  /// This loop reflects a loop as if it would have been created by an OpenMP
  /// statement.
  void codegenForOpenMP(const clast_for *f);
  
  /// @brief Create an parallel loop for a later added fork join parallelism
  ///
  void codegenForForkJoinParallelism(const clast_for *f);

  /// @brief Create several loops for a later added parellel execution
  ///
  void codegenForChunks(const clast_for *f);

  bool isInnermostLoop(const clast_for *f);

  /// @brief Get the number of loop iterations for this loop.
  /// @param f The clast for loop to check.
  int getNumberOfIterations(const clast_for *f);

  /// @brief Create vector instructions for this loop.
  void codegenForVector(const clast_for *f);

  void codegen(const clast_for *f);

  Value *codegen(const clast_equation *eq);

  void codegen(const clast_guard *g);

  void codegen(const clast_stmt *stmt);

  void addParameters(const CloogNames *names);

  IntegerType *getIntPtrTy();

  public:
  void codegen(const clast_root *r);

  ClastStmtCodeGen(Scop *scop, IRBuilder<> &B, Pass *P);
};
}

IntegerType *ClastStmtCodeGen::getIntPtrTy() {
  return P->getAnalysis<TargetData>().getIntPtrType(Builder.getContext());
}

const std::vector<std::string> &ClastStmtCodeGen::getParallelLoops() {
  return parallelLoops;
}

void ClastStmtCodeGen::codegen(const clast_assignment *a) {
  Value *V= ExpGen.codegen(a->RHS, getIntPtrTy());
  ClastVars[a->LHS] = V;
}

void ClastStmtCodeGen::codegen(const clast_assignment *A, ScopStmt *Stmt,
                               unsigned Dim, int VectorDim,
                               std::vector<ValueMapT> *VectorVMap) {
  const PHINode *PN;
  Value *RHS;

  assert(!A->LHS && "Statement assignments do not have left hand side");

  PN = Stmt->getInductionVariableForDimension(Dim);
  RHS = ExpGen.codegen(A->RHS, Builder.getInt64Ty());
  RHS = Builder.CreateTruncOrBitCast(RHS, PN->getType());

  if (VectorVMap)
    (*VectorVMap)[VectorDim][PN] = RHS;

  ValueMap[PN] = RHS;
}

void ClastStmtCodeGen::codegenSubstitutions(const clast_stmt *Assignment,
                                             ScopStmt *Statement, int vectorDim,
  std::vector<ValueMapT> *VectorVMap) {
  int Dimension = 0;

  while (Assignment) {
    assert(CLAST_STMT_IS_A(Assignment, stmt_ass)
           && "Substitions are expected to be assignments");
    codegen((const clast_assignment *)Assignment, Statement, Dimension,
            vectorDim, VectorVMap);
    Assignment = Assignment->next;
    Dimension++;
  }
}

void ClastStmtCodeGen::codegen(const clast_user_stmt *u,
                               std::vector<Value*> *IVS , const char *iterator,
                               isl_set *Domain) {
  ScopStmt *Statement = (ScopStmt *)u->statement->usr;

  if (u->substitutions)
    codegenSubstitutions(u->substitutions, Statement);

  int VectorDimensions = IVS ? IVS->size() : 1;

  if (VectorDimensions == 1) {
    BlockGenerator::generateBlock(Builder, *Statement, ValueMap, P);
    return;
  }

  VectorValueMapT VectorMap(VectorDimensions);

  if (IVS) {
    assert (u->substitutions && "Substitutions expected!");
    int i = 0;
    for (std::vector<Value*>::iterator II = IVS->begin(), IE = IVS->end();
         II != IE; ++II) {
      ClastVars[iterator] = *II;
      codegenSubstitutions(u->substitutions, Statement, i, &VectorMap);
      i++;
    }
  }

  VectorBlockGenerator::generate(Builder, *Statement, VectorMap, Domain, P);
}

void ClastStmtCodeGen::codegen(const clast_block *b) {
  if (b->body)
    codegen(b->body);
}

void ClastStmtCodeGen::codegenForChunks(const clast_for *f) {
  //dbgs() << "Create chunk loops with " << Forks << " chunks \n\n";
  assert(Forks > 1 && "Use -spolly-forks=X to set the number of chunks > 1");

  Value *LowerBound, *UpperBound, *UpperBoundOrig, *IV, *Stride, *ChunkSize;
  BasicBlock *AfterBB;
  
  Type *IntPtrTy = getIntPtrTy();
  IntegerType *Int64Ty = Builder.getInt64Ty();

  LowerBound = ExpGen.codegen(f->LB, IntPtrTy);
  UpperBoundOrig = UpperBound = ExpGen.codegen(f->UB, IntPtrTy);
  
  // Adjust the stride (loop unrolling) 
  APInt APIStride = APInt_from_MPZ(f->stride).zext(64);
  Stride = Builder.getInt(APIStride);

  Value *One = ConstantInt::get(Int64Ty, 1, false);
  ChunkSize  = Builder.CreateSub(UpperBound, LowerBound, "bound_diff");
  ChunkSize  = Builder.CreateSDiv(ChunkSize, ConstantInt::get(Int64Ty, Forks, false));
  ChunkSize  = Builder.CreateSelect(Builder.CreateICmpSLE(ChunkSize, Stride), 
                                    Stride, ChunkSize);
  ChunkSize  = Builder.CreateSub(ChunkSize, One);

  unsigned chunk  = 0;
  UpperBound = Builder.CreateAdd(LowerBound, ChunkSize);
  UpperBound = Builder.CreateSelect(Builder.CreateICmpSLE(UpperBound, UpperBoundOrig),
                                      UpperBound, UpperBoundOrig);
  do {
     
    IV = createLoop(LowerBound, UpperBound, Stride, Builder, P, AfterBB);
    // Add loop iv to symbols.
    ClastVars[f->iterator] = IV;

    if (f->body)
      codegen(f->body);
    
    // Loop is finished, so remove its iv from the live symbols.
    ClastVars.erase(f->iterator);
    Builder.SetInsertPoint(AfterBB->begin());

    LowerBound = Builder.CreateAdd(LowerBound, ChunkSize);
    LowerBound = Builder.CreateAdd(LowerBound, One);
    UpperBound = Builder.CreateAdd(LowerBound, ChunkSize);
    UpperBound = Builder.CreateSelect(Builder.CreateICmpSLE(UpperBound, UpperBoundOrig),
                                      UpperBound, UpperBoundOrig);
  } while (++chunk < Forks);
  if (Instruction *I = dyn_cast<Instruction>(LowerBound)) I->removeFromParent();
  if (Instruction *I = dyn_cast<Instruction>(UpperBound)) I->removeFromParent();
 

}

/// @brief Unroll the loop N times to simplify fork-join parallelism
/// 
/// This function will not actually create parallel code, but it will
/// make it easy to do so. 
/// As the number of forks is given by N := Forks := -polly-forks=XX
/// may not divide the iterationcount (IC), the remaining iterations
/// are unrolled too and sheduled afterwards.
///
///  The created code will look like this:
///  ====================================
/// 
///                             |                                                
///  |--------->>------------ Header------------->>--------------|
///  |                          |                                |
///  |                      ForkSplit                            |
///  A                          |           Where every fork_X   |
///  |                        Fork_0        Block contains an    |
///  |                          |           Instruction to cal-  |
///  |                        Fork_...      culate the IV and    |
///  |                          |           a call to the loop   |
///  |                        Fork_N        body.                |
///  |                          |                                |
///  |---------<<-----------ForkJoin                            |
///                                                              |
///                                                              |
///                          Unroll_0------------<<--------------|
///    There are as much        |
///    Unroll_0 blocks       Unroll_... 
///    (similar to the          |
///     Fork_X blocks)       Unroll_M 
///    as iterations            |
///    remain:               AfterLoop
///    M = min(IC, IC % N)      |
///    However if the IC
///    is NOt constant, 
///    there will be a loop 
///    instead of the Unroll blocks.                  
///   
void ClastStmtCodeGen::codegenForForkJoinParallelism(const clast_for *f) {
  dbgs() << "Create fork join code with " << Forks << "Forks\n\n";
  assert(Forks > 1 && "Use -spolly-forks=X to set the number of forks > 1");

  Value *LowerBound, *UpperBound, *IV, *Stride;
  BasicBlock *AfterBB;
  BasicBlock *Preheader = Builder.GetInsertBlock();
  Function *F = Preheader->getParent();
  DominatorTree &DT = P->getAnalysis<DominatorTree>();
  
  bool Overflow;

  Type *IntPtrTy = getIntPtrTy();
  IntegerType *Int64Ty = Builder.getInt64Ty();
  LLVMContext &Context = IntPtrTy->getContext();

  LowerBound = ExpGen.codegen(f->LB, IntPtrTy);
  UpperBound = ExpGen.codegen(f->UB, IntPtrTy);
  
  // Adjust the stride (loop unrolling) 
  APInt APIStride = APInt_from_MPZ(f->stride).zext(64);
  APInt APIStrideForks = APIStride.smul_ov(APInt(64, Forks), Overflow);
  assert(!Overflow && "Got an Overflow during the IV computations");
  Stride = Builder.getInt(APIStrideForks);
 
  IV = createLoop(LowerBound, UpperBound, Stride, Builder, P, AfterBB);
   
  BasicBlock *Header = cast<Instruction>(IV)->getParent();

  // Copy the unprocessed Header here and delete it later if it is not needed
  ValueToValueMapTy VMap;
  BasicBlock *WhileHeader = CloneBasicBlock(Header, VMap, "", F, 0);

  // Add loop iv to symbols.
  ClastVars[f->iterator] = IV;

  // Look for the next IV and use this for the icmp (prevent to many executions)
  ICmpInst *ICmp = 0;
  BinaryOperator *NextIV = 0;
  for (Value::use_iterator UI = IV->use_begin(), UE = IV->use_end();
       UI != UE; ++UI) {
    if (isa<BinaryOperator>(*UI))
      NextIV = dyn_cast<BinaryOperator>(*UI);
    else if (isa<ICmpInst>(*UI)) 
      ICmp = dyn_cast<ICmpInst>(*UI);
    else
      assert(0 && "Unexpected use of the IV");
  }
  assert(ICmp && "IV ICmp instruction not found");
  assert(NextIV && "NextIV instruction not found");
  
  BasicBlock::iterator IP = Builder.GetInsertPoint();
  /// 
  Builder.SetInsertPoint(NextIV);
  std::vector<Value *> IVs;
  IVs.push_back(IV);
  for (unsigned u = 1; u < Forks; u++) {
    IVs.push_back(Builder.CreateAdd(IVs.back(), ConstantInt::get(Context, APIStride),
                                    IV->getName() + "_" + utostr(u)));
  }
  Value *NextIVMinusOne = IVs[Forks - 1];
  ICmp->replaceUsesOfWith(IV, NextIVMinusOne);
  ///
  Builder.SetInsertPoint(IP);


  BasicBlock *ForkSplit = Builder.GetInsertBlock();
  ForkSplit->setName("polly.fork_split");
  ForkSplit->getTerminator()->eraseFromParent();
  
  BasicBlock *Fork = BasicBlock::Create(Context, "polly.fork", F);
  DT.addNewBlock(Fork, ForkSplit); 
  BasicBlock *ForkJoin = BasicBlock::Create(Context, "polly.fork_join", F);
  DT.addNewBlock(ForkJoin, Fork); 

  Builder.SetInsertPoint(ForkSplit);

  Builder.CreateBr(Fork);
  
  assert(IV->getType() == Int64Ty && "Assumed IV is a 64 bit integer");

  Builder.SetInsertPoint(Fork);
  Builder.CreateBr(ForkJoin);

  Builder.SetInsertPoint(ForkJoin);
  Builder.CreateBr(Header);


  // Update Phi nodes in Header 
  //ForkSplit->replaceSuccessorsPhiUsesWith(ForkJoin); 
  for (BasicBlock::iterator I = Header->begin(), E = Header->end(); 
       I != E; I++) {
    if (PHINode *PHI = dyn_cast<PHINode>(I)) {
      int index  = PHI->getBasicBlockIndex(ForkSplit);
      PHI->setIncomingBlock(index, ForkJoin);
      continue;
    }
    break;
  }

  Builder.SetInsertPoint(Fork, Fork->begin());
  Fork->setName("polly.fork");

  BasicBlock *BeforeBody = SplitBlock(Fork, Builder.GetInsertPoint(), &DT);
  Builder.SetInsertPoint(BeforeBody, BeforeBody->begin());

  if (f->body)
    codegen(f->body);
 
  // Collect all Blocks created by codegen(f->body) 
  std::vector<BasicBlock *> Code;
  DenseSet<BasicBlock *> Blocks(16);
  DenseSet<BasicBlock *> ToDo(16);
  ToDo.insert(BeforeBody);


  while (!ToDo.empty()) {
    BasicBlock *current = *ToDo.begin();
    ToDo.erase(current);
    assert (!Blocks.count(current) && "ToDo Block already contained in Blocks");
    Code.push_back(current);
    Blocks.insert(current);

    for (succ_iterator I = succ_begin(current), E = succ_end(current); 
         I != E; I++) {
      BasicBlock *succ = *I;
      // Skip Blocks we've seen or which should not be extracted
      if (Blocks.count(succ) || ToDo.count(succ) || succ == ForkJoin) continue;

      ToDo.insert(succ); 
    }

  }


  // Extract the collected blocks into a function
  Function *ForkFn = ExtractCodeRegion(DT, Code, false);
  P->getAnalysis<polly::ScopDetection>().markFunctionAsInvalid(ForkFn);
  ForkFn->setName(F->getName() + "_fj_body");
   
  BasicBlock *ForkFnCallBlock = *succ_begin(Fork); 
  bool Success = MergeBlockIntoPredecessor(ForkFnCallBlock, P);
  assert(Success && "Merge ForkFnCallBlock failed");
  
  Fork->setName("polly.fork_0");

  std::vector<BasicBlock *> Clones;
  BasicBlock *Clone = Fork;
  unsigned NR = 1; 
  do {
    APInt APIStrideIteration = APIStride.smul_ov(APInt(64, NR), Overflow);
    assert(!Overflow && "Got an Overflow during the IV computations");

    Clone = cloneLinkAndUpdate(Fork, IV, IVs[NR], 
                               Clone, 0,  F, DT);
    Clone->setName("polly.fork_" + utostr(NR));

    Clones.push_back(Clone);
  } while (++NR < Forks);


  Clone->getTerminator()->setSuccessor(0, ForkJoin);
  ForkJoin->moveAfter(Clone);
  if (AfterBB != ForkJoin)
    AfterBB->moveAfter(ForkJoin);

  // TODO Create a new clast to allow Vectorization 

  unsigned SuccNr = GetSuccessorNumber(Header, AfterBB);

  ConstantInt *LB = dyn_cast<ConstantInt>(LowerBound);
  ConstantInt *UB = dyn_cast<ConstantInt>(UpperBound);

  if (LB && UB && (APIStrideForks != APInt(64, 0))) {
    WhileHeader->eraseFromParent();

    APInt APILB = LB->getValue();
    APInt APIUB = UB->getValue();
    assert(APILB.getBitWidth() == APIUB.getBitWidth() 
           && "Lower and upper bound differ in bit width");
    assert(APIStride.sgt(APInt(64,0)) && "Stride was negative ...");

    APInt Dif = APIUB - APILB;
    APInt Qot = Dif.sdiv(APIStrideForks);
    APInt Cur = APILB + Qot.smul_ov(APIStrideForks, Overflow) 
                + APIStrideForks - APInt(64,1);
    assert(!Overflow && "Got an Overflow during the IV computations");


    if (Cur.slt(APIUB)) {

      // To get the linking right
      Clone = Header;
      
      unsigned IC = 0;
      for (; Cur.sle(APIUB); Cur += APIStride) {
        
        Clone = cloneLinkAndUpdate(Fork, IV, IVs[IC], 
                                   Clone, SuccNr, F, DT);
        Clone->setName("polly.unroll_" + utostr(IC++));
        Clones.push_back(Clone);

        SuccNr = 0;
      }

      Clone->getTerminator()->setSuccessor(0, AfterBB);
      
    }
  } else {
    // LB or UB is not constant
    DT.addNewBlock(WhileHeader, Header);
    WhileHeader->setName("polly.unroll_header");
   
    unsigned SuccBodyNr = GetSuccessorNumber(Header, ForkSplit);

    Clone = cloneLinkAndUpdate(Fork, IV, VMap[IV], 
                               WhileHeader, SuccBodyNr, F, DT);
    Clone->setName("polly.unroll_body");
    Clones.push_back(Clone);

    Header->getTerminator()->setSuccessor(SuccNr, WhileHeader);
    Clone->getTerminator()->setSuccessor(0, WhileHeader);
    
    Instruction *NextIVClone = dyn_cast<Instruction>(VMap[NextIV]);

    for (BasicBlock::iterator BI = WhileHeader->begin(), BE = WhileHeader->end();
         BI != BE; BI++) {
      
      unsigned Operands = BI->getNumOperands();
      for (unsigned u = 0; u < Operands; u++) {
        Value *old = BI->getOperand(u);
        ValueToValueMapTy::iterator VI = VMap.find(old);
        if (VI != VMap.end()) 
          BI->setOperand(u, VI.base()->second);
      }

      if (PHINode *PHI = dyn_cast<PHINode>(BI)) {
        assert(PHI->getNumIncomingValues() == 2 
               && "Expected phi to have exactly two incoming nodes");

        if (PHI->getIncomingValue(0) == NextIVClone) {
          PHI->setIncomingBlock(0, Clone);
          PHI->setIncomingBlock(1, Header);
          PHI->setIncomingValue(1, IV);
        } else {
          assert(PHI->getIncomingValue(1) == NextIVClone
                 && "Mapped IV was not found in unroll header");
          PHI->setIncomingBlock(1, Clone);
          PHI->setIncomingBlock(0, Header);
          PHI->setIncomingValue(0, IV);
        }
      }
    }

    assert(NextIVClone && "Next IV clone was not found in unroll header");

    
    NextIVClone->replaceUsesOfWith(Stride, Stride = Builder.getInt(APIStride));

  }


  // After all blocks cloned Fork we inline the call there to
  if (EnablePollyForkJoinInline) { 
    inlineSingleCall(Fork, ForkFn);
    for (std::vector<BasicBlock *>::iterator cI = Clones.begin(), 
         cE = Clones.end(); cI != cE; cI++) 
      inlineSingleCall(*cI, ForkFn);
  }

  // Loop is finished, so remove its iv from the live symbols.
  ClastVars.erase(f->iterator);
  Builder.SetInsertPoint(AfterBB->begin());

}


void ClastStmtCodeGen::codegenForSequential(const clast_for *f) {
  Value *LowerBound, *UpperBound, *IV, *Stride;
  BasicBlock *AfterBB;
  Type *IntPtrTy = getIntPtrTy();

  LowerBound = ExpGen.codegen(f->LB, IntPtrTy);
  UpperBound = ExpGen.codegen(f->UB, IntPtrTy);
  Stride = Builder.getInt(APInt_from_MPZ(f->stride));

  IV = createLoop(LowerBound, UpperBound, Stride, Builder, P, AfterBB);

  // Add loop iv to symbols.
  ClastVars[f->iterator] = IV;

  if (f->body)
    codegen(f->body);

  // Loop is finished, so remove its iv from the live symbols.
  ClastVars.erase(f->iterator);
  Builder.SetInsertPoint(AfterBB->begin());
}

SetVector<Value*> ClastStmtCodeGen::getOMPValues() {
  SetVector<Value*> Values;

  // The clast variables
  for (CharMapT::iterator I = ClastVars.begin(), E = ClastVars.end();
       I != E; I++)
    Values.insert(I->second);

  // The memory reference base addresses
  for (Scop::iterator SI = S->begin(), SE = S->end(); SI != SE; ++SI) {
    ScopStmt *Stmt = *SI;
    for (SmallVector<MemoryAccess*, 8>::iterator I = Stmt->memacc_begin(),
         E = Stmt->memacc_end(); I != E; ++I) {
      Value *BaseAddr = const_cast<Value*>((*I)->getBaseAddr());
      Values.insert((BaseAddr));
    }
  }
      
  return Values;
}

void ClastStmtCodeGen::updateWithValueMap(OMPGenerator::ValueToValueMapTy &VMap,
                                          bool Reverse) {
  std::set<Value*> Inserted;

  if (Reverse) {
    OMPGenerator::ValueToValueMapTy ReverseMap;

    for (std::map<Value*, Value*>::iterator I = VMap.begin(), E = VMap.end();
         I != E; ++I)
       ReverseMap.insert(std::make_pair(I->second, I->first));

    for (CharMapT::iterator I = ClastVars.begin(), E = ClastVars.end();
         I != E; I++) {
      ClastVars[I->first] = ReverseMap[I->second];
      Inserted.insert(I->second);
    }

    /// FIXME: At the moment we do not reverse the update of the ValueMap.
    ///        This is incomplet, but the failure should be obvious, such that
    ///        we can fix this later.
    return;
  }

  for (CharMapT::iterator I = ClastVars.begin(), E = ClastVars.end();
       I != E; I++) {
    ClastVars[I->first] = VMap[I->second];
    Inserted.insert(I->second);
  }

  for (std::map<Value*, Value*>::iterator I = VMap.begin(), E = VMap.end();
       I != E; ++I) {
    if (Inserted.count(I->first))
      continue;

    ValueMap[I->first] = I->second;
  }
}

static void clearDomtree(Function *F, DominatorTree &DT) {
  DomTreeNode *N = DT.getNode(&F->getEntryBlock());
  std::vector<BasicBlock*> Nodes;
  for (po_iterator<DomTreeNode*> I = po_begin(N), E = po_end(N); I != E; ++I)
    Nodes.push_back(I->getBlock());

  for (std::vector<BasicBlock*>::iterator I = Nodes.begin(), E = Nodes.end();
       I != E; ++I)
    DT.eraseNode(*I);
}

void ClastStmtCodeGen::codegenForOpenMP(const clast_for *For) {
  Value *Stride, *LB, *UB, *IV;
  BasicBlock::iterator LoopBody;
  IntegerType *IntPtrTy = getIntPtrTy();
  SetVector<Value*> Values;
  OMPGenerator::ValueToValueMapTy VMap;
  OMPGenerator OMPGen(Builder, P);

  Stride = Builder.getInt(APInt_from_MPZ(For->stride));
  Stride = Builder.CreateSExtOrBitCast(Stride, IntPtrTy);
  LB = ExpGen.codegen(For->LB, IntPtrTy);
  UB = ExpGen.codegen(For->UB, IntPtrTy);

  Values = getOMPValues();

  IV = OMPGen.createParallelLoop(LB, UB, Stride, Values, VMap, &LoopBody);
  BasicBlock::iterator AfterLoop = Builder.GetInsertPoint();
  Builder.SetInsertPoint(LoopBody);

  updateWithValueMap(VMap, /* reverse */ false);
  ClastVars[For->iterator] = IV;

  if (For->body)
    codegen(For->body);

  ClastVars.erase(For->iterator);
  updateWithValueMap(VMap, /* reverse */ true);

  clearDomtree((*LoopBody).getParent()->getParent(),
               P->getAnalysis<DominatorTree>());

  Builder.SetInsertPoint(AfterLoop);
}

bool ClastStmtCodeGen::isInnermostLoop(const clast_for *f) {
  const clast_stmt *stmt = f->body;

  while (stmt) {
    if (!CLAST_STMT_IS_A(stmt, stmt_user))
      return false;

    stmt = stmt->next;
  }

  return true;
}

int ClastStmtCodeGen::getNumberOfIterations(const clast_for *f) {
  isl_set *loopDomain = isl_set_copy(isl_set_from_cloog_domain(f->domain));
  isl_set *tmp = isl_set_copy(loopDomain);

  // Calculate a map similar to the identity map, but with the last input
  // and output dimension not related.
  //  [i0, i1, i2, i3] -> [i0, i1, i2, o0]
  isl_space *Space = isl_set_get_space(loopDomain);
  Space = isl_space_drop_outputs(Space,
                                 isl_set_dim(loopDomain, isl_dim_set) - 2, 1);
  Space = isl_space_map_from_set(Space);
  isl_map *identity = isl_map_identity(Space);
  identity = isl_map_add_dims(identity, isl_dim_in, 1);
  identity = isl_map_add_dims(identity, isl_dim_out, 1);

  isl_map *map = isl_map_from_domain_and_range(tmp, loopDomain);
  map = isl_map_intersect(map, identity);

  isl_map *lexmax = isl_map_lexmax(isl_map_copy(map));
  isl_map *lexmin = isl_map_lexmin(map);
  isl_map *sub = isl_map_sum(lexmax, isl_map_neg(lexmin));

  isl_set *elements = isl_map_range(sub);

  if (!isl_set_is_singleton(elements)) {
    isl_set_free(elements);
    return -1;
  }

  isl_point *p = isl_set_sample_point(elements);

  isl_int v;
  isl_int_init(v);
  isl_point_get_coordinate(p, isl_dim_set, isl_set_n_dim(loopDomain) - 1, &v);
  int numberIterations = isl_int_get_si(v);
  isl_int_clear(v);
  isl_point_free(p);

  return (numberIterations) / isl_int_get_si(f->stride) + 1;
}

void ClastStmtCodeGen::codegenForVector(const clast_for *F) {
  DEBUG(dbgs() << "Vectorizing loop '" << F->iterator << "'\n";);
  int VectorWidth = getNumberOfIterations(F);

  Value *LB = ExpGen.codegen(F->LB, getIntPtrTy());

  APInt Stride = APInt_from_MPZ(F->stride);
  IntegerType *LoopIVType = dyn_cast<IntegerType>(LB->getType());
  Stride =  Stride.zext(LoopIVType->getBitWidth());
  Value *StrideValue = ConstantInt::get(LoopIVType, Stride);

  std::vector<Value*> IVS(VectorWidth);
  IVS[0] = LB;

  for (int i = 1; i < VectorWidth; i++)
    IVS[i] = Builder.CreateAdd(IVS[i-1], StrideValue, "p_vector_iv");

  isl_set *Domain = isl_set_from_cloog_domain(F->domain);

  // Add loop iv to symbols.
  ClastVars[F->iterator] = LB;

  const clast_stmt *Stmt = F->body;

  while (Stmt) {
    codegen((const clast_user_stmt *)Stmt, &IVS, F->iterator,
            isl_set_copy(Domain));
    Stmt = Stmt->next;
  }

  // Loop is finished, so remove its iv from the live symbols.
  isl_set_free(Domain);
  ClastVars.erase(F->iterator);
}

void ClastStmtCodeGen::codegen(const clast_for *f) {
  bool enabled = (Vector || OpenMP || ForkJoin);
  bool isParallelFor = P->getAnalysis<Dependences>().isParallelFor(f);
  if ( enabled && isParallelFor ) {
    
    int ni = getNumberOfIterations(f);
    
    if (Vector && isInnermostLoop(f) && (1 < ni && ni <= 16)) {
      codegenForVector(f);
      DEBUG(dbgs() << "\t Call codegen for Vector \n");
      return;
    }

    if (!parallelCodeGeneration) {
      if (SPOLLY_CHUNKS) {
        DEBUG(dbgs() << "\t Call codegen for Cunks \n");
        parallelCodeGeneration = true;
        parallelLoops.push_back(f->iterator);
        codegenForChunks(f);
        parallelCodeGeneration = false;
        return;
      } else if (OpenMP) {
        DEBUG(dbgs() << "\t Call codegen for OpenMP \n");
        parallelCodeGeneration = true;
        parallelLoops.push_back(f->iterator);
        codegenForOpenMP(f);
        parallelCodeGeneration = false;
        return;
      } else if (ForkJoin) {
        DEBUG(dbgs() << "\t Call codegen for ForkJoin \n");
        parallelCodeGeneration = true;
        parallelLoops.push_back(f->iterator);
        codegenForForkJoinParallelism(f);
        parallelCodeGeneration = false;
        return;
      }
    }
  }

  codegenForSequential(f);
}

Value *ClastStmtCodeGen::codegen(const clast_equation *eq) {
  Value *LHS = ExpGen.codegen(eq->LHS, getIntPtrTy());
  Value *RHS = ExpGen.codegen(eq->RHS, getIntPtrTy());
  CmpInst::Predicate P;

  if (eq->sign == 0)
    P = ICmpInst::ICMP_EQ;
  else if (eq->sign > 0)
    P = ICmpInst::ICMP_SGE;
  else
    P = ICmpInst::ICMP_SLE;

  return Builder.CreateICmp(P, LHS, RHS);
}

void ClastStmtCodeGen::codegen(const clast_guard *g) {
  Function *F = Builder.GetInsertBlock()->getParent();
  LLVMContext &Context = F->getContext();

  BasicBlock *CondBB = SplitBlock(Builder.GetInsertBlock(),
                                      Builder.GetInsertPoint(), P);
  CondBB->setName("polly.cond");
  BasicBlock *MergeBB = SplitBlock(CondBB, CondBB->begin(), P);
  MergeBB->setName("polly.merge");
  BasicBlock *ThenBB = BasicBlock::Create(Context, "polly.then", F);

  DominatorTree &DT = P->getAnalysis<DominatorTree>();
  DT.addNewBlock(ThenBB, CondBB);
  DT.changeImmediateDominator(MergeBB, CondBB);

  CondBB->getTerminator()->eraseFromParent();

  Builder.SetInsertPoint(CondBB);

  Value *Predicate = codegen(&(g->eq[0]));

  for (int i = 1; i < g->n; ++i) {
    Value *TmpPredicate = codegen(&(g->eq[i]));
    Predicate = Builder.CreateAnd(Predicate, TmpPredicate);
  }

  Builder.CreateCondBr(Predicate, ThenBB, MergeBB);
  Builder.SetInsertPoint(ThenBB);
  Builder.CreateBr(MergeBB);
  Builder.SetInsertPoint(ThenBB->begin());

  codegen(g->then);

  Builder.SetInsertPoint(MergeBB->begin());
}

void ClastStmtCodeGen::codegen(const clast_stmt *stmt) {
  if	    (CLAST_STMT_IS_A(stmt, stmt_root))
    assert(false && "No second root statement expected");
  else if (CLAST_STMT_IS_A(stmt, stmt_ass))
    codegen((const clast_assignment *)stmt);
  else if (CLAST_STMT_IS_A(stmt, stmt_user))
    codegen((const clast_user_stmt *)stmt);
  else if (CLAST_STMT_IS_A(stmt, stmt_block))
    codegen((const clast_block *)stmt);
  else if (CLAST_STMT_IS_A(stmt, stmt_for))
    codegen((const clast_for *)stmt);
  else if (CLAST_STMT_IS_A(stmt, stmt_guard))
    codegen((const clast_guard *)stmt);

  if (stmt->next)
    codegen(stmt->next);
}

void ClastStmtCodeGen::addParameters(const CloogNames *names) {
  SCEVExpander Rewriter(P->getAnalysis<ScalarEvolution>(), "polly");

  int i = 0;
  for (Scop::param_iterator PI = S->param_begin(), PE = S->param_end();
       PI != PE; ++PI) {
    assert(i < names->nb_parameters && "Not enough parameter names");

    const SCEV *Param = *PI;
    Type *Ty = Param->getType();

    Instruction *insertLocation = --(Builder.GetInsertBlock()->end());
    Value *V = Rewriter.expandCodeFor(Param, Ty, insertLocation);
    ClastVars[names->parameters[i]] = V;

    ++i;
  }
}

void ClastStmtCodeGen::codegen(const clast_root *r) {
  addParameters(r->names);

  parallelCodeGeneration = false;

  const clast_stmt *stmt = (const clast_stmt*) r;
  if (stmt->next)
    codegen(stmt->next);
}

ClastStmtCodeGen::ClastStmtCodeGen(Scop *scop, IRBuilder<> &B, Pass *P) :
    S(scop), P(P), Builder(B), ExpGen(Builder, ClastVars) {}

namespace {
class CodeGeneration : public ScopPass {
  Region *region;
  Scop *S;
  DominatorTree *DT;
  RegionInfo *RI;

  std::vector<std::string> parallelLoops;

  public:
  static char ID;

  CodeGeneration() : ScopPass(ID) {}

  // Split the entry edge of the region and generate a new basic block on this
  // edge. This function also updates ScopInfo and RegionInfo.
  //
  // @param region The region where the entry edge will be splitted.
  BasicBlock *splitEdgeAdvanced(Region *region) {
    BasicBlock *newBlock;
    BasicBlock *splitBlock;

    newBlock = SplitEdge(region->getEnteringBlock(), region->getEntry(), this);

    if (DT->dominates(region->getEntry(), newBlock)) {
      BasicBlock *OldBlock = region->getEntry();
      std::string OldName = OldBlock->getName();

      // Update ScopInfo.
      for (Scop::iterator SI = S->begin(), SE = S->end(); SI != SE; ++SI)
        if ((*SI)->getBasicBlock() == OldBlock) {
          (*SI)->setBasicBlock(newBlock);
          break;
        }

      // Update RegionInfo.
      splitBlock = OldBlock;
      OldBlock->setName("polly.split");
      newBlock->setName(OldName);
      region->replaceEntry(newBlock);
      RI->setRegionFor(newBlock, region);
    } else {
      RI->setRegionFor(newBlock, region->getParent());
      splitBlock = newBlock;
    }

    return splitBlock;
  }

  // Create a split block that branches either to the old code or to a new basic
  // block where the new code can be inserted.
  //
  // @param Builder A builder that will be set to point to a basic block, where
  //                the new code can be generated.
  // @return The split basic block.
  BasicBlock *addSplitAndStartBlock(IRBuilder<> *Builder) {
    BasicBlock *StartBlock, *SplitBlock;

    SplitBlock = splitEdgeAdvanced(region);
    SplitBlock->setName("polly.split_new_and_old");
    Function *F = SplitBlock->getParent();
    StartBlock = BasicBlock::Create(F->getContext(), "polly.start", F);
    SplitBlock->getTerminator()->eraseFromParent();
    Builder->SetInsertPoint(SplitBlock);
    Builder->CreateCondBr(Builder->getTrue(), StartBlock, region->getEntry());
    DT->addNewBlock(StartBlock, SplitBlock);
    Builder->SetInsertPoint(StartBlock);
    return SplitBlock;
  }

  // Merge the control flow of the newly generated code with the existing code.
  //
  // @param SplitBlock The basic block where the control flow was split between
  //                   old and new version of the Scop.
  // @param Builder    An IRBuilder that points to the last instruction of the
  //                   newly generated code.
  BasicBlock *mergeControlFlow(BasicBlock *SplitBlock, IRBuilder<> *Builder) {
    BasicBlock *MergeBlock;
    Region *R = region;

    if (R->getExit()->getSinglePredecessor())
      // No splitEdge required.  A block with a single predecessor cannot have
      // PHI nodes that would complicate life.
      MergeBlock = R->getExit();
    else {
      MergeBlock = SplitEdge(R->getExitingBlock(), R->getExit(), this);
      // SplitEdge will never split R->getExit(), as R->getExit() has more than
      // one predecessor. Hence, mergeBlock is always a newly generated block.
      R->replaceExit(MergeBlock);
    }

    Builder->CreateBr(MergeBlock);
    MergeBlock->setName("polly.merge_new_and_old");

    if (DT->dominates(SplitBlock, MergeBlock))
      DT->changeImmediateDominator(MergeBlock, SplitBlock);

    return MergeBlock;
  }

  bool runOnScop(Scop &scop) {
    S = &scop;
    region = &S->getRegion();
    DT = &getAnalysis<DominatorTree>();
    RI = &getAnalysis<RegionInfo>();

    parallelLoops.clear();

    assert(region->isSimple() && "Only simple regions are supported");

    // In the CFG the optimized code of the SCoP is generated next to the
    // original code. Both the new and the original version of the code remain
    // in the CFG. A branch statement decides which version is executed.
    // For now, we always execute the new version (the old one is dead code
    // eliminated by the cleanup passes). In the future we may decide to execute
    // the new version only if certain run time checks succeed. This will be
    // useful to support constructs for which we cannot prove all assumptions at
    // compile time.
    //
    // Before transformation:
    //
    //                        bb0
    //                         |
    //                     orig_scop
    //                         |
    //                        bb1
    //
    // After transformation:
    //                        bb0
    //                         |
    //                  polly.splitBlock
    //                     /       \.
    //                     |     startBlock
    //                     |        |
    //               orig_scop   new_scop
    //                     \      /
    //                      \    /
    //                        bb1 (joinBlock)
    IRBuilder<> builder(region->getEntry());

    // The builder will be set to startBlock.
    BasicBlock *splitBlock = addSplitAndStartBlock(&builder);
    BasicBlock *StartBlock = builder.GetInsertBlock();

    //BasicBlock *MergeBlock = mergeControlFlow(splitBlock, &builder);
    mergeControlFlow(splitBlock, &builder);
    builder.SetInsertPoint(StartBlock->begin());

    ClastStmtCodeGen CodeGen(S, builder, this);
    CloogInfo &C = getAnalysis<CloogInfo>();
    CodeGen.codegen(C.getClast());

    parallelLoops.insert(parallelLoops.begin(),
                         CodeGen.getParallelLoops().begin(),
                         CodeGen.getParallelLoops().end());

    return true;
  }

  virtual void printScop(raw_ostream &OS) const {
    for (std::vector<std::string>::const_iterator PI = parallelLoops.begin(),
         PE = parallelLoops.end(); PI != PE; ++PI)
      OS << "Parallel loop with iterator '" << *PI << "' generated\n";
  }

  virtual void getAnalysisUsage(AnalysisUsage &AU) const {
    AU.addRequired<CloogInfo>();
    AU.addRequired<Dependences>();
    AU.addRequired<DominatorTree>();
    AU.addRequired<RegionInfo>();
    AU.addRequired<ScalarEvolution>();
    AU.addRequired<ScopDetection>();
    AU.addRequired<ScopInfo>();
    AU.addRequired<TargetData>();

    AU.addPreserved<CloogInfo>();
    AU.addPreserved<Dependences>();

    // FIXME: We do not create LoopInfo for the newly generated loops.
    AU.addPreserved<LoopInfo>();
    AU.addPreserved<DominatorTree>();
    AU.addPreserved<ScopDetection>();
    AU.addPreserved<ScalarEvolution>();

    // FIXME: We do not yet add regions for the newly generated code to the
    //        region tree.
    AU.addPreserved<RegionInfo>();
    AU.addPreserved<TempScopInfo>();
    AU.addPreserved<ScopInfo>();
    AU.addPreservedID(IndependentBlocksID);
  }
};
}

char CodeGeneration::ID = 1;

INITIALIZE_PASS_BEGIN(CodeGeneration, "polly-codegen",
                      "Polly - Create LLVM-IR from SCoPs", false, false)
INITIALIZE_PASS_DEPENDENCY(CloogInfo)
INITIALIZE_PASS_DEPENDENCY(Dependences)
INITIALIZE_PASS_DEPENDENCY(DominatorTree)
INITIALIZE_PASS_DEPENDENCY(RegionInfo)
INITIALIZE_PASS_DEPENDENCY(ScalarEvolution)
INITIALIZE_PASS_DEPENDENCY(ScopDetection)
INITIALIZE_PASS_DEPENDENCY(TargetData)
INITIALIZE_PASS_END(CodeGeneration, "polly-codegen",
                      "Polly - Create LLVM-IR from SCoPs", false, false)

Pass *polly::createCodeGenerationPass() {
  return new CodeGeneration();
}
