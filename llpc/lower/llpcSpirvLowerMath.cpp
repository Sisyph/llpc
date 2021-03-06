/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2021 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *
 **********************************************************************************************************************/
/**
 ***********************************************************************************************************************
 * @file  llpcSpirvLowerMath.cpp
 * @brief LLPC source file: implementations of Llpc::SpirvLowerMathConstFolding and Llpc::SpirvLowerMathFloatOp.
 ***********************************************************************************************************************
 */
#include "SPIRVInternal.h"
#include "hex_float.h"
#include "llpcContext.h"
#include "llpcSpirvLower.h"
#include "lgc/Builder.h"
#include "lgc/Pipeline.h"
#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils.h"
#include "llvm/Transforms/Utils/Local.h"

#define DEBUG_TYPE_CONST_FOLDING "llpc-spirv-lower-math-const-folding"
#define DEBUG_TYPE_FLOAT_OP "llpc-spirv-lower-math-float-op"

using namespace lgc;
using namespace llvm;
using namespace SPIRV;
using namespace Llpc;

namespace {

// =====================================================================================================================
// SPIR-V lowering operations for math transformation.
class SpirvLowerMath : public SpirvLower {
public:
  explicit SpirvLowerMath(char &ID)
      : SpirvLower(ID), m_changed(false), m_fp16DenormFlush(false), m_fp32DenormFlush(false), m_fp64DenormFlush(false),
        m_fp16RoundToZero(false) {}

  void init(llvm::Module &module);

  void flushDenormIfNeeded(llvm::Instruction *inst);
  bool isOperandNoContract(llvm::Value *operand);
  void disableFastMath(llvm::Value *value);

  bool m_changed;         // Whether the module is changed
  bool m_fp16DenormFlush; // Whether FP mode wants f16 denorms to be flushed to zero
  bool m_fp32DenormFlush; // Whether FP mode wants f32 denorms to be flushed to zero
  bool m_fp64DenormFlush; // Whether FP mode wants f64 denorms to be flushed to zero
  bool m_fp16RoundToZero; // Whether FP mode wants f16 round-to-zero

  SpirvLowerMath() = delete;
  SpirvLowerMath(const SpirvLowerMath &) = delete;
  SpirvLowerMath &operator=(const SpirvLowerMath &) = delete;
};

// =====================================================================================================================
// SPIR-V lowering operations for math constant folding.
class SpirvLowerMathConstFolding : public SpirvLowerMath {
public:
  SpirvLowerMathConstFolding() : SpirvLowerMath(ID) {}

  void getAnalysisUsage(llvm::AnalysisUsage &analysisUsage) const override {
    analysisUsage.addRequired<llvm::TargetLibraryInfoWrapperPass>();
  }

  virtual bool runOnModule(llvm::Module &module) override;

  static char ID;

  SpirvLowerMathConstFolding(const SpirvLowerMathConstFolding &) = delete;
  SpirvLowerMathConstFolding &operator=(const SpirvLowerMathConstFolding &) = delete;
};

// =====================================================================================================================
// SPIR-V lowering operations for math floating point optimisation.
class SpirvLowerMathFloatOp : public SpirvLowerMath, public llvm::InstVisitor<SpirvLowerMathFloatOp> {
public:
  SpirvLowerMathFloatOp() : SpirvLowerMath(ID) {}

  virtual bool runOnModule(llvm::Module &module) override;
  virtual void visitBinaryOperator(llvm::BinaryOperator &binaryOp);
  virtual void visitUnaryOperator(llvm::UnaryOperator &unaryOp);
  virtual void visitCallInst(llvm::CallInst &callInst);
  virtual void visitFPTruncInst(llvm::FPTruncInst &fptruncInst);

  static char ID;

  SpirvLowerMathFloatOp(const SpirvLowerMathFloatOp &) = delete;
  SpirvLowerMathFloatOp &operator=(const SpirvLowerMathFloatOp &) = delete;
};

} // anonymous namespace

// =====================================================================================================================
// Initializes static members.
char SpirvLowerMathConstFolding::ID = 0;
char SpirvLowerMathFloatOp::ID = 0;

// =====================================================================================================================
// Initialise transform class.
//
// @param [in/out] module : LLVM module to be run on
void SpirvLowerMath::init(Module &module) {
  SpirvLower::init(&module);
  m_changed = false;

  auto &commonShaderMode = m_context->getBuilder()->getCommonShaderMode();
  m_fp16DenormFlush = commonShaderMode.fp16DenormMode == FpDenormMode::FlushOut ||
                      commonShaderMode.fp16DenormMode == FpDenormMode::FlushInOut;
  m_fp32DenormFlush = commonShaderMode.fp32DenormMode == FpDenormMode::FlushOut ||
                      commonShaderMode.fp32DenormMode == FpDenormMode::FlushInOut;
  m_fp64DenormFlush = commonShaderMode.fp64DenormMode == FpDenormMode::FlushOut ||
                      commonShaderMode.fp64DenormMode == FpDenormMode::FlushInOut;
  m_fp16RoundToZero = commonShaderMode.fp16RoundMode == FpRoundMode::Zero;
}

