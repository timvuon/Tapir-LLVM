//===- Loop2Cilk.cpp - Convert Loops of Detaches to use the Cilk Runtime ----------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Convert Loops of Detaches to use the Cilk Runtime
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/LoopPass.h"
#include "llvm/Analysis/ScalarEvolutionExpander.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/SimplifyIndVar.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Transforms/CilkABI.h"
#include "llvm/Transforms/Utils/PromoteMemToReg.h"
#include "llvm/Transforms/Utils/LoopUtils.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Transforms/Utils/PromoteMemToReg.h"

#include <utility>
using std::make_pair;

using namespace llvm;

#define DEBUG_TYPE "loop2cilk"

namespace {
  class Loop2Cilk : public LoopPass {

  public:

    static char ID; // Pass identification, replacement for typeid
    Loop2Cilk() : LoopPass(ID) { }

    bool runOnLoop(Loop *L, LPPassManager &LPM) override;

    void getAnalysisUsage(AnalysisUsage &AU) const override {
      AU.addRequired<DominatorTreeWrapperPass>();
      AU.addRequired<LoopInfoWrapperPass>();
      AU.addRequired<ScalarEvolutionWrapperPass>();
      AU.addRequiredID(LoopSimplifyID);
    }

  private:
    void releaseMemory() override {
    }

  };
}

char Loop2Cilk::ID = 0;
static RegisterPass<Loop2Cilk> X("loop2cilk", "Find cilk for loops and use more efficient runtime", false, false);

INITIALIZE_PASS_BEGIN(Loop2Cilk, "loop2cilk",
                "Find cilk for loops and use more efficient runtime", false, false)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(ScalarEvolutionWrapperPass)
INITIALIZE_PASS_DEPENDENCY(LoopSimplify)
INITIALIZE_PASS_END(Loop2Cilk, "loop2cilk", "Find cilk for loops and use more efficient runtime", false, false)

Pass *llvm::createLoop2CilkPass() {
  return new Loop2Cilk();
}

size_t countPredecessors(BasicBlock* syncer) {
  size_t count = 0;
  for (auto it = pred_begin(syncer), et = pred_end(syncer); it != et; ++it) {
    count++;
  }
  return count;
}


Value* neg( Value* V ) {
  if( Constant* C = dyn_cast<Constant>(V) ) {
    ConstantFolder F;
    return F.CreateNeg(C);
  }

  Instruction* I = nullptr;
  bool move = false;
  if( Argument* A = dyn_cast<Argument>(V) ) {
    I = A->getParent()->getEntryBlock().getFirstNonPHIOrDbgOrLifetime();
  } else {
    assert( isa<Instruction>(V) );
    I = cast<Instruction>(V);
    move = true;
  }
  assert(I);
  IRBuilder<> builder(I);
  Instruction* foo = cast<Instruction>(builder.CreateNeg(V));
  if (move) I->moveBefore(foo);
  return foo;
}

Value* subOne( Value* V ) {
  if( Constant* C = dyn_cast<Constant>(V) ) {
    ConstantFolder F;
    return F.CreateSub(C, ConstantInt::get(V->getType(), 1) );
  }
  Instruction* I = nullptr;
  bool move = false;
  if( Argument* A = dyn_cast<Argument>(V) ) {
    I = A->getParent()->getEntryBlock().getFirstNonPHIOrDbgOrLifetime();
  } else {
    assert( isa<Instruction>(V) );
    I = cast<Instruction>(V);
    move = true;
  }
  assert(I);
  IRBuilder<> builder(I);
  Instruction* foo = cast<Instruction>(builder.CreateSub( V, ConstantInt::get(V->getType(), 1) ));
  if (move) I->moveBefore(foo);
  return foo;
}

Value* addOne( Value* V ) {
  if( Constant* C = dyn_cast<Constant>(V) ) {
    ConstantFolder F;
    return F.CreateAdd(C, ConstantInt::get(V->getType(), 1) );
  }

  Instruction* I = nullptr;
  bool move = false;
  if( Argument* A = dyn_cast<Argument>(V) ) {
    I = A->getParent()->getEntryBlock().getFirstNonPHIOrDbgOrLifetime();
  } else {
    assert( isa<Instruction>(V) );
    I = cast<Instruction>(V);
    move = true;
  }
  assert(I);
  IRBuilder<> builder(I);
  Instruction* foo = cast<Instruction>(builder.CreateAdd( V, ConstantInt::get(V->getType(), 1) ));
  if (move) I->moveBefore(foo);
  return foo;
}

Value* uncast( Value* V ){
  if( auto* in = dyn_cast<TruncInst>(V) ) {
    return uncast(in->getOperand(0));
  }
  if( auto* in = dyn_cast<SExtInst>(V) ) {
    return uncast(in->getOperand(0));
  }
  if( auto* in = dyn_cast<ZExtInst>(V) ) {
    return uncast(in->getOperand(0));
  }
  return V;
}

size_t countPHI(BasicBlock* b){
    int phi = 0;
    BasicBlock::iterator i = b->begin();
    while (isa<PHINode>(i) ) { ++i; phi++; }
    return phi;
}

