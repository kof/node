// Copyright 2011 the V8 project authors. All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//     * Neither the name of Google Inc. nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "v8.h"

#if defined(V8_TARGET_ARCH_X64)

#include "x64/lithium-codegen-x64.h"
#include "code-stubs.h"
#include "stub-cache.h"

namespace v8 {
namespace internal {


#define __ masm()->

bool LCodeGen::GenerateCode() {
  HPhase phase("Code generation", chunk());
  ASSERT(is_unused());
  status_ = GENERATING;
  return GeneratePrologue() &&
      GenerateBody() &&
      GenerateDeferredCode() &&
      GenerateSafepointTable();
}


void LCodeGen::FinishCode(Handle<Code> code) {
  ASSERT(is_done());
  code->set_stack_slots(StackSlotCount());
  code->set_safepoint_table_start(safepoints_.GetCodeOffset());
  PopulateDeoptimizationData(code);
}


void LCodeGen::Abort(const char* format, ...) {
  if (FLAG_trace_bailout) {
    SmartPointer<char> debug_name = graph()->debug_name()->ToCString();
    PrintF("Aborting LCodeGen in @\"%s\": ", *debug_name);
    va_list arguments;
    va_start(arguments, format);
    OS::VPrint(format, arguments);
    va_end(arguments);
    PrintF("\n");
  }
  status_ = ABORTED;
}


void LCodeGen::Comment(const char* format, ...) {
  if (!FLAG_code_comments) return;
  char buffer[4 * KB];
  StringBuilder builder(buffer, ARRAY_SIZE(buffer));
  va_list arguments;
  va_start(arguments, format);
  builder.AddFormattedList(format, arguments);
  va_end(arguments);

  // Copy the string before recording it in the assembler to avoid
  // issues when the stack allocated buffer goes out of scope.
  int length = builder.position();
  Vector<char> copy = Vector<char>::New(length + 1);
  memcpy(copy.start(), builder.Finalize(), copy.length());
  masm()->RecordComment(copy.start());
}


bool LCodeGen::GeneratePrologue() {
  ASSERT(is_generating());

#ifdef DEBUG
  if (strlen(FLAG_stop_at) > 0 &&
      info_->function()->name()->IsEqualTo(CStrVector(FLAG_stop_at))) {
    __ int3();
  }
#endif

  __ push(rbp);  // Caller's frame pointer.
  __ movq(rbp, rsp);
  __ push(rsi);  // Callee's context.
  __ push(rdi);  // Callee's JS function.

  // Reserve space for the stack slots needed by the code.
  int slots = StackSlotCount();
  if (slots > 0) {
    if (FLAG_debug_code) {
      __ movl(rax, Immediate(slots));
      __ movq(kScratchRegister, kSlotsZapValue, RelocInfo::NONE);
      Label loop;
      __ bind(&loop);
      __ push(kScratchRegister);
      __ decl(rax);
      __ j(not_zero, &loop);
    } else {
      __ subq(rsp, Immediate(slots * kPointerSize));
#ifdef _MSC_VER
      // On windows, you may not access the stack more than one page below
      // the most recently mapped page. To make the allocated area randomly
      // accessible, we write to each page in turn (the value is irrelevant).
      const int kPageSize = 4 * KB;
      for (int offset = slots * kPointerSize - kPageSize;
           offset > 0;
           offset -= kPageSize) {
        __ movq(Operand(rsp, offset), rax);
      }
#endif
    }
  }

  // Trace the call.
  if (FLAG_trace) {
    __ CallRuntime(Runtime::kTraceEnter, 0);
  }
  return !is_aborted();
}


bool LCodeGen::GenerateBody() {
  ASSERT(is_generating());
  bool emit_instructions = true;
  for (current_instruction_ = 0;
       !is_aborted() && current_instruction_ < instructions_->length();
       current_instruction_++) {
    LInstruction* instr = instructions_->at(current_instruction_);
    if (instr->IsLabel()) {
      LLabel* label = LLabel::cast(instr);
      emit_instructions = !label->HasReplacement();
    }

    if (emit_instructions) {
      Comment(";;; @%d: %s.", current_instruction_, instr->Mnemonic());
      instr->CompileToNative(this);
    }
  }
  return !is_aborted();
}


LInstruction* LCodeGen::GetNextInstruction() {
  if (current_instruction_ < instructions_->length() - 1) {
    return instructions_->at(current_instruction_ + 1);
  } else {
    return NULL;
  }
}


bool LCodeGen::GenerateDeferredCode() {
  ASSERT(is_generating());
  for (int i = 0; !is_aborted() && i < deferred_.length(); i++) {
    LDeferredCode* code = deferred_[i];
    __ bind(code->entry());
    code->Generate();
    __ jmp(code->exit());
  }

  // Deferred code is the last part of the instruction sequence. Mark
  // the generated code as done unless we bailed out.
  if (!is_aborted()) status_ = DONE;
  return !is_aborted();
}


bool LCodeGen::GenerateSafepointTable() {
  ASSERT(is_done());
  // Ensure that patching a deoptimization point won't overwrite the table.
  for (int i = 0; i < Assembler::kCallInstructionLength; i++) {
    masm()->int3();
  }
  safepoints_.Emit(masm(), StackSlotCount());
  return !is_aborted();
}


Register LCodeGen::ToRegister(int index) const {
  return Register::FromAllocationIndex(index);
}


XMMRegister LCodeGen::ToDoubleRegister(int index) const {
  return XMMRegister::FromAllocationIndex(index);
}


Register LCodeGen::ToRegister(LOperand* op) const {
  ASSERT(op->IsRegister());
  return ToRegister(op->index());
}


XMMRegister LCodeGen::ToDoubleRegister(LOperand* op) const {
  ASSERT(op->IsDoubleRegister());
  return ToDoubleRegister(op->index());
}


bool LCodeGen::IsInteger32Constant(LConstantOperand* op) const {
  return op->IsConstantOperand() &&
      chunk_->LookupLiteralRepresentation(op).IsInteger32();
}


bool LCodeGen::IsTaggedConstant(LConstantOperand* op) const {
  return op->IsConstantOperand() &&
      chunk_->LookupLiteralRepresentation(op).IsTagged();
}


int LCodeGen::ToInteger32(LConstantOperand* op) const {
  Handle<Object> value = chunk_->LookupLiteral(op);
  ASSERT(chunk_->LookupLiteralRepresentation(op).IsInteger32());
  ASSERT(static_cast<double>(static_cast<int32_t>(value->Number())) ==
      value->Number());
  return static_cast<int32_t>(value->Number());
}


Handle<Object> LCodeGen::ToHandle(LConstantOperand* op) const {
  Handle<Object> literal = chunk_->LookupLiteral(op);
  Representation r = chunk_->LookupLiteralRepresentation(op);
  ASSERT(r.IsTagged());
  return literal;
}


Operand LCodeGen::ToOperand(LOperand* op) const {
  // Does not handle registers. In X64 assembler, plain registers are not
  // representable as an Operand.
  ASSERT(op->IsStackSlot() || op->IsDoubleStackSlot());
  int index = op->index();
  if (index >= 0) {
    // Local or spill slot. Skip the frame pointer, function, and
    // context in the fixed part of the frame.
    return Operand(rbp, -(index + 3) * kPointerSize);
  } else {
    // Incoming parameter. Skip the return address.
    return Operand(rbp, -(index - 1) * kPointerSize);
  }
}


void LCodeGen::WriteTranslation(LEnvironment* environment,
                                Translation* translation) {
  if (environment == NULL) return;

  // The translation includes one command per value in the environment.
  int translation_size = environment->values()->length();
  // The output frame height does not include the parameters.
  int height = translation_size - environment->parameter_count();

  WriteTranslation(environment->outer(), translation);
  int closure_id = DefineDeoptimizationLiteral(environment->closure());
  translation->BeginFrame(environment->ast_id(), closure_id, height);
  for (int i = 0; i < translation_size; ++i) {
    LOperand* value = environment->values()->at(i);
    // spilled_registers_ and spilled_double_registers_ are either
    // both NULL or both set.
    if (environment->spilled_registers() != NULL && value != NULL) {
      if (value->IsRegister() &&
          environment->spilled_registers()[value->index()] != NULL) {
        translation->MarkDuplicate();
        AddToTranslation(translation,
                         environment->spilled_registers()[value->index()],
                         environment->HasTaggedValueAt(i));
      } else if (
          value->IsDoubleRegister() &&
          environment->spilled_double_registers()[value->index()] != NULL) {
        translation->MarkDuplicate();
        AddToTranslation(
            translation,
            environment->spilled_double_registers()[value->index()],
            false);
      }
    }

    AddToTranslation(translation, value, environment->HasTaggedValueAt(i));
  }
}


void LCodeGen::AddToTranslation(Translation* translation,
                                LOperand* op,
                                bool is_tagged) {
  if (op == NULL) {
    // TODO(twuerthinger): Introduce marker operands to indicate that this value
    // is not present and must be reconstructed from the deoptimizer. Currently
    // this is only used for the arguments object.
    translation->StoreArgumentsObject();
  } else if (op->IsStackSlot()) {
    if (is_tagged) {
      translation->StoreStackSlot(op->index());
    } else {
      translation->StoreInt32StackSlot(op->index());
    }
  } else if (op->IsDoubleStackSlot()) {
    translation->StoreDoubleStackSlot(op->index());
  } else if (op->IsArgument()) {
    ASSERT(is_tagged);
    int src_index = StackSlotCount() + op->index();
    translation->StoreStackSlot(src_index);
  } else if (op->IsRegister()) {
    Register reg = ToRegister(op);
    if (is_tagged) {
      translation->StoreRegister(reg);
    } else {
      translation->StoreInt32Register(reg);
    }
  } else if (op->IsDoubleRegister()) {
    XMMRegister reg = ToDoubleRegister(op);
    translation->StoreDoubleRegister(reg);
  } else if (op->IsConstantOperand()) {
    Handle<Object> literal = chunk()->LookupLiteral(LConstantOperand::cast(op));
    int src_index = DefineDeoptimizationLiteral(literal);
    translation->StoreLiteral(src_index);
  } else {
    UNREACHABLE();
  }
}


void LCodeGen::CallCode(Handle<Code> code,
                        RelocInfo::Mode mode,
                        LInstruction* instr) {
  if (instr != NULL) {
    LPointerMap* pointers = instr->pointer_map();
    RecordPosition(pointers->position());
    __ call(code, mode);
    RegisterLazyDeoptimization(instr);
  } else {
    LPointerMap no_pointers(0);
    RecordPosition(no_pointers.position());
    __ call(code, mode);
    RecordSafepoint(&no_pointers, Safepoint::kNoDeoptimizationIndex);
  }

  // Signal that we don't inline smi code before these stubs in the
  // optimizing code generator.
  if (code->kind() == Code::TYPE_RECORDING_BINARY_OP_IC ||
      code->kind() == Code::COMPARE_IC) {
    __ nop();
  }
}


void LCodeGen::CallRuntime(Runtime::Function* function,
                           int num_arguments,
                           LInstruction* instr) {
  Abort("Unimplemented: %s", "CallRuntime");
}


void LCodeGen::RegisterLazyDeoptimization(LInstruction* instr) {
  // Create the environment to bailout to. If the call has side effects
  // execution has to continue after the call otherwise execution can continue
  // from a previous bailout point repeating the call.
  LEnvironment* deoptimization_environment;
  if (instr->HasDeoptimizationEnvironment()) {
    deoptimization_environment = instr->deoptimization_environment();
  } else {
    deoptimization_environment = instr->environment();
  }

  RegisterEnvironmentForDeoptimization(deoptimization_environment);
  RecordSafepoint(instr->pointer_map(),
                  deoptimization_environment->deoptimization_index());
}


void LCodeGen::RegisterEnvironmentForDeoptimization(LEnvironment* environment) {
  if (!environment->HasBeenRegistered()) {
    // Physical stack frame layout:
    // -x ............. -4  0 ..................................... y
    // [incoming arguments] [spill slots] [pushed outgoing arguments]

    // Layout of the environment:
    // 0 ..................................................... size-1
    // [parameters] [locals] [expression stack including arguments]

    // Layout of the translation:
    // 0 ........................................................ size - 1 + 4
    // [expression stack including arguments] [locals] [4 words] [parameters]
    // |>------------  translation_size ------------<|

    int frame_count = 0;
    for (LEnvironment* e = environment; e != NULL; e = e->outer()) {
      ++frame_count;
    }
    Translation translation(&translations_, frame_count);
    WriteTranslation(environment, &translation);
    int deoptimization_index = deoptimizations_.length();
    environment->Register(deoptimization_index, translation.index());
    deoptimizations_.Add(environment);
  }
}


void LCodeGen::DeoptimizeIf(Condition cc, LEnvironment* environment) {
  RegisterEnvironmentForDeoptimization(environment);
  ASSERT(environment->HasBeenRegistered());
  int id = environment->deoptimization_index();
  Address entry = Deoptimizer::GetDeoptimizationEntry(id, Deoptimizer::EAGER);
  ASSERT(entry != NULL);
  if (entry == NULL) {
    Abort("bailout was not prepared");
    return;
  }

  if (cc == no_condition) {
    __ Jump(entry, RelocInfo::RUNTIME_ENTRY);
  } else {
    NearLabel done;
    __ j(NegateCondition(cc), &done);
    __ Jump(entry, RelocInfo::RUNTIME_ENTRY);
    __ bind(&done);
  }
}


void LCodeGen::PopulateDeoptimizationData(Handle<Code> code) {
  int length = deoptimizations_.length();
  if (length == 0) return;
  ASSERT(FLAG_deopt);
  Handle<DeoptimizationInputData> data =
      Factory::NewDeoptimizationInputData(length, TENURED);

  data->SetTranslationByteArray(*translations_.CreateByteArray());
  data->SetInlinedFunctionCount(Smi::FromInt(inlined_function_count_));

  Handle<FixedArray> literals =
      Factory::NewFixedArray(deoptimization_literals_.length(), TENURED);
  for (int i = 0; i < deoptimization_literals_.length(); i++) {
    literals->set(i, *deoptimization_literals_[i]);
  }
  data->SetLiteralArray(*literals);

  data->SetOsrAstId(Smi::FromInt(info_->osr_ast_id()));
  data->SetOsrPcOffset(Smi::FromInt(osr_pc_offset_));

  // Populate the deoptimization entries.
  for (int i = 0; i < length; i++) {
    LEnvironment* env = deoptimizations_[i];
    data->SetAstId(i, Smi::FromInt(env->ast_id()));
    data->SetTranslationIndex(i, Smi::FromInt(env->translation_index()));
    data->SetArgumentsStackHeight(i,
                                  Smi::FromInt(env->arguments_stack_height()));
  }
  code->set_deoptimization_data(*data);
}


int LCodeGen::DefineDeoptimizationLiteral(Handle<Object> literal) {
  int result = deoptimization_literals_.length();
  for (int i = 0; i < deoptimization_literals_.length(); ++i) {
    if (deoptimization_literals_[i].is_identical_to(literal)) return i;
  }
  deoptimization_literals_.Add(literal);
  return result;
}


void LCodeGen::PopulateDeoptimizationLiteralsWithInlinedFunctions() {
  ASSERT(deoptimization_literals_.length() == 0);

  const ZoneList<Handle<JSFunction> >* inlined_closures =
      chunk()->inlined_closures();

  for (int i = 0, length = inlined_closures->length();
       i < length;
       i++) {
    DefineDeoptimizationLiteral(inlined_closures->at(i));
  }

  inlined_function_count_ = deoptimization_literals_.length();
}


void LCodeGen::RecordSafepoint(
    LPointerMap* pointers,
    Safepoint::Kind kind,
    int arguments,
    int deoptimization_index) {
  const ZoneList<LOperand*>* operands = pointers->operands();
  Safepoint safepoint = safepoints_.DefineSafepoint(masm(),
      kind, arguments, deoptimization_index);
  for (int i = 0; i < operands->length(); i++) {
    LOperand* pointer = operands->at(i);
    if (pointer->IsStackSlot()) {
      safepoint.DefinePointerSlot(pointer->index());
    } else if (pointer->IsRegister() && (kind & Safepoint::kWithRegisters)) {
      safepoint.DefinePointerRegister(ToRegister(pointer));
    }
  }
  if (kind & Safepoint::kWithRegisters) {
    // Register rsi always contains a pointer to the context.
    safepoint.DefinePointerRegister(rsi);
  }
}


void LCodeGen::RecordSafepoint(LPointerMap* pointers,
                               int deoptimization_index) {
  RecordSafepoint(pointers, Safepoint::kSimple, 0, deoptimization_index);
}


void LCodeGen::RecordSafepointWithRegisters(LPointerMap* pointers,
                                            int arguments,
                                            int deoptimization_index) {
  RecordSafepoint(pointers, Safepoint::kWithRegisters, arguments,
      deoptimization_index);
}


void LCodeGen::RecordPosition(int position) {
  if (!FLAG_debug_info || position == RelocInfo::kNoPosition) return;
  masm()->positions_recorder()->RecordPosition(position);
}


void LCodeGen::DoLabel(LLabel* label) {
  if (label->is_loop_header()) {
    Comment(";;; B%d - LOOP entry", label->block_id());
  } else {
    Comment(";;; B%d", label->block_id());
  }
  __ bind(label->label());
  current_block_ = label->block_id();
  LCodeGen::DoGap(label);
}


void LCodeGen::DoParallelMove(LParallelMove* move) {
  resolver_.Resolve(move);
}


void LCodeGen::DoGap(LGap* gap) {
  for (int i = LGap::FIRST_INNER_POSITION;
       i <= LGap::LAST_INNER_POSITION;
       i++) {
    LGap::InnerPosition inner_pos = static_cast<LGap::InnerPosition>(i);
    LParallelMove* move = gap->GetParallelMove(inner_pos);
    if (move != NULL) DoParallelMove(move);
  }

  LInstruction* next = GetNextInstruction();
  if (next != NULL && next->IsLazyBailout()) {
    int pc = masm()->pc_offset();
    safepoints_.SetPcAfterGap(pc);
  }
}


void LCodeGen::DoParameter(LParameter* instr) {
  // Nothing to do.
}


void LCodeGen::DoCallStub(LCallStub* instr) {
  Abort("Unimplemented: %s", "DoCallStub");
}


void LCodeGen::DoUnknownOSRValue(LUnknownOSRValue* instr) {
  // Nothing to do.
}


void LCodeGen::DoModI(LModI* instr) {
  Abort("Unimplemented: %s", "DoModI");
}


void LCodeGen::DoDivI(LDivI* instr) {
  Abort("Unimplemented: %s", "DoDivI");}


void LCodeGen::DoMulI(LMulI* instr) {
  Abort("Unimplemented: %s", "DoMultI");}


void LCodeGen::DoBitI(LBitI* instr) {
  Abort("Unimplemented: %s", "DoBitI");}


void LCodeGen::DoShiftI(LShiftI* instr) {
  Abort("Unimplemented: %s", "DoShiftI");
}


void LCodeGen::DoSubI(LSubI* instr) {
  LOperand* left = instr->InputAt(0);
  LOperand* right = instr->InputAt(1);
  ASSERT(left->Equals(instr->result()));

  if (right->IsConstantOperand()) {
    __ subl(ToRegister(left),
            Immediate(ToInteger32(LConstantOperand::cast(right))));
  } else if (right->IsRegister()) {
    __ subl(ToRegister(left), ToRegister(right));
  } else {
    __ subl(ToRegister(left), ToOperand(right));
  }

  if (instr->hydrogen()->CheckFlag(HValue::kCanOverflow)) {
    DeoptimizeIf(overflow, instr->environment());
  }
}


void LCodeGen::DoConstantI(LConstantI* instr) {
  ASSERT(instr->result()->IsRegister());
  __ movl(ToRegister(instr->result()), Immediate(instr->value()));
}


void LCodeGen::DoConstantD(LConstantD* instr) {
  ASSERT(instr->result()->IsDoubleRegister());
  XMMRegister res = ToDoubleRegister(instr->result());
  double v = instr->value();
  // Use xor to produce +0.0 in a fast and compact way, but avoid to
  // do so if the constant is -0.0.
  if (BitCast<uint64_t, double>(v) == 0) {
    __ xorpd(res, res);
  } else {
    Register tmp = ToRegister(instr->TempAt(0));
    int32_t v_int32 = static_cast<int32_t>(v);
    if (static_cast<double>(v_int32) == v) {
      __ movl(tmp, Immediate(v_int32));
      __ cvtlsi2sd(res, tmp);
    } else {
      uint64_t int_val = BitCast<uint64_t, double>(v);
      __ Set(tmp, int_val);
      __ movd(res, tmp);
    }
  }
}


void LCodeGen::DoConstantT(LConstantT* instr) {
    ASSERT(instr->result()->IsRegister());
  __ Move(ToRegister(instr->result()), instr->value());
}


void LCodeGen::DoJSArrayLength(LJSArrayLength* instr) {
  Abort("Unimplemented: %s", "DoJSArrayLength");
}


void LCodeGen::DoFixedArrayLength(LFixedArrayLength* instr) {
  Abort("Unimplemented: %s", "DoFixedArrayLength");
}


void LCodeGen::DoValueOf(LValueOf* instr) {
  Abort("Unimplemented: %s", "DoValueOf");
}


void LCodeGen::DoBitNotI(LBitNotI* instr) {
  Abort("Unimplemented: %s", "DoBitNotI");
}


void LCodeGen::DoThrow(LThrow* instr) {
  Abort("Unimplemented: %s", "DoThrow");
}


void LCodeGen::DoAddI(LAddI* instr) {
  LOperand* left = instr->InputAt(0);
  LOperand* right = instr->InputAt(1);
  ASSERT(left->Equals(instr->result()));

  if (right->IsConstantOperand()) {
    __ addl(ToRegister(left),
            Immediate(ToInteger32(LConstantOperand::cast(right))));
  } else if (right->IsRegister()) {
    __ addl(ToRegister(left), ToRegister(right));
  } else {
    __ addl(ToRegister(left), ToOperand(right));
  }

  if (instr->hydrogen()->CheckFlag(HValue::kCanOverflow)) {
    DeoptimizeIf(overflow, instr->environment());
  }
}


void LCodeGen::DoArithmeticD(LArithmeticD* instr) {
  Abort("Unimplemented: %s", "DoArithmeticD");
}


void LCodeGen::DoArithmeticT(LArithmeticT* instr) {
  ASSERT(ToRegister(instr->InputAt(0)).is(rdx));
  ASSERT(ToRegister(instr->InputAt(1)).is(rax));
  ASSERT(ToRegister(instr->result()).is(rax));

  GenericBinaryOpStub stub(instr->op(), NO_OVERWRITE, NO_GENERIC_BINARY_FLAGS);
  stub.SetArgsInRegisters();
  CallCode(stub.GetCode(), RelocInfo::CODE_TARGET, instr);
}


int LCodeGen::GetNextEmittedBlock(int block) {
  for (int i = block + 1; i < graph()->blocks()->length(); ++i) {
    LLabel* label = chunk_->GetLabel(i);
    if (!label->HasReplacement()) return i;
  }
  return -1;
}


void LCodeGen::EmitBranch(int left_block, int right_block, Condition cc) {
  int next_block = GetNextEmittedBlock(current_block_);
  right_block = chunk_->LookupDestination(right_block);
  left_block = chunk_->LookupDestination(left_block);

  if (right_block == left_block) {
    EmitGoto(left_block);
  } else if (left_block == next_block) {
    __ j(NegateCondition(cc), chunk_->GetAssemblyLabel(right_block));
  } else if (right_block == next_block) {
    __ j(cc, chunk_->GetAssemblyLabel(left_block));
  } else {
    __ j(cc, chunk_->GetAssemblyLabel(left_block));
    if (cc != always) {
      __ jmp(chunk_->GetAssemblyLabel(right_block));
    }
  }
}


void LCodeGen::DoBranch(LBranch* instr) {
  int true_block = chunk_->LookupDestination(instr->true_block_id());
  int false_block = chunk_->LookupDestination(instr->false_block_id());

  Representation r = instr->hydrogen()->representation();
  if (r.IsInteger32()) {
    Register reg = ToRegister(instr->InputAt(0));
    __ testl(reg, reg);
    EmitBranch(true_block, false_block, not_zero);
  } else if (r.IsDouble()) {
    XMMRegister reg = ToDoubleRegister(instr->InputAt(0));
    __ xorpd(xmm0, xmm0);
    __ ucomisd(reg, xmm0);
    EmitBranch(true_block, false_block, not_equal);
  } else {
    ASSERT(r.IsTagged());
    Register reg = ToRegister(instr->InputAt(0));
    HType type = instr->hydrogen()->type();
    if (type.IsBoolean()) {
      __ Cmp(reg, Factory::true_value());
      EmitBranch(true_block, false_block, equal);
    } else if (type.IsSmi()) {
      __ SmiCompare(reg, Smi::FromInt(0));
      EmitBranch(true_block, false_block, not_equal);
    } else {
      Label* true_label = chunk_->GetAssemblyLabel(true_block);
      Label* false_label = chunk_->GetAssemblyLabel(false_block);

      __ CompareRoot(reg, Heap::kUndefinedValueRootIndex);
      __ j(equal, false_label);
      __ CompareRoot(reg, Heap::kTrueValueRootIndex);
      __ j(equal, true_label);
      __ CompareRoot(reg, Heap::kFalseValueRootIndex);
      __ j(equal, false_label);
      __ SmiCompare(reg, Smi::FromInt(0));
      __ j(equal, false_label);
      __ JumpIfSmi(reg, true_label);

      // Test for double values. Plus/minus zero and NaN are false.
      NearLabel call_stub;
      __ CompareRoot(FieldOperand(reg, HeapObject::kMapOffset),
                     Heap::kHeapNumberMapRootIndex);
      __ j(not_equal, &call_stub);

      // HeapNumber => false iff +0, -0, or NaN. These three cases set the
      // zero flag when compared to zero using ucomisd.
      __ xorpd(xmm0, xmm0);
      __ ucomisd(xmm0, FieldOperand(reg, HeapNumber::kValueOffset));
      __ j(zero, false_label);
      __ jmp(true_label);

      // The conversion stub doesn't cause garbage collections so it's
      // safe to not record a safepoint after the call.
      __ bind(&call_stub);
      ToBooleanStub stub;
      __ Pushad();
      __ push(reg);
      __ CallStub(&stub);
      __ testq(rax, rax);
      __ Popad();
      EmitBranch(true_block, false_block, not_zero);
    }
  }
}


void LCodeGen::EmitGoto(int block, LDeferredCode* deferred_stack_check) {
  block = chunk_->LookupDestination(block);
  int next_block = GetNextEmittedBlock(current_block_);
  if (block != next_block) {
    // Perform stack overflow check if this goto needs it before jumping.
    if (deferred_stack_check != NULL) {
      __ CompareRoot(rsp, Heap::kStackLimitRootIndex);
      __ j(above_equal, chunk_->GetAssemblyLabel(block));
      __ jmp(deferred_stack_check->entry());
      deferred_stack_check->SetExit(chunk_->GetAssemblyLabel(block));
    } else {
      __ jmp(chunk_->GetAssemblyLabel(block));
    }
  }
}


void LCodeGen::DoDeferredStackCheck(LGoto* instr) {
  Abort("Unimplemented: %s", "DoDeferredStackCheck");
}


void LCodeGen::DoGoto(LGoto* instr) {
  class DeferredStackCheck: public LDeferredCode {
   public:
    DeferredStackCheck(LCodeGen* codegen, LGoto* instr)
        : LDeferredCode(codegen), instr_(instr) { }
    virtual void Generate() { codegen()->DoDeferredStackCheck(instr_); }
   private:
    LGoto* instr_;
  };

  DeferredStackCheck* deferred = NULL;
  if (instr->include_stack_check()) {
    deferred = new DeferredStackCheck(this, instr);
  }
  EmitGoto(instr->block_id(), deferred);
}


inline Condition LCodeGen::TokenToCondition(Token::Value op, bool is_unsigned) {
  Condition cond = no_condition;
  switch (op) {
    case Token::EQ:
    case Token::EQ_STRICT:
      cond = equal;
      break;
    case Token::LT:
      cond = is_unsigned ? below : less;
      break;
    case Token::GT:
      cond = is_unsigned ? above : greater;
      break;
    case Token::LTE:
      cond = is_unsigned ? below_equal : less_equal;
      break;
    case Token::GTE:
      cond = is_unsigned ? above_equal : greater_equal;
      break;
    case Token::IN:
    case Token::INSTANCEOF:
    default:
      UNREACHABLE();
  }
  return cond;
}


void LCodeGen::EmitCmpI(LOperand* left, LOperand* right) {
  if (right->IsConstantOperand()) {
    int32_t value = ToInteger32(LConstantOperand::cast(right));
    if (left->IsRegister()) {
      __ cmpl(ToRegister(left), Immediate(value));
    } else {
      __ cmpl(ToOperand(left), Immediate(value));
    }
  } else if (right->IsRegister()) {
    __ cmpq(ToRegister(left), ToRegister(right));
  } else {
    __ cmpq(ToRegister(left), ToOperand(right));
  }
}


void LCodeGen::DoCmpID(LCmpID* instr) {
  LOperand* left = instr->InputAt(0);
  LOperand* right = instr->InputAt(1);
  LOperand* result = instr->result();

  NearLabel unordered;
  if (instr->is_double()) {
    // Don't base result on EFLAGS when a NaN is involved. Instead
    // jump to the unordered case, which produces a false value.
    __ ucomisd(ToDoubleRegister(left), ToDoubleRegister(right));
    __ j(parity_even, &unordered);
  } else {
    EmitCmpI(left, right);
  }

  NearLabel done;
  Condition cc = TokenToCondition(instr->op(), instr->is_double());
  __ LoadRoot(ToRegister(result), Heap::kTrueValueRootIndex);
  __ j(cc, &done);

  __ bind(&unordered);
  __ LoadRoot(ToRegister(result), Heap::kFalseValueRootIndex);
  __ bind(&done);
}


void LCodeGen::DoCmpIDAndBranch(LCmpIDAndBranch* instr) {
  LOperand* left = instr->InputAt(0);
  LOperand* right = instr->InputAt(1);
  int false_block = chunk_->LookupDestination(instr->false_block_id());
  int true_block = chunk_->LookupDestination(instr->true_block_id());

  if (instr->is_double()) {
    // Don't base result on EFLAGS when a NaN is involved. Instead
    // jump to the false block.
    __ ucomisd(ToDoubleRegister(left), ToDoubleRegister(right));
    __ j(parity_even, chunk_->GetAssemblyLabel(false_block));
  } else {
    EmitCmpI(left, right);
  }

  Condition cc = TokenToCondition(instr->op(), instr->is_double());
  EmitBranch(true_block, false_block, cc);
}


void LCodeGen::DoCmpJSObjectEq(LCmpJSObjectEq* instr) {
  Register left = ToRegister(instr->InputAt(0));
  Register right = ToRegister(instr->InputAt(1));
  Register result = ToRegister(instr->result());

  NearLabel different, done;
  __ cmpq(left, right);
  __ j(not_equal, &different);
  __ LoadRoot(result, Heap::kTrueValueRootIndex);
  __ jmp(&done);
  __ bind(&different);
  __ LoadRoot(result, Heap::kFalseValueRootIndex);
  __ bind(&done);
}


void LCodeGen::DoCmpJSObjectEqAndBranch(LCmpJSObjectEqAndBranch* instr) {
  Register left = ToRegister(instr->InputAt(0));
  Register right = ToRegister(instr->InputAt(1));
  int false_block = chunk_->LookupDestination(instr->false_block_id());
  int true_block = chunk_->LookupDestination(instr->true_block_id());

  __ cmpq(left, right);
  EmitBranch(true_block, false_block, equal);
}


void LCodeGen::DoIsNull(LIsNull* instr) {
  Register reg = ToRegister(instr->InputAt(0));
  Register result = ToRegister(instr->result());

  // If the expression is known to be a smi, then it's
  // definitely not null. Materialize false.
  // Consider adding other type and representation tests too.
  if (instr->hydrogen()->value()->type().IsSmi()) {
    __ LoadRoot(result, Heap::kFalseValueRootIndex);
    return;
  }

  __ CompareRoot(reg, Heap::kNullValueRootIndex);
  if (instr->is_strict()) {
    __ movl(result, Immediate(Heap::kTrueValueRootIndex));
    NearLabel load;
    __ j(equal, &load);
    __ movl(result, Immediate(Heap::kFalseValueRootIndex));
    __ bind(&load);
    __ movq(result, Operand(kRootRegister, result, times_pointer_size, 0));
  } else {
    NearLabel true_value, false_value, done;
    __ j(equal, &true_value);
    __ CompareRoot(reg, Heap::kUndefinedValueRootIndex);
    __ j(equal, &true_value);
    __ JumpIfSmi(reg, &false_value);
    // Check for undetectable objects by looking in the bit field in
    // the map. The object has already been smi checked.
    Register scratch = result;
    __ movq(scratch, FieldOperand(reg, HeapObject::kMapOffset));
    __ testb(FieldOperand(scratch, Map::kBitFieldOffset),
             Immediate(1 << Map::kIsUndetectable));
    __ j(not_zero, &true_value);
    __ bind(&false_value);
    __ LoadRoot(result, Heap::kFalseValueRootIndex);
    __ jmp(&done);
    __ bind(&true_value);
    __ LoadRoot(result, Heap::kTrueValueRootIndex);
    __ bind(&done);
  }
}


void LCodeGen::DoIsNullAndBranch(LIsNullAndBranch* instr) {
  Register reg = ToRegister(instr->InputAt(0));

  int false_block = chunk_->LookupDestination(instr->false_block_id());

  if (instr->hydrogen()->representation().IsSpecialization() ||
      instr->hydrogen()->type().IsSmi()) {
    // If the expression is known to untagged or smi, then it's definitely
    // not null, and it can't be a an undetectable object.
    // Jump directly to the false block.
    EmitGoto(false_block);
    return;
  }

  int true_block = chunk_->LookupDestination(instr->true_block_id());

  __ Cmp(reg, Factory::null_value());
  if (instr->is_strict()) {
    EmitBranch(true_block, false_block, equal);
  } else {
    Label* true_label = chunk_->GetAssemblyLabel(true_block);
    Label* false_label = chunk_->GetAssemblyLabel(false_block);
    __ j(equal, true_label);
    __ Cmp(reg, Factory::undefined_value());
    __ j(equal, true_label);
    __ JumpIfSmi(reg, false_label);
    // Check for undetectable objects by looking in the bit field in
    // the map. The object has already been smi checked.
    Register scratch = ToRegister(instr->TempAt(0));
    __ movq(scratch, FieldOperand(reg, HeapObject::kMapOffset));
    __ testb(FieldOperand(scratch, Map::kBitFieldOffset),
             Immediate(1 << Map::kIsUndetectable));
    EmitBranch(true_block, false_block, not_zero);
  }
}


Condition LCodeGen::EmitIsObject(Register input,
                                 Label* is_not_object,
                                 Label* is_object) {
  ASSERT(!input.is(kScratchRegister));

  __ JumpIfSmi(input, is_not_object);

  __ CompareRoot(input, Heap::kNullValueRootIndex);
  __ j(equal, is_object);

  __ movq(kScratchRegister, FieldOperand(input, HeapObject::kMapOffset));
  // Undetectable objects behave like undefined.
  __ testb(FieldOperand(kScratchRegister, Map::kBitFieldOffset),
           Immediate(1 << Map::kIsUndetectable));
  __ j(not_zero, is_not_object);

  __ movzxbl(kScratchRegister,
             FieldOperand(kScratchRegister, Map::kInstanceTypeOffset));
  __ cmpb(kScratchRegister, Immediate(FIRST_JS_OBJECT_TYPE));
  __ j(below, is_not_object);
  __ cmpb(kScratchRegister, Immediate(LAST_JS_OBJECT_TYPE));
  return below_equal;
}


void LCodeGen::DoIsObject(LIsObject* instr) {
  Register reg = ToRegister(instr->InputAt(0));
  Register result = ToRegister(instr->result());
  Label is_false, is_true, done;

  Condition true_cond = EmitIsObject(reg, &is_false, &is_true);
  __ j(true_cond, &is_true);

  __ bind(&is_false);
  __ LoadRoot(result, Heap::kFalseValueRootIndex);
  __ jmp(&done);

  __ bind(&is_true);
  __ LoadRoot(result, Heap::kTrueValueRootIndex);

  __ bind(&done);
}


void LCodeGen::DoIsObjectAndBranch(LIsObjectAndBranch* instr) {
  Register reg = ToRegister(instr->InputAt(0));

  int true_block = chunk_->LookupDestination(instr->true_block_id());
  int false_block = chunk_->LookupDestination(instr->false_block_id());
  Label* true_label = chunk_->GetAssemblyLabel(true_block);
  Label* false_label = chunk_->GetAssemblyLabel(false_block);

  Condition true_cond = EmitIsObject(reg, false_label, true_label);

  EmitBranch(true_block, false_block, true_cond);
}


void LCodeGen::DoIsSmi(LIsSmi* instr) {
  LOperand* input_operand = instr->InputAt(0);
  Register result = ToRegister(instr->result());
  if (input_operand->IsRegister()) {
    Register input = ToRegister(input_operand);
    __ CheckSmiToIndicator(result, input);
  } else {
    Operand input = ToOperand(instr->InputAt(0));
    __ CheckSmiToIndicator(result, input);
  }
  // result is zero if input is a smi, and one otherwise.
  ASSERT(Heap::kFalseValueRootIndex == Heap::kTrueValueRootIndex + 1);
  __ movq(result, Operand(kRootRegister, result, times_pointer_size,
                          Heap::kTrueValueRootIndex * kPointerSize));
}


void LCodeGen::DoIsSmiAndBranch(LIsSmiAndBranch* instr) {
  int true_block = chunk_->LookupDestination(instr->true_block_id());
  int false_block = chunk_->LookupDestination(instr->false_block_id());

  Condition is_smi;
  if (instr->InputAt(0)->IsRegister()) {
    Register input = ToRegister(instr->InputAt(0));
    is_smi = masm()->CheckSmi(input);
  } else {
    Operand input = ToOperand(instr->InputAt(0));
    is_smi = masm()->CheckSmi(input);
  }
  EmitBranch(true_block, false_block, is_smi);
}


static InstanceType TestType(HHasInstanceType* instr) {
  InstanceType from = instr->from();
  InstanceType to = instr->to();
  if (from == FIRST_TYPE) return to;
  ASSERT(from == to || to == LAST_TYPE);
  return from;
}


static Condition BranchCondition(HHasInstanceType* instr) {
  InstanceType from = instr->from();
  InstanceType to = instr->to();
  if (from == to) return equal;
  if (to == LAST_TYPE) return above_equal;
  if (from == FIRST_TYPE) return below_equal;
  UNREACHABLE();
  return equal;
}


void LCodeGen::DoHasInstanceType(LHasInstanceType* instr) {
  Abort("Unimplemented: %s", "DoHasInstanceType");
}


void LCodeGen::DoHasInstanceTypeAndBranch(LHasInstanceTypeAndBranch* instr) {
  Register input = ToRegister(instr->InputAt(0));

  int true_block = chunk_->LookupDestination(instr->true_block_id());
  int false_block = chunk_->LookupDestination(instr->false_block_id());

  Label* false_label = chunk_->GetAssemblyLabel(false_block);

  __ JumpIfSmi(input, false_label);

  __ CmpObjectType(input, TestType(instr->hydrogen()), kScratchRegister);
  EmitBranch(true_block, false_block, BranchCondition(instr->hydrogen()));
}


void LCodeGen::DoHasCachedArrayIndex(LHasCachedArrayIndex* instr) {
  Abort("Unimplemented: %s", "DoHasCachedArrayIndex");
}


void LCodeGen::DoHasCachedArrayIndexAndBranch(
    LHasCachedArrayIndexAndBranch* instr) {
    Register input = ToRegister(instr->InputAt(0));

  int true_block = chunk_->LookupDestination(instr->true_block_id());
  int false_block = chunk_->LookupDestination(instr->false_block_id());

  __ testl(FieldOperand(input, String::kHashFieldOffset),
           Immediate(String::kContainsCachedArrayIndexMask));
  EmitBranch(true_block, false_block, not_equal);
}


// Branches to a label or falls through with the answer in the z flag.
// Trashes the temp register and possibly input (if it and temp are aliased).
void LCodeGen::EmitClassOfTest(Label* is_true,
                               Label* is_false,
                               Handle<String> class_name,
                               Register input,
                               Register temp) {
  __ JumpIfSmi(input, is_false);
  __ CmpObjectType(input, FIRST_JS_OBJECT_TYPE, temp);
  __ j(below, is_false);

  // Map is now in temp.
  // Functions have class 'Function'.
  __ CmpInstanceType(temp, JS_FUNCTION_TYPE);
  if (class_name->IsEqualTo(CStrVector("Function"))) {
    __ j(equal, is_true);
  } else {
    __ j(equal, is_false);
  }

  // Check if the constructor in the map is a function.
  __ movq(temp, FieldOperand(temp, Map::kConstructorOffset));

  // As long as JS_FUNCTION_TYPE is the last instance type and it is
  // right after LAST_JS_OBJECT_TYPE, we can avoid checking for
  // LAST_JS_OBJECT_TYPE.
  ASSERT(LAST_TYPE == JS_FUNCTION_TYPE);
  ASSERT(JS_FUNCTION_TYPE == LAST_JS_OBJECT_TYPE + 1);

  // Objects with a non-function constructor have class 'Object'.
  __ CmpObjectType(temp, JS_FUNCTION_TYPE, kScratchRegister);
  if (class_name->IsEqualTo(CStrVector("Object"))) {
    __ j(not_equal, is_true);
  } else {
    __ j(not_equal, is_false);
  }

  // temp now contains the constructor function. Grab the
  // instance class name from there.
  __ movq(temp, FieldOperand(temp, JSFunction::kSharedFunctionInfoOffset));
  __ movq(temp, FieldOperand(temp,
                             SharedFunctionInfo::kInstanceClassNameOffset));
  // The class name we are testing against is a symbol because it's a literal.
  // The name in the constructor is a symbol because of the way the context is
  // booted.  This routine isn't expected to work for random API-created
  // classes and it doesn't have to because you can't access it with natives
  // syntax.  Since both sides are symbols it is sufficient to use an identity
  // comparison.
  ASSERT(class_name->IsSymbol());
  __ Cmp(temp, class_name);
  // End with the answer in the z flag.
}


void LCodeGen::DoClassOfTest(LClassOfTest* instr) {
  Register input = ToRegister(instr->InputAt(0));
  Register result = ToRegister(instr->result());
  ASSERT(input.is(result));
  Register temp = ToRegister(instr->TempAt(0));
  Handle<String> class_name = instr->hydrogen()->class_name();
  NearLabel done;
  Label is_true, is_false;

  EmitClassOfTest(&is_true, &is_false, class_name, input, temp);

  __ j(not_equal, &is_false);

  __ bind(&is_true);
  __ LoadRoot(result, Heap::kTrueValueRootIndex);
  __ jmp(&done);

  __ bind(&is_false);
  __ LoadRoot(result, Heap::kFalseValueRootIndex);
  __ bind(&done);
}


void LCodeGen::DoClassOfTestAndBranch(LClassOfTestAndBranch* instr) {
  Register input = ToRegister(instr->InputAt(0));
  Register temp = ToRegister(instr->TempAt(0));
  Handle<String> class_name = instr->hydrogen()->class_name();

  int true_block = chunk_->LookupDestination(instr->true_block_id());
  int false_block = chunk_->LookupDestination(instr->false_block_id());

  Label* true_label = chunk_->GetAssemblyLabel(true_block);
  Label* false_label = chunk_->GetAssemblyLabel(false_block);

  EmitClassOfTest(true_label, false_label, class_name, input, temp);

  EmitBranch(true_block, false_block, equal);
}


void LCodeGen::DoCmpMapAndBranch(LCmpMapAndBranch* instr) {
  Register reg = ToRegister(instr->InputAt(0));
  int true_block = instr->true_block_id();
  int false_block = instr->false_block_id();

  __ Cmp(FieldOperand(reg, HeapObject::kMapOffset), instr->map());
  EmitBranch(true_block, false_block, equal);
}


void LCodeGen::DoInstanceOf(LInstanceOf* instr) {
  Abort("Unimplemented: %s", "DoInstanceOf");
}


void LCodeGen::DoInstanceOfAndBranch(LInstanceOfAndBranch* instr) {
  int true_block = chunk_->LookupDestination(instr->true_block_id());
  int false_block = chunk_->LookupDestination(instr->false_block_id());

  InstanceofStub stub(InstanceofStub::kArgsInRegisters);
  CallCode(stub.GetCode(), RelocInfo::CODE_TARGET, instr);
  __ testq(rax, rax);
  EmitBranch(true_block, false_block, zero);
}


void LCodeGen::DoInstanceOfKnownGlobal(LInstanceOfKnownGlobal* instr) {
  Abort("Unimplemented: %s", "DoInstanceOfKnowGLobal");
}


void LCodeGen::DoDeferredLInstanceOfKnownGlobal(LInstanceOfKnownGlobal* instr,
                                                Label* map_check) {
  Abort("Unimplemented: %s", "DoDeferredLInstanceOfKnownGlobakl");
}


void LCodeGen::DoCmpT(LCmpT* instr) {
  Token::Value op = instr->op();

  Handle<Code> ic = CompareIC::GetUninitialized(op);
  CallCode(ic, RelocInfo::CODE_TARGET, instr);

  Condition condition = TokenToCondition(op, false);
  if (op == Token::GT || op == Token::LTE) {
    condition = ReverseCondition(condition);
  }
  NearLabel true_value, done;
  __ testq(rax, rax);
  __ j(condition, &true_value);
  __ LoadRoot(ToRegister(instr->result()), Heap::kFalseValueRootIndex);
  __ jmp(&done);
  __ bind(&true_value);
  __ LoadRoot(ToRegister(instr->result()), Heap::kTrueValueRootIndex);
  __ bind(&done);
}


void LCodeGen::DoCmpTAndBranch(LCmpTAndBranch* instr) {
  Token::Value op = instr->op();
  int true_block = chunk_->LookupDestination(instr->true_block_id());
  int false_block = chunk_->LookupDestination(instr->false_block_id());

  Handle<Code> ic = CompareIC::GetUninitialized(op);
  CallCode(ic, RelocInfo::CODE_TARGET, instr);

  // The compare stub expects compare condition and the input operands
  // reversed for GT and LTE.
  Condition condition = TokenToCondition(op, false);
  if (op == Token::GT || op == Token::LTE) {
    condition = ReverseCondition(condition);
  }
  __ testq(rax, rax);
  EmitBranch(true_block, false_block, condition);
}


void LCodeGen::DoReturn(LReturn* instr) {
  if (FLAG_trace) {
    // Preserve the return value on the stack and rely on the runtime
    // call to return the value in the same register.
    __ push(rax);
    __ CallRuntime(Runtime::kTraceExit, 1);
  }
  __ movq(rsp, rbp);
  __ pop(rbp);
  __ ret((ParameterCount() + 1) * kPointerSize);
}


void LCodeGen::DoLoadGlobal(LLoadGlobal* instr) {
  Register result = ToRegister(instr->result());
  if (result.is(rax)) {
    __ load_rax(instr->hydrogen()->cell().location(),
                RelocInfo::GLOBAL_PROPERTY_CELL);
  } else {
    __ movq(result, instr->hydrogen()->cell(), RelocInfo::GLOBAL_PROPERTY_CELL);
    __ movq(result, Operand(result, 0));
  }
  if (instr->hydrogen()->check_hole_value()) {
    __ CompareRoot(result, Heap::kTheHoleValueRootIndex);
    DeoptimizeIf(equal, instr->environment());
  }
}


void LCodeGen::DoStoreGlobal(LStoreGlobal* instr) {
  Register value = ToRegister(instr->InputAt(0));
  if (value.is(rax)) {
    __ store_rax(instr->hydrogen()->cell().location(),
                 RelocInfo::GLOBAL_PROPERTY_CELL);
  } else {
    __ movq(kScratchRegister,
            Handle<Object>::cast(instr->hydrogen()->cell()),
            RelocInfo::GLOBAL_PROPERTY_CELL);
    __ movq(Operand(kScratchRegister, 0), value);
  }
}


void LCodeGen::DoLoadContextSlot(LLoadContextSlot* instr) {
  Abort("Unimplemented: %s", "DoLoadContextSlot");
}


void LCodeGen::DoLoadNamedField(LLoadNamedField* instr) {
  Register object = ToRegister(instr->InputAt(0));
  Register result = ToRegister(instr->result());
  if (instr->hydrogen()->is_in_object()) {
    __ movq(result, FieldOperand(object, instr->hydrogen()->offset()));
  } else {
    __ movq(result, FieldOperand(object, JSObject::kPropertiesOffset));
    __ movq(result, FieldOperand(result, instr->hydrogen()->offset()));
  }
}


void LCodeGen::DoLoadNamedGeneric(LLoadNamedGeneric* instr) {
  Abort("Unimplemented: %s", "DoLoadNamedGeneric");
}


void LCodeGen::DoLoadFunctionPrototype(LLoadFunctionPrototype* instr) {
  Abort("Unimplemented: %s", "DoLoadFunctionPrototype");
}


void LCodeGen::DoLoadElements(LLoadElements* instr) {
  Abort("Unimplemented: %s", "DoLoadElements");
}


void LCodeGen::DoAccessArgumentsAt(LAccessArgumentsAt* instr) {
  Abort("Unimplemented: %s", "DoAccessArgumentsAt");
}


void LCodeGen::DoLoadKeyedFastElement(LLoadKeyedFastElement* instr) {
  Abort("Unimplemented: %s", "DoLoadKeyedFastElement");
}


void LCodeGen::DoLoadKeyedGeneric(LLoadKeyedGeneric* instr) {
  Abort("Unimplemented: %s", "DoLoadKeyedGeneric");
}


void LCodeGen::DoArgumentsElements(LArgumentsElements* instr) {
  Abort("Unimplemented: %s", "DoArgumentsElements");
}


void LCodeGen::DoArgumentsLength(LArgumentsLength* instr) {
  Abort("Unimplemented: %s", "DoArgumentsLength");
}


void LCodeGen::DoApplyArguments(LApplyArguments* instr) {
  Abort("Unimplemented: %s", "DoApplyArguments");
}


void LCodeGen::DoPushArgument(LPushArgument* instr) {
  LOperand* argument = instr->InputAt(0);
  if (argument->IsConstantOperand()) {
    LConstantOperand* const_op = LConstantOperand::cast(argument);
    Handle<Object> literal = chunk_->LookupLiteral(const_op);
    Representation r = chunk_->LookupLiteralRepresentation(const_op);
    if (r.IsInteger32()) {
      ASSERT(literal->IsNumber());
      __ push(Immediate(static_cast<int32_t>(literal->Number())));
    } else if (r.IsDouble()) {
      Abort("unsupported double immediate");
    } else {
      ASSERT(r.IsTagged());
      __ Push(literal);
    }
  } else if (argument->IsRegister()) {
    __ push(ToRegister(argument));
  } else {
    ASSERT(!argument->IsDoubleRegister());
    __ push(ToOperand(argument));
  }
}


void LCodeGen::DoGlobalObject(LGlobalObject* instr) {
  Register result = ToRegister(instr->result());
  __ movq(result, GlobalObjectOperand());
}


void LCodeGen::DoGlobalReceiver(LGlobalReceiver* instr) {
  Register result = ToRegister(instr->result());
  __ movq(result, Operand(rsi, Context::SlotOffset(Context::GLOBAL_INDEX)));
  __ movq(result, FieldOperand(result, GlobalObject::kGlobalReceiverOffset));
}


void LCodeGen::CallKnownFunction(Handle<JSFunction> function,
                                 int arity,
                                 LInstruction* instr) {
  Abort("Unimplemented: %s", "CallKnownFunction");
}


void LCodeGen::DoCallConstantFunction(LCallConstantFunction* instr) {
  Abort("Unimplemented: %s", "DoCallConstantFunction");
}


void LCodeGen::DoDeferredMathAbsTaggedHeapNumber(LUnaryMathOperation* instr) {
  Abort("Unimplemented: %s", "DoDeferredMathAbsTaggedHeapNumber");
}


void LCodeGen::DoMathAbs(LUnaryMathOperation* instr) {
  Abort("Unimplemented: %s", "DoMathAbs");
}


void LCodeGen::DoMathFloor(LUnaryMathOperation* instr) {
  Abort("Unimplemented: %s", "DoMathFloor");
}


void LCodeGen::DoMathRound(LUnaryMathOperation* instr) {
  Abort("Unimplemented: %s", "DoMathRound");
}


void LCodeGen::DoMathSqrt(LUnaryMathOperation* instr) {
  Abort("Unimplemented: %s", "DoMathSqrt");
}


void LCodeGen::DoMathPowHalf(LUnaryMathOperation* instr) {
  Abort("Unimplemented: %s", "DoMathPowHalf");
}


void LCodeGen::DoPower(LPower* instr) {
  Abort("Unimplemented: %s", "DoPower");
}


void LCodeGen::DoMathLog(LUnaryMathOperation* instr) {
  Abort("Unimplemented: %s", "DoMathLog");
}


void LCodeGen::DoMathCos(LUnaryMathOperation* instr) {
  Abort("Unimplemented: %s", "DoMathCos");
}


void LCodeGen::DoMathSin(LUnaryMathOperation* instr) {
  Abort("Unimplemented: %s", "DoMathSin");
}


void LCodeGen::DoUnaryMathOperation(LUnaryMathOperation* instr) {
  Abort("Unimplemented: %s", "DoUnaryMathOperation");
}


void LCodeGen::DoCallKeyed(LCallKeyed* instr) {
  Abort("Unimplemented: %s", "DoCallKeyed");
}


void LCodeGen::DoCallNamed(LCallNamed* instr) {
  Abort("Unimplemented: %s", "DoCallNamed");
}


void LCodeGen::DoCallFunction(LCallFunction* instr) {
  Abort("Unimplemented: %s", "DoCallFunction");
}


void LCodeGen::DoCallGlobal(LCallGlobal* instr) {
  Abort("Unimplemented: %s", "DoCallGlobal");
}


void LCodeGen::DoCallKnownGlobal(LCallKnownGlobal* instr) {
  Abort("Unimplemented: %s", "DoCallKnownGlobal");
}


void LCodeGen::DoCallNew(LCallNew* instr) {
  ASSERT(ToRegister(instr->InputAt(0)).is(rdi));
  ASSERT(ToRegister(instr->result()).is(rax));

  Handle<Code> builtin(Builtins::builtin(Builtins::JSConstructCall));
  __ Set(rax, instr->arity());
  CallCode(builtin, RelocInfo::CONSTRUCT_CALL, instr);
}


void LCodeGen::DoCallRuntime(LCallRuntime* instr) {
  Abort("Unimplemented: %s", "DoCallRuntime");
}


void LCodeGen::DoStoreNamedField(LStoreNamedField* instr) {
  Register object = ToRegister(instr->object());
  Register value = ToRegister(instr->value());
  int offset = instr->offset();

  if (!instr->transition().is_null()) {
    __ Move(FieldOperand(object, HeapObject::kMapOffset), instr->transition());
  }

  // Do the store.
  if (instr->is_in_object()) {
    __ movq(FieldOperand(object, offset), value);
    if (instr->needs_write_barrier()) {
      Register temp = ToRegister(instr->TempAt(0));
      // Update the write barrier for the object for in-object properties.
      __ RecordWrite(object, offset, value, temp);
    }
  } else {
    Register temp = ToRegister(instr->TempAt(0));
    __ movq(temp, FieldOperand(object, JSObject::kPropertiesOffset));
    __ movq(FieldOperand(temp, offset), value);
    if (instr->needs_write_barrier()) {
      // Update the write barrier for the properties array.
      // object is used as a scratch register.
      __ RecordWrite(temp, offset, value, object);
    }
  }
}


void LCodeGen::DoStoreNamedGeneric(LStoreNamedGeneric* instr) {
  Abort("Unimplemented: %s", "DoStoreNamedGeneric");
}


void LCodeGen::DoBoundsCheck(LBoundsCheck* instr) {
  Abort("Unimplemented: %s", "DoBoundsCheck");
}


void LCodeGen::DoStoreKeyedFastElement(LStoreKeyedFastElement* instr) {
  Abort("Unimplemented: %s", "DoStoreKeyedFastElement");
}


void LCodeGen::DoStoreKeyedGeneric(LStoreKeyedGeneric* instr) {
  Abort("Unimplemented: %s", "DoStoreKeyedGeneric");
}


void LCodeGen::DoInteger32ToDouble(LInteger32ToDouble* instr) {
  LOperand* input = instr->InputAt(0);
  ASSERT(input->IsRegister() || input->IsStackSlot());
  LOperand* output = instr->result();
  ASSERT(output->IsDoubleRegister());
  __ cvtlsi2sd(ToDoubleRegister(output), ToOperand(input));
}


void LCodeGen::DoNumberTagI(LNumberTagI* instr) {
  LOperand* input = instr->InputAt(0);
  ASSERT(input->IsRegister() && input->Equals(instr->result()));
  Register reg = ToRegister(input);

  __ Integer32ToSmi(reg, reg);
}


void LCodeGen::DoNumberTagD(LNumberTagD* instr) {
  class DeferredNumberTagD: public LDeferredCode {
   public:
    DeferredNumberTagD(LCodeGen* codegen, LNumberTagD* instr)
        : LDeferredCode(codegen), instr_(instr) { }
    virtual void Generate() { codegen()->DoDeferredNumberTagD(instr_); }
   private:
    LNumberTagD* instr_;
  };

  XMMRegister input_reg = ToDoubleRegister(instr->InputAt(0));
  Register reg = ToRegister(instr->result());
  Register tmp = ToRegister(instr->TempAt(0));

  DeferredNumberTagD* deferred = new DeferredNumberTagD(this, instr);
  if (FLAG_inline_new) {
    __ AllocateHeapNumber(reg, tmp, deferred->entry());
  } else {
    __ jmp(deferred->entry());
  }
  __ bind(deferred->exit());
  __ movsd(FieldOperand(reg, HeapNumber::kValueOffset), input_reg);
}


void LCodeGen::DoDeferredNumberTagD(LNumberTagD* instr) {
  // TODO(3095996): Get rid of this. For now, we need to make the
  // result register contain a valid pointer because it is already
  // contained in the register pointer map.
  Register reg = ToRegister(instr->result());
  __ Move(reg, Smi::FromInt(0));

  __ PushSafepointRegisters();
  __ CallRuntimeSaveDoubles(Runtime::kAllocateHeapNumber);
  RecordSafepointWithRegisters(
      instr->pointer_map(), 0, Safepoint::kNoDeoptimizationIndex);
  // Ensure that value in rax survives popping registers.
  __ movq(kScratchRegister, rax);
  __ PopSafepointRegisters();
  __ movq(reg, kScratchRegister);
}


void LCodeGen::DoSmiTag(LSmiTag* instr) {
  Abort("Unimplemented: %s", "DoSmiTag");
}


void LCodeGen::DoSmiUntag(LSmiUntag* instr) {
  Abort("Unimplemented: %s", "DoSmiUntag");
}


void LCodeGen::EmitNumberUntagD(Register input_reg,
                                XMMRegister result_reg,
                                LEnvironment* env) {
  NearLabel load_smi, heap_number, done;

  // Smi check.
  __ JumpIfSmi(input_reg, &load_smi);

  // Heap number map check.
  __ CompareRoot(FieldOperand(input_reg, HeapObject::kMapOffset),
                 Heap::kHeapNumberMapRootIndex);
  __ j(equal, &heap_number);

  __ CompareRoot(input_reg, Heap::kUndefinedValueRootIndex);
  DeoptimizeIf(not_equal, env);

  // Convert undefined to NaN. Compute NaN as 0/0.
  __ xorpd(result_reg, result_reg);
  __ divsd(result_reg, result_reg);
  __ jmp(&done);

  // Heap number to XMM conversion.
  __ bind(&heap_number);
  __ movsd(result_reg, FieldOperand(input_reg, HeapNumber::kValueOffset));
  __ jmp(&done);

  // Smi to XMM conversion
  __ bind(&load_smi);
  __ SmiToInteger32(kScratchRegister, input_reg);  // Untag smi first.
  __ cvtlsi2sd(result_reg, kScratchRegister);
  __ bind(&done);
}


void LCodeGen::DoDeferredTaggedToI(LTaggedToI* instr) {
  Abort("Unimplemented: %s", "DoDeferredTaggedToI");
}


void LCodeGen::DoTaggedToI(LTaggedToI* instr) {
  Abort("Unimplemented: %s", "DoTaggedToI");
}


void LCodeGen::DoNumberUntagD(LNumberUntagD* instr) {
  Abort("Unimplemented: %s", "DoNumberUntagD");
}


void LCodeGen::DoDoubleToI(LDoubleToI* instr) {
  Abort("Unimplemented: %s", "DoDoubleToI");
}


void LCodeGen::DoCheckSmi(LCheckSmi* instr) {
  LOperand* input = instr->InputAt(0);
  ASSERT(input->IsRegister());
  Condition cc = masm()->CheckSmi(ToRegister(input));
  if (instr->condition() != equal) {
    cc = NegateCondition(cc);
  }
  DeoptimizeIf(cc, instr->environment());
}


void LCodeGen::DoCheckInstanceType(LCheckInstanceType* instr) {
  Abort("Unimplemented: %s", "DoCheckInstanceType");
}


void LCodeGen::DoCheckFunction(LCheckFunction* instr) {
  ASSERT(instr->InputAt(0)->IsRegister());
  Register reg = ToRegister(instr->InputAt(0));
  __ Cmp(reg, instr->hydrogen()->target());
  DeoptimizeIf(not_equal, instr->environment());
}


void LCodeGen::DoCheckMap(LCheckMap* instr) {
  LOperand* input = instr->InputAt(0);
  ASSERT(input->IsRegister());
  Register reg = ToRegister(input);
  __ Cmp(FieldOperand(reg, HeapObject::kMapOffset),
         instr->hydrogen()->map());
  DeoptimizeIf(not_equal, instr->environment());
}


void LCodeGen::LoadHeapObject(Register result, Handle<HeapObject> object) {
  Abort("Unimplemented: %s", "LoadHeapObject");
}


void LCodeGen::DoCheckPrototypeMaps(LCheckPrototypeMaps* instr) {
  Register reg = ToRegister(instr->TempAt(0));

  Handle<JSObject> holder = instr->holder();
  Handle<JSObject> current_prototype = instr->prototype();

  // Load prototype object.
  LoadHeapObject(reg, current_prototype);

  // Check prototype maps up to the holder.
  while (!current_prototype.is_identical_to(holder)) {
    __ Cmp(FieldOperand(reg, HeapObject::kMapOffset),
           Handle<Map>(current_prototype->map()));
    DeoptimizeIf(not_equal, instr->environment());
    current_prototype =
        Handle<JSObject>(JSObject::cast(current_prototype->GetPrototype()));
    // Load next prototype object.
    LoadHeapObject(reg, current_prototype);
  }

  // Check the holder map.
  __ Cmp(FieldOperand(reg, HeapObject::kMapOffset),
         Handle<Map>(current_prototype->map()));
  DeoptimizeIf(not_equal, instr->environment());
}


void LCodeGen::DoArrayLiteral(LArrayLiteral* instr) {
  Abort("Unimplemented: %s", "DoArrayLiteral");
}


void LCodeGen::DoObjectLiteral(LObjectLiteral* instr) {
  Abort("Unimplemented: %s", "DoObjectLiteral");
}


void LCodeGen::DoRegExpLiteral(LRegExpLiteral* instr) {
  Abort("Unimplemented: %s", "DoRegExpLiteral");
}


void LCodeGen::DoFunctionLiteral(LFunctionLiteral* instr) {
  Abort("Unimplemented: %s", "DoFunctionLiteral");
}


void LCodeGen::DoTypeof(LTypeof* instr) {
  Abort("Unimplemented: %s", "DoTypeof");
}


void LCodeGen::DoTypeofIs(LTypeofIs* instr) {
  Abort("Unimplemented: %s", "DoTypeofIs");
}


void LCodeGen::DoTypeofIsAndBranch(LTypeofIsAndBranch* instr) {
  Register input = ToRegister(instr->InputAt(0));
  int true_block = chunk_->LookupDestination(instr->true_block_id());
  int false_block = chunk_->LookupDestination(instr->false_block_id());
  Label* true_label = chunk_->GetAssemblyLabel(true_block);
  Label* false_label = chunk_->GetAssemblyLabel(false_block);

  Condition final_branch_condition = EmitTypeofIs(true_label,
                                                  false_label,
                                                  input,
                                                  instr->type_literal());

  EmitBranch(true_block, false_block, final_branch_condition);
}


Condition LCodeGen::EmitTypeofIs(Label* true_label,
                                 Label* false_label,
                                 Register input,
                                 Handle<String> type_name) {
  Condition final_branch_condition = no_condition;
  if (type_name->Equals(Heap::number_symbol())) {
    __ JumpIfSmi(input, true_label);
    __ Cmp(FieldOperand(input, HeapObject::kMapOffset),
           Factory::heap_number_map());
    final_branch_condition = equal;

  } else if (type_name->Equals(Heap::string_symbol())) {
    __ JumpIfSmi(input, false_label);
    __ movq(input, FieldOperand(input, HeapObject::kMapOffset));
    __ testb(FieldOperand(input, Map::kBitFieldOffset),
             Immediate(1 << Map::kIsUndetectable));
    __ j(not_zero, false_label);
    __ CmpInstanceType(input, FIRST_NONSTRING_TYPE);
    final_branch_condition = below;

  } else if (type_name->Equals(Heap::boolean_symbol())) {
    __ CompareRoot(input, Heap::kTrueValueRootIndex);
    __ j(equal, true_label);
    __ CompareRoot(input, Heap::kFalseValueRootIndex);
    final_branch_condition = equal;

  } else if (type_name->Equals(Heap::undefined_symbol())) {
    __ CompareRoot(input, Heap::kUndefinedValueRootIndex);
    __ j(equal, true_label);
    __ JumpIfSmi(input, false_label);
    // Check for undetectable objects => true.
    __ movq(input, FieldOperand(input, HeapObject::kMapOffset));
    __ testb(FieldOperand(input, Map::kBitFieldOffset),
             Immediate(1 << Map::kIsUndetectable));
    final_branch_condition = not_zero;

  } else if (type_name->Equals(Heap::function_symbol())) {
    __ JumpIfSmi(input, false_label);
    __ CmpObjectType(input, FIRST_FUNCTION_CLASS_TYPE, input);
    final_branch_condition = above_equal;

  } else if (type_name->Equals(Heap::object_symbol())) {
    __ JumpIfSmi(input, false_label);
    __ Cmp(input, Factory::null_value());
    __ j(equal, true_label);
    // Check for undetectable objects => false.
    __ testb(FieldOperand(input, Map::kBitFieldOffset),
             Immediate(1 << Map::kIsUndetectable));
    __ j(not_zero, false_label);
    // Check for JS objects that are not RegExp or Function => true.
    __ CmpInstanceType(input, FIRST_JS_OBJECT_TYPE);
    __ j(below, false_label);
    __ CmpInstanceType(input, FIRST_FUNCTION_CLASS_TYPE);
    final_branch_condition = below_equal;

  } else {
    final_branch_condition = never;
    __ jmp(false_label);
  }

  return final_branch_condition;
}


void LCodeGen::DoLazyBailout(LLazyBailout* instr) {
  // No code for lazy bailout instruction. Used to capture environment after a
  // call for populating the safepoint data with deoptimization data.
}


void LCodeGen::DoDeoptimize(LDeoptimize* instr) {
  DeoptimizeIf(no_condition, instr->environment());
}


void LCodeGen::DoDeleteProperty(LDeleteProperty* instr) {
  Abort("Unimplemented: %s", "DoDeleteProperty");
}


void LCodeGen::DoStackCheck(LStackCheck* instr) {
  // Perform stack overflow check.
  NearLabel done;
  ExternalReference stack_limit = ExternalReference::address_of_stack_limit();
  __ CompareRoot(rsp, Heap::kStackLimitRootIndex);
  __ j(above_equal, &done);

  StackCheckStub stub;
  CallCode(stub.GetCode(), RelocInfo::CODE_TARGET, instr);
  __ bind(&done);
}


void LCodeGen::DoOsrEntry(LOsrEntry* instr) {
  Abort("Unimplemented: %s", "DoOsrEntry");
}

#undef __

} }  // namespace v8::internal

#endif  // V8_TARGET_ARCH_X64