// =====================================================================================================================
// Checks desired denormal flush behavior and inserts llvm.canonicalize.
//
// @param inst : Instruction to flush denormals if needed
void SpirvLowerMath::flushDenormIfNeeded(Instruction *inst) {
  auto destTy = inst->getType();
  if ((destTy->getScalarType()->isHalfTy() && m_fp16DenormFlush) ||
      (destTy->getScalarType()->isFloatTy() && m_fp32DenormFlush) ||
      (destTy->getScalarType()->isDoubleTy() && m_fp64DenormFlush)) {
    // Has to flush denormals, insert canonicalize to make a MUL (* 1.0) forcibly
    auto builder = m_context->getBuilder();
    builder->SetInsertPoint(inst->getNextNode());
    auto canonical = builder->CreateIntrinsic(Intrinsic::canonicalize, destTy, UndefValue::get(destTy));

    inst->replaceAllUsesWith(canonical);
    canonical->setArgOperand(0, inst);
    m_changed = true;
  }
}

// =====================================================================================================================
// Recursively finds backward if the FPMathOperator operand does not specifiy "contract" flag.
//
// @param operand : Operand to check
bool SpirvLowerMath::isOperandNoContract(Value *operand) {
  if (isa<BinaryOperator>(operand)) {
    auto inst = dyn_cast<BinaryOperator>(operand);

    if (isa<FPMathOperator>(operand)) {
      auto fastMathFlags = inst->getFastMathFlags();
      bool allowContract = fastMathFlags.allowContract();
      if (fastMathFlags.any() && !allowContract)
        return true;
    }

    for (auto opIt = inst->op_begin(), end = inst->op_end(); opIt != end; ++opIt)
      return isOperandNoContract(*opIt);
  }
  return false;
}

// =====================================================================================================================
// Disable fast math for all values related with the specified value
//
// @param value : Value to disable fast math
void SpirvLowerMath::disableFastMath(Value *value) {
  std::set<Instruction *> allValues;
  std::list<Instruction *> workSet;
  if (isa<Instruction>(value)) {
    allValues.insert(cast<Instruction>(value));
    workSet.push_back(cast<Instruction>(value));
  }

  auto it = workSet.begin();
  while (!workSet.empty()) {
    if (isa<FPMathOperator>(*it)) {
      // Reset fast math flags to default
      auto inst = cast<Instruction>(*it);
      FastMathFlags fastMathFlags;
      inst->copyFastMathFlags(fastMathFlags);
    }

    for (Value *operand : (*it)->operands()) {
      if (isa<Instruction>(operand)) {
        // Add new values
        auto inst = cast<Instruction>(operand);
        if (allValues.find(inst) == allValues.end()) {
          allValues.insert(inst);
          workSet.push_back(inst);
        }
      }
    }

    it = workSet.erase(it);
  }
}

#define DEBUG_TYPE DEBUG_TYPE_CONST_FOLDING

// =====================================================================================================================
// Executes constand folding SPIR-V lowering pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
bool SpirvLowerMathConstFolding::runOnModule(Module &module) {
  LLVM_DEBUG(dbgs() << "Run the pass Spirv-Lower-Math-Const-Folding\n");

  SpirvLowerMath::init(module);

  if (m_fp16DenormFlush || m_fp32DenormFlush || m_fp64DenormFlush) {
    // Do constant folding if we need flush denorm to zero.
    auto &targetLibInfo = getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(*m_entryPoint);
    auto &dataLayout = m_module->getDataLayout();

    for (auto &block : *m_entryPoint) {
      for (auto instIter = block.begin(), instEnd = block.end(); instIter != instEnd;) {
        Instruction *inst = &(*instIter++);

        // DCE instruction if trivially dead.
        if (isInstructionTriviallyDead(inst, &targetLibInfo)) {
          LLVM_DEBUG(dbgs() << "Algebriac transform: DCE: " << *inst << '\n');
          inst->eraseFromParent();
          m_changed = true;
          continue;
        }

        // Skip Constant folding if it isn't floating point const expression
        auto destType = inst->getType();
        if (inst->use_empty() || inst->getNumOperands() == 0 || !destType->isFPOrFPVectorTy() ||
            !isa<Constant>(inst->getOperand(0)))
          continue;

        // ConstantProp instruction if trivially constant.
        if (Constant *constVal = ConstantFoldInstruction(inst, dataLayout, &targetLibInfo)) {
          LLVM_DEBUG(dbgs() << "Algebriac transform: constant folding: " << *constVal << " from: " << *inst << '\n');
          if ((destType->isHalfTy() && m_fp16DenormFlush) || (destType->isFloatTy() && m_fp32DenormFlush) ||
              (destType->isDoubleTy() && m_fp64DenormFlush)) {
            // Replace denorm value with zero
            if (constVal->isFiniteNonZeroFP() && !constVal->isNormalFP())
              constVal = ConstantFP::get(destType, 0.0);
          }

          inst->replaceAllUsesWith(constVal);
          if (isInstructionTriviallyDead(inst, &targetLibInfo))
            inst->eraseFromParent();

          m_changed = true;
          continue;
        }
      }
    }
  }

  return m_changed;
}