int64_t getInt(Value* v, bool & failed){
  if( ConstantInt* CI = dyn_cast<ConstantInt>(v) ) {
    failed = false;
    return CI->getSExtValue();
  }
  failed = true;
  return -1;
}

bool isOne(Value* v){
  bool m = false;
  return getInt(v, m) == 1;
}

bool isZero(Value* v){
  bool m = false;
  return getInt(v, m) == 0;
}

bool attemptRecursiveMoveHelper(Instruction* toMoveAfter, Instruction* toCheck, DominatorTree& DT, std::vector<Instruction*>& candidates) {
  switch (toCheck->getOpcode()) {
    case Instruction::Add:
    case Instruction::FAdd:
    case Instruction::Sub:
    case Instruction::FSub:
    case Instruction::Mul:
    case Instruction::FMul:
    case Instruction::UDiv:
    case Instruction::SDiv:
    case Instruction::FDiv:
    case Instruction::URem:
    case Instruction::SRem:
    case Instruction::FRem:

    case Instruction::And:
    case Instruction::Or:
    case Instruction::Xor:

    case Instruction::ICmp:
    case Instruction::FCmp:
    case Instruction::Select:
    case Instruction::ExtractElement:
    case Instruction::InsertElement:
    case Instruction::ShuffleVector:
    case Instruction::ExtractValue:
    case Instruction::InsertValue:

    case Instruction::Shl:
    case Instruction::LShr:
    case Instruction::AShr:

    case Instruction::Trunc:
    case Instruction::ZExt:
    case Instruction::FPToUI:
    case Instruction::UIToFP:
    case Instruction::SIToFP:
    case Instruction::FPTrunc:
    case Instruction::FPExt:
    case Instruction::PtrToInt:
    case Instruction::IntToPtr:
    case Instruction::BitCast:

      for (auto & u2 : toCheck->uses() ) {
        if (!DT.dominates(toMoveAfter, u2) ) {
          assert( isa<Instruction>(u2.getUser()) );
          if (!attemptRecursiveMoveHelper(toMoveAfter, cast<Instruction>(u2.getUser()), DT, candidates)) return false;
        }
      }
    default: return false;
  }
  return true;
}

bool attemptRecursiveMove(Instruction* toMoveAfter, Instruction* toCheck, DominatorTree& DT) {
  std::vector<Instruction*> candidates;
  bool b = attemptRecursiveMoveHelper(toMoveAfter, toCheck, DT, candidates);
  if (!b) return false;

  auto last = toMoveAfter;
  for (int i=candidates.size()-1; i>0; i--) {
    candidates[i]->moveBefore(last);
    last = candidates[i];
  }
  if (last != toMoveAfter) toMoveAfter->moveBefore(last);
  return true; 
}

bool recursiveMoveBefore(Instruction* toMoveBefore, Value* toMoveVal, DominatorTree& DT) {
  Instruction* toMoveI = dyn_cast<Instruction>(toMoveVal);
  if (!toMoveI) return true;

  if( llvm::verifyFunction(*toMoveBefore->getParent()->getParent(), nullptr) ) {
    toMoveBefore->getParent()->getParent()->dump();
  }
  assert( !llvm::verifyFunction(*toMoveBefore->getParent()->getParent(), &llvm::errs()) );

  std::vector<Value*> toMove;
  toMove.push_back(toMoveI);
  Instruction* pi = toMoveBefore;

  while (!toMove.empty()) {
    auto b = toMove.back();
    toMove.pop_back();
    if( Instruction* inst = dyn_cast<Instruction>(b) ) {
      if( !DT.dominates(inst, toMoveBefore) ) {
        //errs() << "moving: ";
        //b->dump();
        for (User::op_iterator i = inst->op_begin(), e = inst->op_end(); i != e; ++i) {
          Value *v = *i;
          toMove.push_back(v);
        }
        if (inst->mayHaveSideEffects()) {
          errs() << "something side fx\n";
          assert( !llvm::verifyFunction(*toMoveBefore->getParent()->getParent(), &llvm::errs()) );
          return false;
        }
        inst->moveBefore(pi);
        pi = inst;
      }
    }
  }

  if( llvm::verifyFunction(*toMoveBefore->getParent()->getParent(), nullptr) ) {
    toMoveBefore->getParent()->getParent()->dump();
  }
  assert( !llvm::verifyFunction(*toMoveBefore->getParent()->getParent(), &llvm::errs()) );
  return true;
}

/* Returns ind var / number of iterations */
std::pair<PHINode*,Value*> getIndVar(Loop *L, BasicBlock* detacher, DominatorTree& DT) {
  BasicBlock *H = L->getHeader();

  BasicBlock *Incoming = nullptr, *Backedge = nullptr;
  pred_iterator PI = pred_begin(H);
  assert(PI != pred_end(H) && "Loop must have at least one backedge!");
  Backedge = *PI++;
  if (PI == pred_end(H)) return make_pair(nullptr,nullptr);  // dead loop
  Incoming = *PI++;
  if (PI != pred_end(H)) return make_pair(nullptr,nullptr);  // multiple backedges?

  if (L->contains(Incoming)) {
    if (L->contains(Backedge)) return make_pair(nullptr,nullptr);
    std::swap(Incoming, Backedge);
  } else if (!L->contains(Backedge)) return make_pair(nullptr,nullptr);

  assert( L->contains(Backedge) );
  assert( !L->contains(Incoming) );
  llvm::CmpInst* cmp = 0;
  int cmpIdx = -1;
  llvm::Value* opc = 0;

  BasicBlock* cmpNode = Backedge;
  if (H != detacher) {
    cmpNode = detacher->getUniquePredecessor();
    if(cmpNode==nullptr) return make_pair(nullptr,nullptr);
  }

  if (BranchInst* brnch = dyn_cast<BranchInst>(cmpNode->getTerminator()) ) {
    if (!brnch->isConditional()) goto cmp_error;
    if ( (cmp = dyn_cast<CmpInst>(brnch->getCondition())) ) {
      
    } else {
      errs() << "no comparison inst from backedge\n";
      cmpNode->getTerminator()->dump();
      return make_pair(nullptr,nullptr);
    }
    if ( !L->contains(brnch->getSuccessor(0)) ) {
      cmp->setPredicate(CmpInst::getInversePredicate(cmp->getPredicate()));
      brnch->swapSuccessors();
    }
    if (!cmp->isIntPredicate() || cmp->getPredicate() == CmpInst::ICMP_EQ ) {
      cmpNode->getParent()->dump();
      cmpNode->dump();
      cmp->dump();
      brnch->dump();
      return make_pair(nullptr,nullptr);
    }
  } else {
    cmp_error:
    errs() << "<no comparison from backedge>\n";
    cmpNode->getTerminator()->dump();
    cmpNode->getParent()->dump();
    errs() << "</no comparison from backedge>\n";
    return make_pair(nullptr,nullptr);
  }

  for (unsigned i=0; i<2; i++) {
    LoadInst* inst = dyn_cast<LoadInst>(uncast(cmp->getOperand(i)));
    if (!inst) continue;
    AllocaInst* alloca = dyn_cast<AllocaInst>(inst->getOperand(0));
    if (!alloca) continue;
    if (isAllocaPromotable(alloca, DT)) {
      PromoteMemToReg({alloca}, DT, nullptr, nullptr);
    }
  }

  // Loop over all of the PHI nodes, looking for a canonical indvar.
  PHINode* RPN = nullptr;
  Instruction* INCR = nullptr;
  Value* amt = nullptr;
  std::vector<std::tuple<PHINode*,Instruction*,Value*>> others;
  for (BasicBlock::iterator I = H->begin(); isa<PHINode>(I); ++I) {
    assert( isa<PHINode>(I) );
    PHINode *PN = cast<PHINode>(I);
    if( !PN->getType()->isIntegerTy() ) {
      errs() << "phinode uses non-int\n";
      return make_pair(nullptr,nullptr);
    }
    if (BinaryOperator* Inc = dyn_cast<BinaryOperator>(PN->getIncomingValueForBlock(Backedge))) {
      if (Inc->getOpcode() == Instruction::Sub && Inc->getOperand(0) == PN) {
        IRBuilder<> build(Inc);
        auto val = build.CreateNeg(Inc->getOperand(1));
        auto tmp = build.CreateAdd(PN, val);
        assert( isa<BinaryOperator>(tmp) );
        auto newI = cast<BinaryOperator>(tmp);
        Inc->replaceAllUsesWith(newI);
        for (auto& tup : others) {
          if (std::get<1>(tup) == Inc) std::get<1>(tup) = newI;
          if (std::get<2>(tup) == Inc) std::get<2>(tup) = newI;
        }
        Inc->eraseFromParent();
        Inc = newI;
      }
      if (Inc->getOpcode() == Instruction::Add && (Inc->getOperand(0) == PN || Inc->getOperand(1) == PN) ) {
        if (Inc->getOperand(1) == PN ) Inc->swapOperands();
        assert(Inc->getOperand(0) == PN);
        bool rpnr = false;
        bool incr = false;
        for(unsigned i = 0; i < cmp->getNumOperands(); i++) {
          rpnr |= uncast(cmp->getOperand(i)) == PN;
          incr |= uncast(cmp->getOperand(i)) == Inc;
          if( rpnr | incr ) cmpIdx = i;
        }
        assert( !rpnr || !incr );
        if( rpnr | incr ) {
          amt = Inc->getOperand(1);
          RPN = PN;
          INCR = Inc;
          opc = rpnr?RPN:INCR;
        } else {
          others.push_back( std::make_tuple(PN,Inc,Inc->getOperand(1)) );
        }
        if (!recursiveMoveBefore(Incoming->getTerminator(), Inc->getOperand(1), DT)) return make_pair(nullptr, nullptr);
        if (!recursiveMoveBefore(Incoming->getTerminator(), PN->getIncomingValueForBlock(Incoming), DT)) return make_pair(nullptr, nullptr);
      } else {
        errs() << "no add found for:\n"; PN->dump(); Inc->dump();
        H->getParent()->dump();
        return make_pair(nullptr,nullptr);
      }
    } else {
      errs() << "no inc found for:\n"; PN->dump();
    }
  }

  assert( !llvm::verifyFunction(*L->getHeader()->getParent(), &llvm::errs()) );


  if( RPN == 0 ) {
    errs() << "<no RPN>\n";
    cmp->dump();
    errs() << "<---->\n";
    H->dump();
    errs() << "<---->\n";
    for( auto a : others ) { std::get<0>(a)->dump(); }
    errs() << "</no RPN>\n";
    return make_pair(nullptr,nullptr);
  }

  //errs() << "PRE_REPLACE:\n"; H->getParent()->dump();

  llvm::Value* mul;
  llvm::Value* newV;

  SmallPtrSet<llvm::Value*, 4> toIgnore;
  {
    IRBuilder<> builder(detacher->getTerminator()->getSuccessor(0)->getFirstNonPHIOrDbgOrLifetime());
    if( isOne(amt) ) mul = RPN;
    else toIgnore.insert(mul = builder.CreateMul(RPN, amt));
    if( isZero(RPN->getIncomingValueForBlock(Incoming) )) newV = mul;
    else toIgnore.insert(newV = builder.CreateAdd(mul, RPN->getIncomingValueForBlock(Incoming) ));

    //  std::vector<Value*> replacements;
    for( auto a : others ) {
      llvm::Value* val = builder.CreateSExtOrTrunc(RPN, std::get<0>(a)->getType());
      if (val != RPN) toIgnore.insert(val); 
      llvm::Value* amt0 = std::get<2>(a);
      if( !isOne(amt0) ) val = builder.CreateMul(val,amt0);
      if (val != RPN) toIgnore.insert(val);
      llvm::Value* add0 = std::get<0>(a)->getIncomingValueForBlock(Incoming);
      if( !isZero(add0) ) val = builder.CreateAdd(val,add0);
      if (val != RPN) toIgnore.insert(val);
      //std::get<0>(a)->dump();
      assert( isa<Instruction>(val) );
      Instruction* ival = cast<Instruction>(val);

      for (auto& u : std::get<0>(a)->uses()) {
        assert( isa<Instruction>(u.getUser()) );
        Instruction *user = cast<Instruction>(u.getUser());

        //No need to override use in PHINode itself
        if (user == std::get<0>(a)) continue;
        //No need to override use in increment
        if (user == std::get<1>(a)) continue;

        if (!attemptRecursiveMove(ival, user, DT)) {
          val->dump();
          user->dump();
          std::get<0>(a)->dump();
          H->getParent()->dump();
        }
        assert(DT.dominates(ival, user));
      }
      {
        auto tmp = std::get<0>(a);
        tmp->replaceAllUsesWith(val);
        for (auto& tup : others) {
          if (std::get<1>(tup) == tmp) std::get<1>(tup) = tmp;
          if (std::get<2>(tup) == tmp) std::get<2>(tup) = tmp;
        }
        tmp->eraseFromParent();
      }
      if(std::get<1>(a)->getNumUses() == 0) {
        auto tmp = std::get<1>(a);
        bool replacable = true;
        for (auto& tup : others) {
          if (std::get<1>(tup) == tmp || std::get<2>(tup) == tmp) replacable = false;
        }
        if (replacable) tmp->eraseFromParent();
      }
    }

    //errs() << "RPN  :\n"; RPN->dump();
    //errs() << "MUL  :\n"; mul->dump();
    //errs() << "NEWV :\n"; newV->dump();
    //errs() << "NEWVP:\n"; ((Instruction*)newV)->getParent()->dump();
  }

  if( llvm::verifyFunction(*L->getHeader()->getParent(), nullptr) ) L->getHeader()->getParent()->dump();
  assert( !llvm::verifyFunction(*L->getHeader()->getParent(), &llvm::errs()) );

  std::vector<Use*> uses;
  for( Use& U : RPN->uses() ) uses.push_back(&U);
  for( Use* Up : uses ) {
    Use &U = *Up;
    assert( isa<Instruction>(U.getUser()) );
    Instruction *I = cast<Instruction>(U.getUser());
    if( I == INCR ) INCR->setOperand(1, ConstantInt::get( RPN->getType(), 1 ) );
    else if( toIgnore.count(I) > 0 && I != RPN ) continue;
    else if( uncast(I) == cmp || I == cmp->getOperand(0) || I == cmp->getOperand(1) || uncast(I) == cmp || I == RPN || I->getParent() == cmp->getParent() || I->getParent() == detacher) continue;
    else {
      assert( isa<Instruction>(newV) );
      Instruction* ival = cast<Instruction>(newV);
      assert( isa<Instruction>(U.getUser()) );
      if (attemptRecursiveMove(ival, cast<Instruction>(U.getUser()), DT)) {
        llvm::errs() << "newV: ";
        newV->dump();
        llvm::errs() << "U: ";
        U->dump();
        llvm::errs() << "I: ";
        I->dump();
        llvm::errs() << "uncast(I): ";
        uncast(I)->dump();
        llvm::errs() << "errs: ";
        cmp->dump();
        llvm::errs() << "RPN: ";
        RPN->dump();
        H->getParent()->dump();
      }
      assert( DT.dominates((Instruction*) newV, U) );
      U.set( newV );
    }
  }
  
  if( llvm::verifyFunction(*L->getHeader()->getParent(), nullptr) ) L->getHeader()->getParent()->dump();
  assert( !llvm::verifyFunction(*L->getHeader()->getParent(), &llvm::errs()) );

  IRBuilder<> build(cmp);
  llvm::Value* val = build.CreateSExtOrTrunc(cmp->getOperand(cmpIdx),RPN->getType());
  llvm::Value* adder = RPN->getIncomingValueForBlock(Incoming);
  llvm::Value* amt0  = amt;

  int cast_type = 0;
  if( isa<TruncInst>(val) ) cast_type = 1;
  if( isa<SExtInst>(val) ) cast_type = 2;
  if( isa<ZExtInst>(val) ) cast_type = 3;

  switch(cast_type) {
    default:;
    case 1: amt0 = build.CreateTrunc(amt0,val->getType());
    case 2: amt0 = build.CreateSExt( amt0,val->getType());
    case 3: amt0 = build.CreateZExt( amt0,val->getType());
  }

  {
    switch(cast_type){
      default:;
      case 1: adder = build.CreateTrunc(adder,val->getType());
      case 2: adder = build.CreateSExt( adder,val->getType());
      case 3: adder = build.CreateZExt( adder,val->getType());
    }
    Value *bottom = adder, *top = val;
    if (opc != RPN) {
      cmp->setOperand(1-cmpIdx, RPN);
      bottom = build.CreateAdd(adder, amt0);
    }
    int dir = 0;
    switch (cmp->getPredicate() ) {
      case CmpInst::ICMP_UGE:
      case CmpInst::ICMP_UGT:
      case CmpInst::ICMP_SGE:
      case CmpInst::ICMP_SGT:
        dir = -1;break;
      case CmpInst::ICMP_ULE:
      case CmpInst::ICMP_ULT:
      case CmpInst::ICMP_SLE:
      case CmpInst::ICMP_SLT:
        dir = +1;break;
      default:
        dir = 0;break;
    }
    if (dir < 0) { std::swap(bottom, top); }
    //llvm::errs() << "bottom: "; bottom->dump();
    //llvm::errs() << "top: "; top->dump();
    if( !isZero(bottom) ) val = build.CreateSub(top, bottom);
    //llvm::errs() << "diff: "; val->dump();
    switch (cmp->getPredicate() ) {
      case CmpInst::ICMP_UGT:
      case CmpInst::ICMP_SGT:
      case CmpInst::ICMP_ULT:
      case CmpInst::ICMP_SLT:
        val = subOne(val);  
        break;  
      case CmpInst::ICMP_SLE:
      case CmpInst::ICMP_ULE:
      case CmpInst::ICMP_SGE:
      case CmpInst::ICMP_UGE:
      default:
        break;
    }
    //llvm::errs() << "tdiff: "; val->dump();
  }
  {
    //llvm::errs() << "amt0 *: "; amt0->dump();
    //llvm::errs() << "val *: "; val->dump();
    switch (cmp->getPredicate() ) {
      case CmpInst::ICMP_SLE:
      case CmpInst::ICMP_ULE:
      case CmpInst::ICMP_ULT:
      case CmpInst::ICMP_SLT:
        if (cmpIdx == 0) amt0 = neg(amt0); break;
      case CmpInst::ICMP_SGE:
      case CmpInst::ICMP_UGE:
      case CmpInst::ICMP_UGT:
      case CmpInst::ICMP_SGT:
        if (cmpIdx == 1) amt0 = neg(amt0); break;
      case CmpInst::ICMP_NE:
        //amt0 = build.CreateSelect(build.CreateICmpSGT(amt0,ConstantInt::get(val->getType(), 0)),amt0,neg(amt0));
      default:
        break;
    }
    //llvm::errs() << "amt0 : "; amt0->dump();
    if (!isOne(amt0)) val = build.CreateSDiv(val, amt0);
    if (cmp->getPredicate()!=CmpInst::ICMP_NE) val = addOne(val);
  }

  cmp->setPredicate(CmpInst::ICMP_NE);

  cmp->setOperand(cmpIdx, val);
  cmp->setOperand(1-cmpIdx, RPN);

  if( llvm::verifyFunction(*L->getHeader()->getParent(), nullptr) ) L->getHeader()->getParent()->dump();
  assert( !llvm::verifyFunction(*L->getHeader()->getParent(), &llvm::errs()) );

  RPN->setIncomingValue( RPN->getBasicBlockIndex(Incoming),  ConstantInt::get( RPN->getType(), 0 ) );

  if( llvm::verifyFunction(*L->getHeader()->getParent(), nullptr) ) L->getHeader()->getParent()->dump();
  assert( !llvm::verifyFunction(*L->getHeader()->getParent(), &llvm::errs()) );

  return make_pair(RPN, val);
}