#undef DEBUG_TYPE // DEBUG_TYPE_CONST_FOLDING
#define DEBUG_TYPE DEBUG_TYPE_FLOAT_OP

// =====================================================================================================================
// Executes floating point optimisation SPIR-V lowering pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
bool SpirvLowerMathFloatOp::runOnModule(Module &module) {
  LLVM_DEBUG(dbgs() << "Run the pass Spirv-Lower-Math-Float-Op\n");

  SpirvLowerMath::init(module);
  visit(m_module);

  return m_changed;
}

// =====================================================================================================================
// Visits unary operator instruction.
//
// @param unaryOp : Unary operator instruction
void SpirvLowerMathFloatOp::visitUnaryOperator(UnaryOperator &unaryOp) {
  if (unaryOp.getOpcode() == Instruction::FNeg)
    flushDenormIfNeeded(&unaryOp);
}

// =====================================================================================================================
// Visits binary operator instruction.
//
// @param binaryOp : Binary operator instruction
void SpirvLowerMathFloatOp::visitBinaryOperator(BinaryOperator &binaryOp) {
  Instruction::BinaryOps opCode = binaryOp.getOpcode();

  auto src1 = binaryOp.getOperand(0);
  auto src2 = binaryOp.getOperand(1);
  bool src1IsConstZero =
      isa<ConstantAggregateZero>(src1) || (isa<ConstantFP>(src1) && cast<ConstantFP>(src1)->isZero());
  bool src2IsConstZero =
      isa<ConstantAggregateZero>(src2) || (isa<ConstantFP>(src2) && cast<ConstantFP>(src2)->isZero());
  Value *dest = nullptr;

  if (opCode == Instruction::FAdd) {
    // Recursively find backward if the operand "does not" specify contract flags
    auto fastMathFlags = binaryOp.getFastMathFlags();
    if (fastMathFlags.allowContract()) {
      bool hasNoContract = isOperandNoContract(src1) || isOperandNoContract(src2);
      bool allowContract = !hasNoContract;

      // Reassocation and contract should be same
      fastMathFlags.setAllowReassoc(allowContract);
      fastMathFlags.setAllowContract(allowContract);
      binaryOp.copyFastMathFlags(fastMathFlags);
    }
  } else if (opCode == Instruction::FSub) {
    if (src1IsConstZero) {
      // NOTE: Source1 is constant zero, we might be performing FNEG operation. This will be optimized
      // by backend compiler with sign bit reversed via XOR. Check floating-point controls.
      flushDenormIfNeeded(&binaryOp);
    }
  }

  // NOTE: We can't do constant folding for the following floating operations if we have floating-point controls that
  // will flush denormals or preserve NaN.
  if (!m_fp16DenormFlush && !m_fp32DenormFlush && !m_fp64DenormFlush) {
    switch (opCode) {
    case Instruction::FAdd:
      if (binaryOp.getFastMathFlags().noNaNs()) {
        if (src1IsConstZero)
          dest = src2;
        else if (src2IsConstZero)
          dest = src1;
      }
      break;
    case Instruction::FMul:
      if (binaryOp.getFastMathFlags().noNaNs()) {
        if (src1IsConstZero)
          dest = src1;
        else if (src2IsConstZero)
          dest = src2;
      }
      break;
    case Instruction::FDiv:
      if (binaryOp.getFastMathFlags().noNaNs()) {
        if (src1IsConstZero && !src2IsConstZero)
          dest = src1;
      }
      break;
    case Instruction::FSub:
      if (binaryOp.getFastMathFlags().noNaNs()) {
        if (src2IsConstZero)
          dest = src1;
      }
      break;
    default:
      break;
    }

    if (dest) {
      binaryOp.replaceAllUsesWith(dest);
      binaryOp.dropAllReferences();
      binaryOp.eraseFromParent();

      m_changed = true;
    }
  }

  // Replace FDIV x, y with FDIV 1.0, y; MUL x if it isn't optimized
  if (opCode == Instruction::FDiv && !dest && src1 && src2) {
    Constant *one = ConstantFP::get(binaryOp.getType(), 1.0);
    if (src1 != one) {
      IRBuilder<> builder(*m_context);
      builder.SetInsertPoint(&binaryOp);
      builder.setFastMathFlags(binaryOp.getFastMathFlags());
      Value *rcp = builder.CreateFDiv(ConstantFP::get(binaryOp.getType(), 1.0), src2);
      Value *fDiv = builder.CreateFMul(src1, rcp);

      binaryOp.replaceAllUsesWith(fDiv);
      binaryOp.dropAllReferences();
      binaryOp.eraseFromParent();

      m_changed = true;
    }
  }
}