void removeFromAll(Loop* L, BasicBlock* B){
  if( !L ) return;
  if( L->contains(B) ) L->removeBlockFromLoop(B);
  removeFromAll(L->getParentLoop(), B);
}

template<typename A, typename B> bool contains(const A& a, const B& b) {
  return std::find(a.begin(), a.end(), b) != a.end();
}

BasicBlock* getTrueExit(Loop *L){
  SmallVector< BasicBlock *, 32> exitBlocks;
  L->getExitBlocks(exitBlocks);
  BasicBlock* endL = 0;
  SmallPtrSet<BasicBlock *, 32> exits(exitBlocks.begin(), exitBlocks.end());
  SmallPtrSet<BasicBlock *, 32> alsoLoop;

  bool toRemove = true;
  while (toRemove) {
    toRemove = false;
    if( exits.size() >= 2 ) {
      for( auto tempExit : exits ) {
        SmallPtrSet<BasicBlock *, 32> reachable;
        std::vector<BasicBlock*> Q = { tempExit };
        bool valid = true;
        while(!Q.empty() && valid) {
          auto m = Q.back();
          Q.pop_back();
          if( isa<UnreachableInst>(m->getTerminator()) ) { reachable.insert(m); continue; }
          else if( auto b = dyn_cast<BranchInst>(m->getTerminator()) ) {
            reachable.insert(m);
            for( unsigned i=0; i<b->getNumSuccessors(); i++ ) {
               auto suc = b->getSuccessor(i);
               if( L->contains(suc) || contains(exitBlocks,suc) || contains(alsoLoop, suc) || contains(reachable, suc) ) {

               } else{
                Q.push_back(suc);
                break;
              }
            }
          }
          else valid = false;
        }
        if( valid && reachable.size() > 0 ) {
          for( auto b : reachable){
            exits.erase(b);
            alsoLoop.insert(b);
          }
          toRemove = true;
        }
      }
    }
  }

    if( exits.size() == 1 ) endL = * exits.begin();
    else {
      //errs() << "<blocks>\n";
      //for(auto a : exits ) a->dump();
      //errs() << "</blocks>\n";
    }
    return endL;
}