// =====================================================================================================================
// Visits call instruction.
//
// @param callInst : Call instruction
void SpirvLowerMathFloatOp::visitCallInst(CallInst &callInst) {
  auto callee = callInst.getCalledFunction();
  if (!callee)
    return;

  if (callee->isIntrinsic() && callee->getIntrinsicID() == Intrinsic::fabs) {
    // NOTE: FABS will be optimized by backend compiler with sign bit removed via AND.
    flushDenormIfNeeded(&callInst);
  } else {
    // Disable fast math for gl_Position.
    // TODO: Having this here is not good, as it requires us to know implementation details of Builder.
    // We need to find a neater way to do it.
    auto calleeName = callee->getName();
    unsigned builtIn = InvalidValue;
    Value *valueWritten = nullptr;
    if (calleeName.startswith("lgc.output.export.builtin.")) {
      builtIn = cast<ConstantInt>(callInst.getOperand(0))->getZExtValue();
      valueWritten = callInst.getOperand(callInst.getNumArgOperands() - 1);
    } else if (calleeName.startswith("lgc.create.write.builtin")) {
      builtIn = cast<ConstantInt>(callInst.getOperand(1))->getZExtValue();
      valueWritten = callInst.getOperand(0);
    }
    if (builtIn == lgc::BuiltInPosition)
      disableFastMath(valueWritten);
  }
}

// =====================================================================================================================
// Visits fptrunc instruction.
//
// @param fptruncInst : Fptrunc instruction
void SpirvLowerMathFloatOp::visitFPTruncInst(FPTruncInst &fptruncInst) {
  if (m_fp16RoundToZero) {
    auto src = fptruncInst.getOperand(0);
    auto srcTy = src->getType();
    auto destTy = fptruncInst.getDestTy();

    if (srcTy->getScalarType()->isDoubleTy() && destTy->getScalarType()->isHalfTy()) {
      // NOTE: doubel -> float16 conversion is done in backend compiler with RTE rounding. Thus, we have to split
      // it with two phases to disable such lowering if we need RTZ rounding.
      auto floatTy = srcTy->isVectorTy() ? FixedVectorType::get(Type::getFloatTy(*m_context),
                                                                cast<FixedVectorType>(srcTy)->getNumElements())
                                         : Type::getFloatTy(*m_context);
      auto floatValue = new FPTruncInst(src, floatTy, "", &fptruncInst);
      auto dest = new FPTruncInst(floatValue, destTy, "", &fptruncInst);

      fptruncInst.replaceAllUsesWith(dest);
      fptruncInst.dropAllReferences();
      fptruncInst.eraseFromParent();

      m_changed = true;
    }
  }
}

#undef DEBUG_TYPE // DEBUG_TYPE_FLOAT_OP

// =====================================================================================================================
// Initializes SPIR-V lowering - math constant folding.
INITIALIZE_PASS(SpirvLowerMathConstFolding, DEBUG_TYPE_CONST_FOLDING, "Lower SPIR-V math constant folding", false,
                false)

// =====================================================================================================================
// Initializes SPIR-V lowering - math constant folding.
INITIALIZE_PASS(SpirvLowerMathFloatOp, DEBUG_TYPE_FLOAT_OP, "Lower SPIR-V math floating point optimisation", false,
                false)

namespace Llpc {

// =====================================================================================================================
// Pass creator, SPIR-V lowering for math constant folding.
ModulePass *createSpirvLowerMathConstFolding() {
  return new SpirvLowerMathConstFolding();
}

// =====================================================================================================================
// Pass creator, SPIR-V lowering for math floating point optimisation.
ModulePass *createSpirvLowerMathFloatOp() {
  return new SpirvLowerMathFloatOp();
}

} // namespace Llpc