bool Loop2Cilk::runOnLoop(Loop *L, LPPassManager &LPM) {
  if (skipOptnoneFunction(L)) {
  	assert( !llvm::verifyFunction(*L->getHeader()->getParent(), &llvm::errs()) );
    return false;
  }

	assert( !llvm::verifyFunction(*L->getHeader()->getParent(), &llvm::errs()) );

  if (!L->isLoopSimplifyForm()) {
    simplifyLoop(L, nullptr, nullptr, nullptr, nullptr, false);
  }

  BasicBlock* Header = L->getHeader();
  assert(Header);

  Loop* parentL = L->getParentLoop();
  LoopInfo &LI = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();

  TerminatorInst* T = Header->getTerminator();
  if (!isa<BranchInst>(T)) {
    BasicBlock *Preheader = L->getLoopPreheader();
    if( isa<BranchInst>(Preheader->getTerminator()) ) { T = Preheader->getTerminator(); Header = Preheader; }
    else {
      llvm::errs() << "Loop not entered via branch instance\n";
      T->dump();
  	  assert( !llvm::verifyFunction(*L->getHeader()->getParent(), &llvm::errs()) );
      return false;
    }
  }


  assert( !llvm::verifyFunction(*L->getHeader()->getParent(), &llvm::errs()) );

  assert( isa<BranchInst>(T) );
  BranchInst* B = cast<BranchInst>(T);

  BasicBlock *detacher = nullptr, *syncer = nullptr;
  /////!!< BEGIN ESTABLISH DETACH/SYNC BLOCKS
  if (B->getNumSuccessors() != 2) {
    BasicBlock* endL = getTrueExit(L);
    //BasicBlock* oendL = endL;
    while (endL && !isa<SyncInst>(endL->getTerminator())) {
      if( getNonPhiSize(endL) == 1 && isa<BranchInst>(endL->getTerminator()) && endL->getTerminator()->getNumSuccessors() == 1 ) {
        endL = endL->getTerminator()->getSuccessor(0);
      }
      else
        endL = nullptr;
    }

    if (endL) {
      syncer = endL;
      assert(syncer && isa<SyncInst>(syncer->getTerminator()));
      detacher = B->getSuccessor(0);
      assert(detacher && isa<DetachInst>(detacher->getTerminator()));
    } else {
     	assert( !llvm::verifyFunction(*L->getHeader()->getParent(), &llvm::errs()) );
      return false;
    }
  } else {
    detacher = B->getSuccessor(0);
    syncer = B->getSuccessor(1);

    if (isa<DetachInst>(B->getSuccessor(0)->getTerminator()) && isa<SyncInst>(B->getSuccessor(1)->getTerminator())) {
      detacher = B->getSuccessor(0);
      syncer   = B->getSuccessor(1);
    } else if (isa<DetachInst>(B->getSuccessor(1)->getTerminator()) && isa<SyncInst>(B->getSuccessor(0)->getTerminator())) {
      detacher = B->getSuccessor(1);
      syncer   = B->getSuccessor(0);
    } else {
      //errs() << "none sync" << "\n";
      //syncer->dump();
      //detacher->dump();
      return false;
    }

    BasicBlock* done = getTrueExit(L);
    if (!done) {
      errs() << "no unique exit block\n";
      assert( !llvm::verifyFunction(*L->getHeader()->getParent(), &llvm::errs()) );
      return false;
    }

    if (BranchInst* BI = dyn_cast<BranchInst>(done->getTerminator())) {
      if( BI->getNumSuccessors() == 2 ) {
        if( BI->getSuccessor(0) == detacher && BI->getSuccessor(1) == syncer )
          done = syncer;
        if( BI->getSuccessor(1) == detacher && BI->getSuccessor(0) == syncer )
          done = syncer;
      }
    }

    if (getUniquePred(done) == syncer) {
      auto term = done->getTerminator();
      bool good = true;
      for (unsigned i=0; i<term->getNumSuccessors(); i++)
        if (L->contains( term->getSuccessor(i))) {
          good = false;
          break;
        }
      if (good) done = syncer;
    }
    if (done != syncer) {
      errs() << "exit != sync\n";
      return false;
    }
  }
  /////!!< END ESTABLISH DETACH/SYNC BLOCKS
  assert(syncer && isa<SyncInst>(syncer->getTerminator()));
  assert(detacher && isa<DetachInst>(detacher->getTerminator()));

  DetachInst* det = cast<DetachInst>(detacher->getTerminator());

  DominatorTree &DT = getAnalysis<DominatorTreeWrapperPass>().getDomTree();

  /////!!< REQUIRE DETACHER BLOCK IS EMPTY EXCEPT FOR BRANCH
  while (getNonPhiSize(detacher)!=1) {
    Instruction* badInst = getLastNonTerm(detacher);
    if (!badInst->mayWriteToMemory()) {
      bool dominated = true;
      for (const Use &U : badInst->uses()) {
        if (!DT.dominates(BasicBlockEdge(detacher, det->getSuccessor(0) ), U) ) { errs() << "use not dominated:\n"; U->dump(); dominated = false; break; }
      }
      if (dominated) {
        badInst->moveBefore( getFirstPostPHI(det->getSuccessor(0)) );
        assert( !llvm::verifyFunction(*L->getHeader()->getParent(), &llvm::errs()) );
        continue;
      }
    } else errs() << "mayWrite:\n"; 
    errs() << "invalid detach size of " << getNonPhiSize(detacher) << "|" << detacher->size() << "\n";
    detacher->dump();
    return false;
  }

  /////!!< REQUIRE SYNC BLOCK HAS ONLY PHI's / EXIT
  while (getNonPhiSize(syncer)!=1) {
    Instruction* badInst = getLastNonTerm(syncer);
    if (!badInst->mayWriteToMemory()) {
      badInst->moveBefore( getFirstPostPHI(syncer->getTerminator()->getSuccessor(0)) );
    	assert( !llvm::verifyFunction(*L->getHeader()->getParent(), &llvm::errs()) );
    } else {
      errs() << "invalid sync size" << "\n";
      return false;
    }
  }

  /////!!< REMOVE ANY SYNC BLOCK PHI's
  while (syncer->size() != 1) {
    assert( isa<PHINode>(&syncer->front()) );
    PHINode* pn = cast<PHINode>(&syncer->front());
    if (pn->getNumIncomingValues() != 1 ) {
      errs() << "invalid phi for sync\n";
      return false;
    }
    pn->replaceAllUsesWith(pn->getIncomingValue(0));
    pn->eraseFromParent();
    assert( !llvm::verifyFunction(*L->getHeader()->getParent(), &llvm::errs()) );
  }

  std::pair<PHINode*,Value*> indVarResult = getIndVar(L, detacher, DT);
  PHINode* oldvar = indVarResult.first;
  Value* cmp = indVarResult.second;

  //oldvar guarenteed to be canonical (start at 0, inc by 1, end at ...)
  assert( !llvm::verifyFunction(*L->getHeader()->getParent(), &llvm::errs()) );
  if (!oldvar) {
      errs() << "no induction var\n";
      assert( !llvm::verifyFunction(*L->getHeader()->getParent(), &llvm::errs()) );
      return false;
  }

  assert( ( L->getHeader()->size() == getNonPhiSize(L->getHeader()) + 1 ) && "Can only cilk_for loops with only 1 phi node " );

  bool simplified = false;
  while (!simplified) {
    simplified = true;
    for (auto it = pred_begin(syncer), et = pred_end(syncer); it != et; ++it) {
      BasicBlock* endL = *it;
      if( getNonPhiSize(endL) == 1 && isa<BranchInst>(endL->getTerminator()) && endL->getTerminator()->getNumSuccessors() == 1 ) {
        bool success = TryToSimplifyUncondBranchFromEmptyBlock(endL);
        if(success) {
          removeFromAll(parentL, endL);
          LI.changeLoopFor(endL, nullptr);
          LI.removeBlock(endL);
          simplified = false;
          break;
        }
      }
    }
  }

  DT.recalculate(*L->getHeader()->getParent());

  llvm::CallInst* call = 0;
  llvm::Value*    closure = 0;

  if( llvm::verifyFunction(*Header->getParent(), nullptr) ) {
    Header->getParent()->getParent()->dump();
  }
  assert( !llvm::verifyFunction(*Header->getParent(), &llvm::errs()) );

  if (!recursiveMoveBefore(Header->getTerminator(), cmp, DT)) {
    errs() << "cmp not moved\n";
    assert( !llvm::verifyFunction(*L->getHeader()->getParent(), &llvm::errs()) );
    return false;
  }
  Function* extracted = llvm::cilk::extractDetachBodyToFunction( *det, &call, /*closure*/ oldvar, &closure );
  //Header->getParent()->getParent()->dump();
  if (llvm::verifyFunction(*Header->getParent(), nullptr)) {
    Header->getParent()->getParent()->dump();
  }
  assert(!llvm::verifyFunction(*Header->getParent(), &llvm::errs()));

  if( !extracted ) {
    errs() << "not extracted\n";
    assert( !llvm::verifyFunction(*L->getHeader()->getParent(), &llvm::errs()) );
    return false;
  }

  {
    for( BasicBlock& b : extracted->getBasicBlockList() )
      if( true ) {
        removeFromAll(parentL, &b);
        LI.changeLoopFor(&b, nullptr);
        LI.removeBlock(&b);
      }
  }

  if (llvm::verifyFunction(*Header->getParent(), nullptr)) {
    Header->getParent()->getParent()->dump();
  }
  assert(!llvm::verifyFunction(*Header->getParent(), &llvm::errs()));

  Module* M = extracted->getParent();
  auto a1 = det->getSuccessor(0);
  auto a2 = det->getSuccessor(1);

  oldvar->removeIncomingValue( 1U );
  oldvar->removeIncomingValue( 0U );
  assert( oldvar->getNumUses() == 0 );

  if (llvm::verifyFunction(*Header->getParent(), nullptr)) {
    Header->getParent()->getParent()->dump();
  }
  assert(!llvm::verifyFunction(*Header->getParent(), &llvm::errs()));

  assert( det->use_empty() );

  det->eraseFromParent();
  if( countPredecessors(a2) == 0 ){
    auto tmp = a1;
    a1 = a2;
    a2 = tmp;
  }

  if (parentL) parentL->removeChildLoop( std::find(parentL->getSubLoops().begin(), parentL->getSubLoops().end(), L) );
  LI.removeBlock(a1);

  removeFromAll(parentL, a1);
  DeleteDeadBlock(a1);
  if (a1 != a2) {
    LI.removeBlock(a2);
    removeFromAll(parentL, a2);
    DeleteDeadBlock(a2);
  }

  assert( Header->getTerminator()->use_empty() );
  Header->getTerminator()->eraseFromParent();
  IRBuilder<> b2(Header);
  b2.CreateBr(detacher);
  IRBuilder<> b(detacher);

  llvm::Function* F;
  if( ((llvm::IntegerType*)cmp->getType())->getBitWidth() == 32 )
    F = CILKRTS_FUNC(cilk_for_32, *M);
  else {
    assert( ((llvm::IntegerType*)cmp->getType())->getBitWidth() == 64 );
    F = CILKRTS_FUNC(cilk_for_64, *M);
  }

  llvm::Value* args[] = { b.CreatePointerCast(extracted, F->getFunctionType()->getParamType(0) ), b.CreatePointerCast( closure, F->getFunctionType()->getParamType(1) ), cmp, ConstantInt::get( llvm::Type::getIntNTy( cmp->getContext(), 8*sizeof(int) ), 0 ) };
  b.CreateCall(F, args);

  assert (syncer->size() == 1);
  b.CreateBr(syncer);

  if (llvm::verifyFunction(*Header->getParent(), nullptr)) {
    llvm::errs() << "BAD\n";
    //Header->getParent()->dump();
  }
  assert(!llvm::verifyFunction(*Header->getParent(), &llvm::errs()));

  ScalarEvolution &SE = getAnalysis<ScalarEvolutionWrapperPass>().getSE();
  SE.forgetLoop(L);

  DT.recalculate(*Header->getParent());
  L->invalidate();

  if (parentL) parentL->verifyLoop();

  if (llvm::verifyFunction(*Header->getParent(), nullptr)) {
    Header->getParent()->getParent()->dump();
  }
  assert(!llvm::verifyFunction(*Header->getParent(), &llvm::errs()));
  return true;
}
