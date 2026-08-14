#pragma warning(push)
#pragma warning(disable: 4244 4267)
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/Dominators.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/Transforms/Utils/Mem2Reg.h>
#include <llvm/Transforms/Scalar/SROA.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>
#include <llvm/Transforms/IPO/GlobalDCE.h>
#include <llvm/Transforms/Instrumentation/AddressSanitizer.h>
#include <llvm/Object/COFF.h>
#include <llvm/Object/Binary.h>
#include <llvm/Object/Archive.h>
#include <llvm/Object/COFFImportFile.h>
#include <llvm/ADT/StringSet.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/TimeProfiler.h>
#include <llvm/Support/JSON.h>
#include <llvm/IR/DiagnosticInfo.h>
#include <llvm/IR/DiagnosticHandler.h>
#pragma warning(pop)
#include <antlr4-runtime.h>

#include "platform/GeneratedParser.h"
#include "LLVMBackend.h"
#include "MainListener.h"
#include "GrammarTreeListener.h"
#include <filesystem>
#include <optional>
#include <algorithm>
#include <cctype>
#include <map>
#include <set>

#if defined(__APPLE__)
// Step 3 (macOS self-contained link): harvest libSystem's exported symbols from
// the live dyld shared cache to synthesize a linker stub, so -o needs no SDK.
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <dlfcn.h>
#include <cstring>
#include <sys/sysctl.h>
#endif

// ---- Definitions moved out of LLVMBackend.h (OwnershipTemps) ----

void LLVMBackend::SetTargetLongWidth(bool targetWindows, int platformBits)
{
        longBits_ = (targetWindows || platformBits == 32) ? 32 : 64;
    }

void LLVMBackend::AddVectorizeLoopInfo(const VectorizeLoopInfo& info)
{ vectorizeLoops_.push_back(info); }

int LLVMBackend::ArrayViewBufferFieldIndex(const std::string& typeName)
{
        if (typeName.empty()) return -1;
        const auto& ds = GetDataStructure(typeName);
        for (int i = 0; i < (int)ds.StructFields.size(); ++i)
            if (ds.StructFields[i].VariableName == "_ptr" && ds.StructFields[i].IsArrayView)
                return i;
        return -1;
    }

bool LLVMBackend::ArrayViewElementOwnsNothing(const TypeAndValue& elemField)
{
        if (elemField.IsInterface) return false;
        if (elemField.Pointer && elemField.IsUnique) return false;
        return !IsOwningValueOrClosureType(elemField.TypeName);
    }

void LLVMBackend::NoteVectorizeSpanAccessor(int loopLine, const std::string& accessor,
                                   const std::string& receiver, int line, int col)
{
        for (auto& vi : vectorizeLoops_)
            if (vi.line == loopLine && !vi.hasSpanAccessor)
            {
                vi.hasSpanAccessor = true;
                vi.spanAccessor = accessor;
                vi.spanReceiver = receiver;
                vi.spanLine = line;
                vi.spanCol = col;
                return;
            }
    }

void LLVMBackend::RegisterInterfaceBox(const InterfaceBoxRecord& record)
{
        if (record.FatValue == nullptr) return;
        for (auto& entry : interfaceBoxRecords_)
            if (entry.FatValue == record.FatValue
                && entry.DataPointer == record.DataPointer
                && entry.Source == record.Source)
            {
                entry = record;
                return;
        }
        interfaceBoxRecords_.push_back(record);
    }

const LLVMBackend::InterfaceBoxRecord* LLVMBackend::FindInterfaceBoxByFatValue(const llvm::Value* value) const
{
        for (const auto& entry : interfaceBoxRecords_)
            if (entry.FatValue == value) return &entry;
        return nullptr;
    }

const LLVMBackend::InterfaceBoxRecord* LLVMBackend::FindInterfaceBoxByDataPointer(const llvm::Value* value,
                                                            InterfaceBoxSource source) const
{
        for (const auto& entry : interfaceBoxRecords_)
            if (entry.DataPointer == value && entry.Source == source) return &entry;
        return nullptr;
    }

void LLVMBackend::RegisterNullCoalesceJoin(llvm::Value* joined, std::vector<NullCoalesceJoinArm> arms)
{
        if (joined == nullptr || arms.empty()) return;
        for (auto& entry : nullCoalesceJoins_)
            if (entry.Joined == joined) { entry.Arms = std::move(arms); return; }
        nullCoalesceJoins_.push_back({ joined, std::move(arms) });
    }

const LLVMBackend::NullCoalesceJoin* LLVMBackend::FindNullCoalesceJoin(const llvm::Value* value) const
{
        if (value == nullptr) return nullptr;
        for (const auto& entry : nullCoalesceJoins_)
            if (entry.Joined == value) return &entry;
        return nullptr;
    }

void LLVMBackend::RegisterUniqueFieldRead(llvm::Value* value, llvm::Value* storage)
{
        if (value == nullptr || storage == nullptr) return;
        for (auto& entry : uniqueFieldReadValues_)
            if (entry.first == value)
            {
                entry.second.Storage = storage;
                entry.second.Block = builder != nullptr && builder->GetInsertBlock() != nullptr
                    ? builder->GetInsertBlock() : nullptr;
                return;
            }
        uniqueFieldReadValues_.push_back({
            value,
            { storage, builder != nullptr && builder->GetInsertBlock() != nullptr
                ? builder->GetInsertBlock() : nullptr }
        });
    }

void LLVMBackend::PropagateUniqueFieldRead(llvm::Value* trueValue, llvm::Value* falseValue,
                                           llvm::Value* joined)
{
        if (joined == nullptr) return;

        auto direct = [&](const llvm::Value* value, llvm::BasicBlock* block,
                          std::vector<UniqueFieldReadSource>& out) -> bool {
            for (const auto& entry : uniqueFieldReadValues_)
                if (entry.first == value)
                {
                    out.push_back({ entry.second.Storage, block != nullptr ? block : entry.second.Block });
                    return true;
                }
            return false;
        };
        auto joinedSources = [&](const llvm::Value* value,
                                 std::vector<UniqueFieldReadSource>& out) -> bool {
            for (const auto& entry : uniqueFieldReadJoins_)
                if (entry.Joined == value)
                {
                    out.insert(out.end(), entry.Sources.begin(), entry.Sources.end());
                    return true;
                }
            return false;
        };
        auto isNull = [](const llvm::Value* value) {
            auto* constant = llvm::dyn_cast_or_null<llvm::Constant>(value);
            return constant != nullptr && constant->isNullValue();
        };
        auto collect = [&](const llvm::Value* value, llvm::BasicBlock* block,
                           std::vector<UniqueFieldReadSource>& out) {
            if (isNull(value)) return true;
            if (direct(value, block, out)) return true;
            return joinedSources(value, out);
        };

        std::vector<UniqueFieldReadSource> sources;
        bool recognized = false;
        if (auto* phi = llvm::dyn_cast<llvm::PHINode>(joined))
        {
            recognized = true;
            for (unsigned i = 0; i < phi->getNumIncomingValues(); i++)
                if (!collect(phi->getIncomingValue(i), phi->getIncomingBlock(i), sources))
                {
                    recognized = false;
                    break;
                }
        }
        else if (const auto* nullJoin = FindNullCoalesceJoin(joined))
        {
            recognized = true;
            for (const auto& arm : nullJoin->Arms)
                if (!collect(arm.Value, arm.Block, sources))
                {
                    recognized = false;
                    break;
                }
        }
        else
        {
            recognized = collect(trueValue, nullptr, sources)
                && collect(falseValue, nullptr, sources);
        }
        if (!recognized || sources.empty()) return;

        for (auto& entry : uniqueFieldReadJoins_)
            if (entry.Joined == joined)
            {
                entry.Sources = std::move(sources);
                return;
            }
        uniqueFieldReadJoins_.push_back({ joined, std::move(sources) });
    }

bool LLVMBackend::IsUniqueFieldReadValue(const llvm::Value* value) const
{
        if (value == nullptr) return false;
        for (const auto& entry : uniqueFieldReadValues_)
            if (entry.first == value) return true;
        for (const auto& entry : uniqueFieldReadJoins_)
            if (entry.Joined == value) return true;
        return false;
    }

const LLVMBackend::UniqueFieldReadJoin* LLVMBackend::FindUniqueFieldReadJoin(
    const llvm::Value* value) const
{
        if (value == nullptr) return nullptr;
        for (const auto& entry : uniqueFieldReadJoins_)
            if (entry.Joined == value) return &entry;
        return nullptr;
    }

void LLVMBackend::RegisterJoinArmCastOccurrence(const llvm::Value* joined, unsigned index, size_t occurrence)
{
        if (joined == nullptr) return;
        for (auto& entry : joinArmOccurrences_)
            if (entry.Joined == joined && entry.Index == index) { entry.Occurrence = occurrence; return; }
        joinArmOccurrences_.push_back({ joined, index, occurrence });
    }

size_t LLVMBackend::JoinArmCastOccurrence(const llvm::Value* joined, unsigned index, size_t fallback) const
{
        if (joined == nullptr) return fallback;
        for (const auto& entry : joinArmOccurrences_)
            if (entry.Joined == joined && entry.Index == index) return entry.Occurrence;
        return fallback;
    }

void LLVMBackend::PromoteCastOccurrence(llvm::Value* value, size_t from)
{
        if (value == nullptr || from == currentCastOccurrence_) return;
        if (IsCodeValueDataCast(value, from)) RegisterCodeValueDataCast(value);
        if (IsDataValueCodeCast(value, from)) RegisterDataValueCodeCast(value);
    }

void LLVMBackend::RegisterOwningTempUniqueField(llvm::Value* value)
{
        if (value == nullptr) return;
        for (auto* entry : owningTempUniqueFields_) if (entry == value) return;
        owningTempUniqueFields_.push_back(value);
    }

bool LLVMBackend::IsLedgeredOwningTempUniqueField(const llvm::Value* value) const
{
        for (auto* entry : owningTempUniqueFields_) if (entry == value) return true;
        return false;
    }

bool LLVMBackend::JoinCarriesOwningTempUniqueField(const llvm::Value* value, int depth) const
{
        if (value == nullptr || depth > kMaxJoinArmDepth) return false;
        if (IsLedgeredOwningTempUniqueField(value)) return true;
        if (const auto* phi = llvm::dyn_cast<llvm::PHINode>(value))
        {
            for (unsigned i = 0; i < phi->getNumIncomingValues(); i++)
                if (JoinCarriesOwningTempUniqueField(phi->getIncomingValue(i), depth + 1)) return true;
            return false;
        }
        if (const NullCoalesceJoin* join = FindNullCoalesceJoin(value))
        {
            for (const auto& arm : join->Arms)
                if (JoinCarriesOwningTempUniqueField(arm.Value, depth + 1)) return true;
            return false;
        }
        return false;
    }

void LLVMBackend::RegisterLaunderedTempUniqueField(llvm::Value* result, const std::string& calleeName,
                                                   const std::string& access)
{
        if (result == nullptr) return;
        for (auto& entry : launderedTempUniqueFields_)
            if (entry.Result == result) return;   // first launder wins: it names the inner field
        launderedTempUniqueFields_.push_back({ result, calleeName, access });
    }

const LLVMBackend::LaunderedTempUniqueField* LLVMBackend::FindLaunderedTempUniqueField(
        const llvm::Value* value, int depth) const
{
        if (value == nullptr || depth > kMaxJoinArmDepth) return nullptr;
        for (const auto& entry : launderedTempUniqueFields_)
            if (entry.Result == value) return &entry;
        if (const auto* phi = llvm::dyn_cast<llvm::PHINode>(value))
        {
            for (unsigned i = 0; i < phi->getNumIncomingValues(); i++)
                if (const auto* hit = FindLaunderedTempUniqueField(phi->getIncomingValue(i), depth + 1))
                    return hit;
            return nullptr;
        }
        if (const NullCoalesceJoin* join = FindNullCoalesceJoin(value))
            for (const auto& arm : join->Arms)
                if (const auto* hit = FindLaunderedTempUniqueField(arm.Value, depth + 1)) return hit;
        return nullptr;
    }

void LLVMBackend::RegisterPendingLaunderTempUniqueField(llvm::Value* result,
        const std::vector<std::pair<const llvm::Function*, unsigned>>& conds,
        const std::string& calleeName, const std::string& access)
{
        if (result == nullptr || conds.empty()) return;
        for (auto& entry : pendingLaunderTempUniqueFields_)
            if (entry.Result == result) return;   // first launder wins, as in the eager ledger
        pendingLaunderTempUniqueFields_.push_back({ result, conds, calleeName, access });
    }

const LLVMBackend::PendingLaunderTempUniqueField*
LLVMBackend::FindPendingLaunderTempUniqueField(const llvm::Value* value, int depth) const
{
        if (value == nullptr || depth > kMaxJoinArmDepth) return nullptr;
        for (const auto& entry : pendingLaunderTempUniqueFields_)
            if (entry.Result == value) return &entry;
        if (const auto* phi = llvm::dyn_cast<llvm::PHINode>(value))
        {
            for (unsigned i = 0; i < phi->getNumIncomingValues(); i++)
                if (const auto* hit = FindPendingLaunderTempUniqueField(phi->getIncomingValue(i),
                                                                       depth + 1))
                    return hit;
            return nullptr;
        }
        if (const NullCoalesceJoin* join = FindNullCoalesceJoin(value))
            for (const auto& arm : join->Arms)
                if (const auto* hit = FindPendingLaunderTempUniqueField(arm.Value, depth + 1))
                    return hit;
        return nullptr;
    }

bool LLVMBackend::RecordDeferredTempUniqueFieldEscape(const llvm::Value* value,
        const std::string& destDesc, const std::string& file, size_t line, size_t column)
{
        const PendingLaunderTempUniqueField* pending = FindPendingLaunderTempUniqueField(value);
        if (pending == nullptr) return false;
        deferredTempUniqueFieldEscapes_.push_back({ pending->Conds, pending->CalleeName,
                                                    pending->Access, destDesc, "", "",
                                                    file, line, column });
        return true;
    }

void LLVMBackend::RecordDeferredTempUniqueFieldSinkEscape(const llvm::Value* value,
        const std::string& functionName, const std::string& paramName)
{
        const PendingLaunderTempUniqueField* pending = FindPendingLaunderTempUniqueField(value);
        if (pending == nullptr) return;
        deferredTempUniqueFieldEscapes_.push_back({ pending->Conds, pending->CalleeName,
                                                    pending->Access, "", functionName, paramName,
                                                    sourceFileName, (size_t)currentLine,
                                                    (size_t)currentColumn });
    }

std::string LLVMBackend::DescribeLaunderedTempUniqueFieldEscape(const std::string& calleeName,
        const std::string& access, const std::string& destDesc)
{
        std::string field = access.empty() ? std::string("a unique field")
                                           : std::format("unique field '{}'", access);
        return std::format(
            "cannot store the result of '{}' into {} - '{}' may return its argument, which "
            "here is {} of a temporary, and the temporary's synthesized destructor frees "
            "the pointee at the end of this statement. Bind the whole call result to a "
            "local first and pass the field read from that local.",
            calleeName, destDesc, calleeName, field);
    }

std::string LLVMBackend::DescribeTempUniqueFieldSinkEscape(const std::string& functionName,
        const std::string& paramName)
{
        return std::format(
            "call to '{}': cannot pass a unique field of a temporary to parameter '{}', which "
            "takes ownership - the temporary's synthesized destructor frees the pointee at the "
            "end of this statement, so the callee would own freed memory. Bind the whole call "
            "result to a local first and 'move' the field out of that local.",
            functionName, paramName);
    }

bool LLVMBackend::LaunderCondsAllProve(
        const std::vector<std::pair<const llvm::Function*, unsigned>>& conds)
{
        for (const auto& cond : conds)
        {
            if (cond.first == nullptr || !FunctionBodyIsComplete(cond.first)) return false;
            if (!ParameterMayReachReturn(cond.first, cond.second)) return false;
        }
        return !conds.empty();
    }

/*
 * The RETURN half of the record-then-resolve pair. Runs after the STORE half, which is the order
 * the eager path has within a statement: the argument gate fires at the call, the destination
 * gate at the store that follows it.
 */
void LLVMBackend::ResolveDeferredTempUniqueFieldEscapes()
{
        std::vector<DeferredTempUniqueFieldEscape> pending;
        pending.swap(deferredTempUniqueFieldEscapes_);
        // ONE diagnostic per compile: LogError throws out of this loop, exactly as in the
        // store half's resolve.
        for (const auto& entry : pending)
        {
            if (!LaunderCondsAllProve(entry.Conds)) continue;
            ReportingFileScope fileScope(this, entry.File, entry.Line, entry.Column);
            // LogError throws, so exactly one of these ever runs.
            if (!entry.SinkFunction.empty())
                LogError(DescribeTempUniqueFieldSinkEscape(entry.SinkFunction, entry.SinkParam));
            else
                LogError(DescribeLaunderedTempUniqueFieldEscape(entry.CalleeName, entry.Access,
                                                                entry.DestDesc));
        }
    }

void LLVMBackend::RegisterDataValue(llvm::Value* value)
{
        if (value == nullptr) return;
        for (auto* entry : dataValues_) if (entry == value) return;
        dataValues_.push_back(value);
    }

bool LLVMBackend::IsLedgeredDataValue(const llvm::Value* value) const
{
        for (auto* entry : dataValues_) if (entry == value) return true;
        return false;
    }

void LLVMBackend::RegisterCodeValue(llvm::Value* value)
{
        if (value == nullptr) return;
        for (auto* entry : codeValues_) if (entry == value) return;
        codeValues_.push_back(value);
    }

void LLVMBackend::RegisterCodeValueDataCast(llvm::Value* value)
{
        if (value == nullptr) return;
        for (auto& entry : codeValueDataCasts_)
            if (entry.first == value && entry.second == currentCastOccurrence_) return;
        codeValueDataCasts_.push_back({ value, currentCastOccurrence_ });
    }

bool LLVMBackend::IsLedgeredCodeValue(const llvm::Value* value) const
{
        for (auto* entry : codeValues_) if (entry == value) return true;
        return false;
    }

bool LLVMBackend::IsCodeValueDataCast(const llvm::Value* value, size_t occurrence) const
{
        for (auto& entry : codeValueDataCasts_)
            if (entry.first == value && entry.second == occurrence) return true;
        return false;
    }

void LLVMBackend::RegisterDataValueCodeCast(llvm::Value* value)
{
        if (value == nullptr) return;
        if (llvm::isa<llvm::ConstantPointerNull>(value)) return;
        if (llvm::isa<llvm::Function>(value)) return;
        for (auto& entry : dataValueCodeCasts_)
            if (entry.first == value && entry.second == currentCastOccurrence_) return;
        dataValueCodeCasts_.push_back({ value, currentCastOccurrence_ });
    }

bool LLVMBackend::IsDataValueCodeCast(const llvm::Value* value, size_t occurrence) const
{
        for (auto& entry : dataValueCodeCasts_)
            if (entry.first == value && entry.second == occurrence) return true;
        return false;
    }

bool LLVMBackend::JoinArmCarriesCodeValue(const llvm::Value* value, size_t occurrence, int depth) const
{
        if (value == nullptr || depth > kMaxJoinArmDepth) return false;
        if (IsCodeValueDataCast(value, occurrence)) return false;
        if (llvm::isa<llvm::Function>(value)) return true;
        if (IsLedgeredCodeValue(value)) return true;
        return JoinCarriesCodeValue(value, occurrence, depth);
    }

bool LLVMBackend::JoinCarriesCodeValue(const llvm::Value* value, size_t occurrence, int depth) const
{
        if (value == nullptr || depth > kMaxJoinArmDepth) return false;
        if (IsCodeValueDataCast(value, occurrence)) return false;
        // Each arm answers under the occurrence IT evaluated under, so a cast written on one arm
        // launders only that arm (see joinArmOccurrences_).
        if (const auto* phi = llvm::dyn_cast<llvm::PHINode>(value))
        {
            for (unsigned i = 0; i < phi->getNumIncomingValues(); i++)
                if (JoinArmCarriesCodeValue(phi->getIncomingValue(i),
                        JoinArmCastOccurrence(phi, i, occurrence), depth + 1)) return true;
            return false;
        }
        if (const NullCoalesceJoin* join = FindNullCoalesceJoin(value))
            for (unsigned i = 0; i < join->Arms.size(); i++)
                if (JoinArmCarriesCodeValue(join->Arms[i].Value,
                        JoinArmCastOccurrence(value, i, occurrence), depth + 1)) return true;
        return false;
    }

int LLVMBackend::JoinArmDataKind(const llvm::Value* value, size_t occurrence, int depth) const
{
        if (value == nullptr) return -1;
        // The user cast THIS arm to a code type: their assertion covers this arm and no other, so
        // it is neutral here rather than blocking a sibling arm's own proof.
        if (IsDataValueCodeCast(value, occurrence)) return 0;
        if (llvm::isa<llvm::ConstantPointerNull>(value)) return 0;
        if (llvm::isa<llvm::Function>(value)) return -1;
        if (IsLedgeredDataValue(value)) return 1;
        return JoinDeliversDataValue(value, occurrence, depth) ? 1 : -1;
    }

bool LLVMBackend::JoinDeliversDataValue(const llvm::Value* value, size_t occurrence, int depth) const
{
        if (value == nullptr || depth > kMaxJoinArmDepth) return false;
        bool proven = false;
        if (const auto* phi = llvm::dyn_cast<llvm::PHINode>(value))
        {
            if (phi->getNumIncomingValues() == 0) return false;
            for (unsigned i = 0; i < phi->getNumIncomingValues(); i++)
            {
                const int kind = JoinArmDataKind(phi->getIncomingValue(i),
                    JoinArmCastOccurrence(phi, i, occurrence), depth + 1);
                if (kind < 0) return false;
                if (kind > 0) proven = true;
            }
            return proven;
        }
        if (const NullCoalesceJoin* join = FindNullCoalesceJoin(value))
        {
            if (join->Arms.empty()) return false;
            for (unsigned i = 0; i < join->Arms.size(); i++)
            {
                const int kind = JoinArmDataKind(join->Arms[i].Value,
                    JoinArmCastOccurrence(value, i, occurrence), depth + 1);
                if (kind < 0) return false;
                if (kind > 0) proven = true;
            }
            return proven;
        }
        return false;
    }

void LLVMBackend::RecordPendingReturnDangleCheck(llvm::AllocaInst* slot, int line, int col,
                                        const std::string& ifaceName,
                                        bool frameStorageProvenance,
                                        bool provenanceUnknown,
                                        const std::string& frameStorageClassName)
{
        if (!slot || !builder) return;
        llvm::BasicBlock* bb = builder->GetInsertBlock();
        if (!bb || !bb->getParent()) return;
        pendingReturnDangleChecks_[bb->getParent()].push_back(
            { bb->getParent(), slot, line, col, ifaceName, frameStorageProvenance,
              provenanceUnknown, frameStorageClassName });
}

void LLVMBackend::SetInterfaceBoxReturnDangleProvenance(const std::string& name,
                                                         bool frameStorage, bool unknown,
                                                         const std::string& className)
{
        if (name.empty()) return;
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            auto it = frame.namedVariable.find(name);
            if (it == frame.namedVariable.end()) continue;
            it->second.InterfaceBoxFrameStorage = frameStorage && !unknown;
            it->second.InterfaceBoxReturnProvenanceUnknown = unknown;
            it->second.InterfaceBoxFrameStorageClassName =
                it->second.InterfaceBoxFrameStorage ? className : std::string();
            return;
        }
}

void LLVMBackend::DiscardPendingReturnDangleChecks(llvm::Function* F)
{
        if (F) pendingReturnDangleChecks_.erase(F);
    }

llvm::Value* LLVMBackend::NullIfaceHandleValue(const llvm::WeakVH& h)
{
        return static_cast<llvm::Value*>(h);
    }

llvm::AllocaInst* LLVMBackend::ResolveIfaceStorageLoc(llvm::Value* slot,
                                                    llvm::SmallVectorImpl<uint64_t>& path)
{
        path.clear();
        llvm::SmallVector<llvm::GetElementPtrInst*, 4> chain;
        llvm::Value* cur = slot;
        while (auto* gep = llvm::dyn_cast_or_null<llvm::GetElementPtrInst>(cur))
        {
            if (chain.size() >= 8) return nullptr;
            chain.push_back(gep);
            cur = gep->getPointerOperand();
        }
        auto* base = llvm::dyn_cast_or_null<llvm::AllocaInst>(cur);
        if (base == nullptr) return nullptr;

        llvm::Type* ty = base->getAllocatedType();
        for (auto it = chain.rbegin(); it != chain.rend(); ++it)
        {
            llvm::GetElementPtrInst* gep = *it;
            if (!gep->hasAllConstantIndices()) return nullptr;
            if (gep->getSourceElementType() != ty) return nullptr;
            auto idx = gep->idx_begin();
            if (idx == gep->idx_end()) return nullptr;
            if (!llvm::cast<llvm::ConstantInt>(idx->get())->isZero()) return nullptr;
            for (++idx; idx != gep->idx_end(); ++idx)
            {
                const auto* ci = llvm::cast<llvm::ConstantInt>(idx->get());
                if (ci->getBitWidth() > 64 || ci->isNegative()) return nullptr;
                if (ci->getZExtValue() > 0xFFFFFFFFull) return nullptr;
                if (path.size() >= 8) return nullptr;
                path.push_back(ci->getZExtValue());
            }
            ty = gep->getResultElementType();
        }
        return base;
    }

llvm::GlobalVariable* LLVMBackend::ResolveIfaceStorageGlobal(llvm::Value* slot,
                                                            llvm::SmallVectorImpl<uint64_t>& path)
{
        path.clear();
        llvm::SmallVector<llvm::GEPOperator*, 4> chain;
        llvm::Value* cur = slot;
        while (auto* gep = llvm::dyn_cast_or_null<llvm::GEPOperator>(cur))
        {
            if (chain.size() >= 8) return nullptr;
            chain.push_back(gep);
            cur = gep->getPointerOperand();
        }
        auto* base = llvm::dyn_cast_or_null<llvm::GlobalVariable>(cur);
        if (base == nullptr) return nullptr;

        llvm::Type* ty = base->getValueType();
        for (auto it = chain.rbegin(); it != chain.rend(); ++it)
        {
            llvm::GEPOperator* gep = *it;
            if (!gep->hasAllConstantIndices()) return nullptr;
            if (gep->getSourceElementType() != ty) return nullptr;
            auto idx = gep->idx_begin();
            if (idx == gep->idx_end()) return nullptr;
            if (!llvm::cast<llvm::ConstantInt>(idx->get())->isZero()) return nullptr;
            for (++idx; idx != gep->idx_end(); ++idx)
            {
                const auto* ci = llvm::cast<llvm::ConstantInt>(idx->get());
                if (ci->getBitWidth() > 64 || ci->isNegative()) return nullptr;
                if (ci->getZExtValue() > 0xFFFFFFFFull) return nullptr;
                if (path.size() >= 8) return nullptr;
                path.push_back(ci->getZExtValue());
            }
            ty = gep->getResultElementType();
        }
        return base;
    }

void LLVMBackend::RecordPendingNullIfaceDispatch(const NullIfaceDispatchSite& site, llvm::Value* slot,
                                        llvm::Value* anchor, const std::string& ifaceName)
{
        if (site.VarName.empty() || site.Line <= 0) return;
        auto* anchorInst = llvm::dyn_cast_or_null<llvm::Instruction>(anchor);
        if (slot == nullptr || anchorInst == nullptr) return;
        llvm::BasicBlock* bb = anchorInst->getParent();
        if (bb == nullptr || bb->getParent() == nullptr) return;

        // A GLOBAL receiver goes to its own ledger: its null-ness is a module-level initializer
        // and the "never assigned" fact is whole-module, neither of which is knowable here.
        if (auto* gv = llvm::dyn_cast<llvm::GlobalVariable>(slot))
        {
            pendingNullIfaceGlobal_.push_back(
                { gv, anchorInst, site.VarName, site.MemberName, ifaceName, site.IsField,
                  site.Line, site.Col, {} });
            return;
        }

        llvm::SmallVector<uint64_t, 4> path;
        llvm::AllocaInst* base = ResolveIfaceStorageLoc(slot, path);
        if (base != nullptr)
        {
            if (base->getFunction() != bb->getParent()) return;
            // A sub-object receiver must be nameable as written, or the diagnostic would name
            // the container instead of the thing that is null. No name, no record.
            std::string varName = site.VarName;
            if (!path.empty())
            {
                if (site.ReceiverText.empty()) return;
                varName = site.ReceiverText;
            }
            pendingNullIfaceDispatch_[bb->getParent()].push_back(
                { base, path, anchorInst, varName, site.MemberName, ifaceName, site.IsField,
                  site.Line, site.Col });
            return;
        }

        // Not a frame-local - try a FIELD/ELEMENT of a global: the base is a GlobalVariable
        // reached through a constant-expression GEP chain rather than a literal GlobalVariable
        // slot. Same naming rule as the sub-object-of-alloca case above.
        llvm::SmallVector<uint64_t, 4> gpath;
        llvm::GlobalVariable* gbase = ResolveIfaceStorageGlobal(slot, gpath);
        if (gbase == nullptr) return;
        std::string gVarName = site.VarName;
        if (!gpath.empty())
        {
            if (site.ReceiverText.empty()) return;
            gVarName = site.ReceiverText;
        }
        pendingNullIfaceGlobal_.push_back(
            { gbase, anchorInst, gVarName, site.MemberName, ifaceName, site.IsField,
              site.Line, site.Col, gpath });
    }

void LLVMBackend::DiscardPendingNullIfaceDispatch(llvm::Function* F)
{
        if (!F) return;
        pendingNullIfaceDispatch_.erase(F);
        // The global ledger survives to module end, so an aborted body's records must be
        // removed here or the control-dependence test would run on a partial CFG.
        std::erase_if(pendingNullIfaceGlobal_, [F](const PendingNullIfaceGlobalAccess& r)
        {
            auto* inst = llvm::dyn_cast_or_null<llvm::Instruction>(NullIfaceHandleValue(r.Anchor));
            return inst == nullptr || inst->getFunction() == F;
        });
    }

void LLVMBackend::SetSourceLocation(size_t line, size_t column)
{
        currentLine = line;
        currentColumn = column;
}

void LLVMBackend::LogErrorMessage(std::string englishTemplate,
                                  std::vector<std::string> arguments) const
{
        EmitError(diagnosticLocalization_.Localize(englishTemplate, arguments),
                  DiagnosticLocalization::FormatSourceTemplate(englishTemplate, arguments));
}

void LLVMBackend::LogError(std::string message) const
{
        EmitError(std::move(message));
}

void LLVMBackend::LogRawError(std::string message) const
{
        EmitError(std::move(message));
}

void LLVMBackend::EmitError(std::string message, std::string sourceMessage) const
{
        // Speculative compile-time evaluation: swallow the diagnostic and unwind. The construct
        // is re-evaluated for real later, where the diagnostic (if any) fires normally.
        if (suppressErrors_)
            throw SpeculativeEvalAbort{};
        // An expect_error match is not a real diagnostic - test it before the sink dispatch so it
        // never reaches the editor as a live error, nor aborts LSP analysis of the rest of the file.
        const std::string& matchMessage = sourceMessage.empty() ? message : sourceMessage;
        bool isExpectedMatch = !expectedError.empty()
            && matchMessage.find(expectedError) != std::string::npos;
        if (diagnosticSink_)
        {
            if (isExpectedMatch)
                throw ExpectedErrorReceived{};
            // LSP mode: cout is redirected to stderr and would duplicate the diagnostic already
            // sent to the client over the sink. Don't echo.
            diagnosticSink_(sourceFileName, currentLine, currentColumn, message, 1);
            throw CompilerAbortException{ message, sourceFileName, currentLine, currentColumn };
        }
        std::cout << std::format("{}({},{}): {}\n", sourceFileName, currentLine, currentColumn, message);
        if (!expectedError.empty())
        {
            if (isExpectedMatch)
            {
                std::cout << "PASS: expected error received\n";
                throw ExpectedErrorReceived{};
            }
            std::cout << std::format("FAIL: expected error '{}' but got '{}'\n", expectedError, message);
            FailCompilation(message);
        }
        FailCompilation(message);
    }

[[noreturn]] void LLVMBackend::FailCompilation(const std::string& message) const
{
        if (batchMode_)
            throw CompilerAbortException{ message, sourceFileName, currentLine, currentColumn };
        exit(1);
    }

void LLVMBackend::LogWarning(std::string message) const
{
        if (diagnosticSink_)
        {
            // LSP mode: route through the sink so warnings appear in the editor,
            // not just on stderr where they were invisible.
            diagnosticSink_(sourceFileName, currentLine, currentColumn, message, 2);
            return;
        }
        std::cout << std::format("{}({},{}): warning: {}\n", sourceFileName, currentLine, currentColumn, message);
    }

bool LLVMBackend::FieldSatisfiesThreadDiscipline(const TypeAndValue& field) const
{
        if (!field.GuardedBy.empty())
            return true;
        if (xthreadScanLevel_ < 3 && field.TypeName.rfind("atomic", 0) == 0)
            return true;
        return false;
    }

void LLVMBackend::SetTypeAnnotations(const std::string& name, std::vector<AnnotationValue> anns)
{
        if (anns.empty()) typeAnnotations_.erase(name);
        else typeAnnotations_[name] = std::move(anns);
    }

const LLVMBackend::AnnotationValue* LLVMBackend::FindTypeAnnotation(const std::string& name, const std::string& annName) const
{
        auto it = typeAnnotations_.find(name);
        if (it == typeAnnotations_.end()) return nullptr;
        for (const auto& a : it->second)
            if (a.Name == annName) return &a;
        return nullptr;
    }

std::string LLVMBackend::GetTypeAnnotationArg(const std::string& name, const std::string& annName) const
{
        auto* a = FindTypeAnnotation(name, annName);
        return a ? a->Value : std::string{};
    }

void LLVMBackend::RestoreFileScopeExpectedError()
{
        expectedError = fileScopeExpectedError_;
        expectedErrorScopeDepth = SIZE_MAX;
    }

llvm::DIFile* LLVMBackend::GetDIFileForCurrentSource()
{
        if (!diBuilder) return nullptr;
        const std::string& p = currentSourceFilePath_;
        if (p.empty()) return diFile;
        auto it = diFileCache_.find(p);
        if (it != diFileCache_.end()) return it->second;
        // Split path into directory + filename manually to avoid needing <filesystem>
        // in this header.
        size_t slash = p.find_last_of("/\\");
        std::string fname = (slash == std::string::npos) ? p : p.substr(slash + 1);
        std::string dir   = (slash == std::string::npos) ? std::string() : p.substr(0, slash);
        auto* f = diBuilder->createFile(fname, dir);
        diFileCache_[p] = f;
        return f;
    }

llvm::Function* LLVMBackend::createFunctionProto(const std::string& name, llvm::FunctionType* returnType)
{
        auto fn = llvm::Function::Create(returnType, llvm::Function::ExternalLinkage, name, *module);

        // CFlat treats null pointer dereferences as defined behavior (hardware fault -> SEH).
        // NullPointerIsValid prevents instcombine from removing null loads/stores as UB.
        fn->addFnAttr(llvm::Attribute::NullPointerIsValid);

        llvm::verifyFunction(*fn);

        return fn;
    }

void LLVMBackend::SetVariableRefCountStorage(const std::string& varName, llvm::Value* refStorage)
{
        for (auto& frame : stackNamedVariable)
        {
            auto it = frame.namedVariable.find(varName);
            if (it != frame.namedVariable.end())
            {
                it->second.RefCountStorage = refStorage;
                return;
            }
        }
    }

// Set or CLEAR the `new T[n]` provenance of a raw pointer local. Clearing on any other source is
// the point: a stale-positive flag costs a copy, a stale-negative one would be a use-after-free.
void LLVMBackend::SetVariableRawNewArray(const std::string& varName, bool value,
                                         llvm::Value* rawArrayLength)
{
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            auto it = frame.namedVariable.find(varName);
            if (it != frame.namedVariable.end())
            {
                it->second.AllocatedByRawNewArray = value;
                StoreRawArrayLength(it->second, rawArrayLength);
                return;
            }
        }
    }

// The reassignment twin of ParseDeclaration consuming lastAllocAlignment. Without it a local
// first assigned AFTER its declaration froze at alignment 0 and freed an over-aligned block
// through plain `operator delete`, which corrupts the heap.
void LLVMBackend::SetVariableAllocAlignment(const std::string& varName, uint64_t allocAlign)
{
        if (varName.empty() || allocAlign == 0) return;
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            auto it = frame.namedVariable.find(varName);
            if (it != frame.namedVariable.end())
            {
                it->second.AllocAlignment = std::max(it->second.AllocAlignment, allocAlign);
                return;
            }
        }
    }

llvm::Value* LLVMBackend::LoadRawArrayLength(const NamedVariable& namedVar)
{
        auto* i64Ty = builder->getInt64Ty();
        if (namedVar.RawArrayLengthStorage != nullptr)
            return builder->CreateLoad(i64Ty, namedVar.RawArrayLengthStorage, "raw_array_count");
        llvm::Value* count = namedVar.RawArrayLength;
        if (count == nullptr) count = RawArrayCountOf(namedVar.Primary);
        return count != nullptr ? Upconvert(count, i64Ty) : nullptr;
    }

void LLVMBackend::StoreRawArrayLength(const NamedVariable& namedVar, llvm::Value* rawArrayLength)
{
        if (namedVar.RawArrayLengthStorage == nullptr) return;
        auto* count = rawArrayLength != nullptr
            ? Upconvert(rawArrayLength, builder->getInt64Ty())
            : builder->getInt64(-1);
        builder->CreateStore(count, namedVar.RawArrayLengthStorage);
    }

void LLVMBackend::SetVariableOwning(const std::string& varName, bool value)
{
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            auto it = frame.namedVariable.find(varName);
            if (it != frame.namedVariable.end())
            {
                it->second.IsOwning = value;
                return;
            }
        }
    }

bool LLVMBackend::IsVariableOwning(const std::string& name) const
{
        if (name.empty()) return false;
        for (const auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
                return it->second.IsOwning;
            if (auto it = frame.functionArgument.find(name); it != frame.functionArgument.end())
                return it->second.IsOwning;
        }
        return false;
    }

bool LLVMBackend::IsVariableBorrowedOwningValue(const std::string& name) const
{
        if (name.empty()) return false;
        for (const auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
                return it->second.IsBorrowedOwningValue;
            if (auto it = frame.functionArgument.find(name); it != frame.functionArgument.end())
                return it->second.IsBorrowedOwningValue;
        }
        return false;
    }

LLVMBackend::VarStorageRef LLVMBackend::FindVariableStorage(const std::string& name) const
{
        if (name.empty()) return {};
        for (const auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
                return { it->second.Storage, it->second.BaseType };
            if (auto it = frame.functionArgument.find(name); it != frame.functionArgument.end())
                return { it->second.Storage, it->second.BaseType };
        }
        return {};
    }

LLVMBackend::NamedVariable* LLVMBackend::FindLiveNamedVariable(const std::string& name)
{
        if (name.empty()) return nullptr;
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
                return &it->second;
            if (auto it = frame.functionArgument.find(name); it != frame.functionArgument.end())
                return &it->second;
        }
        return nullptr;
    }

bool LLVMBackend::IsVariableOwningString(const std::string& name) const
{
        if (name.empty()) return false;
        for (const auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
                return it->second.IsOwningString;
            if (auto it = frame.functionArgument.find(name); it != frame.functionArgument.end())
                return it->second.IsOwningString;
        }
        return false;
    }

bool LLVMBackend::IsVariableBorrowingOwnedString(const std::string& name) const
{
        if (name.empty()) return false;
        for (const auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
                return it->second.BorrowsOwnedString;
            if (auto it = frame.functionArgument.find(name); it != frame.functionArgument.end())
                return it->second.BorrowsOwnedString;
        }
        return false;
    }

void LLVMBackend::SetVariableBorrowsOwnedString(const std::string& name, bool value)
{
        if (name.empty()) return;
        auto* here = builder != nullptr ? builder->GetInsertBlock() : nullptr;
        auto* function = here != nullptr ? here->getParent() : nullptr;
        auto apply = [&](NamedVariable& nv) {
            if (value)
            {
                nv.BorrowsOwnedString = true;
                nv.OwnedStringBorrowBlock = here;
                nv.OwnedStringBorrowFunction = function;
            }
            else if (nv.BorrowsOwnedString
                     && nv.OwnedStringBorrowBlock == here
                     && nv.OwnedStringBorrowFunction == function)
            {
                nv.BorrowsOwnedString = false;
                nv.OwnedStringBorrowBlock = nullptr;
                nv.OwnedStringBorrowFunction = nullptr;
            }
        };
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
                { apply(it->second); return; }
            if (auto it = frame.functionArgument.find(name); it != frame.functionArgument.end())
                { apply(it->second); return; }
        }
    }

void LLVMBackend::SetVariableBorrowsOwnedElement(const std::string& name, bool value,
        const std::string& container, bool externallyOwned)
{
        if (name.empty()) return;
        auto* here = builder != nullptr ? builder->GetInsertBlock() : nullptr;
        auto* function = here != nullptr ? here->getParent() : nullptr;
        auto apply = [&](NamedVariable& nv) {
            if (value)
            {
                nv.BorrowsOwnedElement = true;
                nv.OwnedElementContainer = container;
                nv.BorrowedElementExternallyOwned = externallyOwned;
                nv.OwnedElementBorrowBlock = here;
                nv.OwnedElementBorrowFunction = function;
            }
            else if (nv.BorrowsOwnedElement
                     && nv.OwnedElementBorrowBlock == here
                     && nv.OwnedElementBorrowFunction == function)
            {
                nv.BorrowsOwnedElement = false;
                nv.OwnedElementContainer.clear();
                nv.BorrowedElementExternallyOwned = false;
                nv.OwnedElementBorrowBlock = nullptr;
                nv.OwnedElementBorrowFunction = nullptr;
            }
        };
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
                { apply(it->second); return; }
            if (auto it = frame.functionArgument.find(name); it != frame.functionArgument.end())
                { apply(it->second); return; }
        }
    }

void LLVMBackend::EmitConditionalOwningPtrCleanup(const NamedVariable& namedVar, llvm::Value* refCount)
{
        auto* zeroCond = builder->CreateICmpEQ(refCount, builder->getInt32(0), "refiszero");
        auto* freeBB = llvm::BasicBlock::Create(*context, "refcount.free", builder->GetInsertBlock()->getParent());
        auto* skipBB = llvm::BasicBlock::Create(*context, "refcount.skip", builder->GetInsertBlock()->getParent());
        builder->CreateCondBr(zeroCond, freeBB, skipBB);
        builder->SetInsertPoint(freeBB);
        EmitOwningPtrCleanup(namedVar);
        builder->CreateBr(skipBB);
        builder->SetInsertPoint(skipBB);
    }

void LLVMBackend::EmitOwningPtrCleanup(const NamedVariable& namedVar, llvm::Value* replacement)
{
        // Load the current pointer value from the alloca
        auto* ptrVal = builder->CreateLoad(namedVar.BaseType, namedVar.Storage);

        // Skip if null (pointer may have been moved out)
        llvm::Value* skipCleanup = builder->CreateICmpEQ(
            ptrVal,
            llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(namedVar.BaseType)));
        if (replacement != nullptr && replacement->getType() == ptrVal->getType())
            skipCleanup = builder->CreateOr(
                skipCleanup, builder->CreateICmpEQ(ptrVal, replacement, "move.same"));
        auto* cleanupBB = llvm::BasicBlock::Create(*context, "move.cleanup", builder->GetInsertBlock()->getParent());
        auto* afterBB   = llvm::BasicBlock::Create(*context, "move.after",   builder->GetInsertBlock()->getParent());
        builder->CreateCondBr(skipCleanup, afterBB, cleanupBB);

        builder->SetInsertPoint(cleanupBB);

        // Call the full destructor (user dtor + member fields) if the type needs one. Resolve
        // through the delete-site resolver: a pointee still incomplete here (self-referential
        // element) binds the deferred stub instead of silently dropping the call.
        if (namedVar.RawArrayLengthStorage != nullptr || namedVar.RawArrayLength != nullptr)
        {
            auto* dtor = GetFullDestructorForDelete(namedVar.TypeAndValue.TypeName);
            if (dtor != nullptr)
            {
                auto* count = LoadRawArrayLength(namedVar);
                auto* arrayBB = llvm::BasicBlock::Create(
                    *context, "raw_array_dtor_array", builder->GetInsertBlock()->getParent());
                auto* scalarBB = llvm::BasicBlock::Create(
                    *context, "raw_array_dtor_scalar", builder->GetInsertBlock()->getParent());
                auto* doneBB = llvm::BasicBlock::Create(
                    *context, "raw_array_dtor_done", builder->GetInsertBlock()->getParent());
                builder->CreateCondBr(
                    builder->CreateICmpSGE(count, builder->getInt64(0)), arrayBB, scalarBB);
                builder->SetInsertPoint(arrayBB);
                EmitCountedArrayDestruction(ptrVal, namedVar.TypeAndValue.TypeName, count);
                builder->CreateBr(doneBB);
                builder->SetInsertPoint(scalarBB);
                builder->CreateCall(dtor->getFunctionType(), dtor, { ptrVal });
                builder->CreateBr(doneBB);
                builder->SetInsertPoint(doneBB);
            }
        }
        else if (auto* dtor = GetFullDestructorForDelete(namedVar.TypeAndValue.TypeName))
            builder->CreateCall(dtor->getFunctionType(), dtor, { ptrVal });

        // Free the pointer. An over-aligned block came from the aligned allocator, so it must be
        // freed via __delete_aligned to match. Two sources: the element TYPE's own alignment
        // (`struct alignas(64) T`), recovered here from the static type just like the `delete`
        // path does, and any per-site `new T[n] alignas(N)` excess carried on the local.
        auto* voidPtrTy = builder->getInt8Ty()->getPointerTo();
        auto* voidPtr = builder->CreateBitCast(ptrVal, voidPtrTy);
        uint64_t effAlign = namedVar.AllocAlignment;
        if (!namedVar.TypeAndValue.TypeName.empty() && !namedVar.TypeAndValue.ElemPointer)
        {
            TypeAndValue tv{ .TypeName = namedVar.TypeAndValue.TypeName };
            llvm::Type* t = GetType(tv);
            if (t != nullptr && t->isSized())
                effAlign = std::max(effAlign, GetEffectiveAlignmentForType(tv.TypeName, t));
        }
        llvm::Function* alignedDel = effAlign > kDefaultNewAlign
            ? GetFunction("__delete_aligned") : nullptr;
        if (alignedDel)
            builder->CreateCall(alignedDel->getFunctionType(), alignedDel, { voidPtr });
        else if (auto* opDel = GetFunction("operator delete"))
            builder->CreateCall(opDel->getFunctionType(), opDel, { voidPtr });

        builder->CreateBr(afterBB);
        builder->SetInsertPoint(afterBB);
    }

void LLVMBackend::EmitCountedArrayDestruction(llvm::Value* ptrVal,
                                               const std::string& typeName,
                                               llvm::Value* count)
{
        auto* dtor = GetFullDestructorForDelete(typeName);
        auto* ptrTy = llvm::dyn_cast_or_null<llvm::PointerType>(ptrVal ? ptrVal->getType() : nullptr);
        if (dtor == nullptr || ptrTy == nullptr || count == nullptr) return;

        auto* i64Ty = builder->getInt64Ty();
        count = Upconvert(count, i64Ty);
        auto* fn = builder->GetInsertBlock()->getParent();
        auto* condBB = llvm::BasicBlock::Create(*context, "array_dtor_cond", fn);
        auto* bodyBB = llvm::BasicBlock::Create(*context, "array_dtor_body", fn);
        auto* doneBB = llvm::BasicBlock::Create(*context, "array_dtor_done", fn);
        auto* indexAlloca = CreateAlloca(i64Ty);
        builder->CreateStore(count, indexAlloca);
        auto* isNull = builder->CreateICmpEQ(ptrVal, llvm::ConstantPointerNull::get(ptrTy));
        auto* isEmpty = builder->CreateICmpSLE(count, builder->getInt64(0));
        builder->CreateCondBr(builder->CreateOr(isNull, isEmpty), doneBB, condBB);

        builder->SetInsertPoint(condBB);
        auto* index = builder->CreateLoad(i64Ty, indexAlloca);
        builder->CreateCondBr(
            builder->CreateICmpSGT(index, builder->getInt64(0)), bodyBB, doneBB);
        builder->SetInsertPoint(bodyBB);
        auto* next = builder->CreateSub(index, builder->getInt64(1));
        auto* elemType = GetType(TypeAndValue{ .TypeName = typeName });
        auto* elemPtr = builder->CreateGEP(elemType, ptrVal, next, "array_dtor_elem");
        builder->CreateCall(dtor->getFunctionType(), dtor, { elemPtr });
        builder->CreateStore(next, indexAlloca);
        builder->CreateBr(condBB);
        builder->SetInsertPoint(doneBB);
    }

bool LLVMBackend::IsOwningInterfaceValue(const NamedVariable& namedVar) const
{
        return namedVar.IsOwning && namedVar.Storage != nullptr
            && (namedVar.TypeAndValue.IsUnique || namedVar.TypeAndValue.IsUniqueTypeArg)
            && namedVar.TypeAndValue.IsFatInterfaceValue();
    }

void LLVMBackend::EmitOwningInterfaceCleanup(const NamedVariable& namedVar)
{
        auto* fatVal = builder->CreateLoad(GetFatPtrType(), namedVar.Storage);
        DeleteInterfaceValue(fatVal, namedVar.TypeAndValue.TypeName, namedVar.Storage);
    }

bool LLVMBackend::IsOwningUniqueArray(const NamedVariable& namedVar) const
{
        // IsOwning is deliberately NOT required: it is set from a scalar's single `new` source and
        // an array has none. `unique` on the declaration is itself the ownership statement here.
        if (namedVar.Storage == nullptr || namedVar.BaseType == nullptr) return false;
        if (namedVar.IsAliasBorrow || namedVar.TypeAndValue.IsAlias) return false;
        if (!namedVar.TypeAndValue.IsUnique && !namedVar.TypeAndValue.IsUniqueTypeArg) return false;
        if (namedVar.TypeAndValue.ConstArraySize == 0) return false;
        return namedVar.BaseType->isArrayTy();
    }

void LLVMBackend::EmitOwningUniqueArrayCleanup(const NamedVariable& namedVar)
{
        auto* arrTy = llvm::cast<llvm::ArrayType>(namedVar.BaseType);
        auto* elemTy = arrTy->getElementType();
        uint64_t count = arrTy->getNumElements();
        bool isIface = namedVar.TypeAndValue.IsFatInterfaceValue();
        for (uint64_t i = 0; i < count; i++)
        {
            auto* elemPtr = builder->CreateConstInBoundsGEP2_64(arrTy, namedVar.Storage, 0, i, "uniq.elem");
            NamedVariable elem = namedVar;
            elem.Storage = elemPtr;
            elem.BaseType = elemTy;
            elem.TypeAndValue.ConstArraySize = 0;
            if (isIface)
                EmitOwningInterfaceCleanup(elem);
            else if (elemTy->isPointerTy())
                EmitOwningPtrCleanup(elem);
        }
    }

void LLVMBackend::RegisterOwnedStringTemp(llvm::Value* value)
{
        if (value == nullptr) return;
        // Idempotent: an SSA value owns exactly one buffer, so it must be freed once.
        // A comparison operand that is an operator+ result is registered here both by
        // TryBinaryOperatorOverload and by the comparison operand pass - dedup avoids a
        // double free at flush.
        for (const auto& e : pendingOwnedStringTemps)
            if (e.first == value) return;
        pendingOwnedStringTemps.emplace_back(value, builder->GetInsertBlock());
    }

bool LLVMBackend::IsPendingOwnedStringTemp(llvm::Value* value) const
{
        if (value == nullptr) return false;
        for (const auto& e : pendingOwnedStringTemps)
            if (e.first == value) return true;
        return false;
    }

void LLVMBackend::UnregisterOwnedStringTemp(llvm::Value* value)
{
        if (value == nullptr) return;
        std::erase_if(pendingOwnedStringTemps,
            [&](const std::pair<llvm::Value*, llvm::BasicBlock*>& e) { return e.first == value; });
    }

void LLVMBackend::RegisterOwnedReturnTemp(llvm::Value* value, const std::string& fnName,
                                 const TypeAndValue& retType)
{
        if (value == nullptr) return;
        bool isOwningPtr = retType.Pointer && value->getType()->isPointerTy()
            && !retType.IsInterface && !retType.ElemPointer && retType.ConstArraySize == 0;
        for (auto& e : ownedReturnTemps_)
            if (e.Value == value) { e.FnName = fnName; break; }
        if (std::none_of(ownedReturnTemps_.begin(), ownedReturnTemps_.end(),
                         [&](const OwnedReturnTemp& e) { return e.Value == value; }))
            ownedReturnTemps_.push_back({ value, fnName });
        for (auto& e : ownedReturnReleaseTemps_)
            if (e.Value == value)
            {
                e.TypeName = retType.TypeName;
                e.AllocAlign = retType.AllocAlignValue;
                e.IsOwningPtr = isOwningPtr;
                return;
            }
        ownedReturnReleaseTemps_.push_back({ value, retType.TypeName,
                                             retType.AllocAlignValue, isOwningPtr });
}

void LLVMBackend::PropagateOwnedReturnTemp(llvm::Value* from, llvm::Value* to)
{
        const OwnedReturnTemp* src = FindOwnedReturnEntryForDiagnostic(from);
        if (to == nullptr) return;
        if (src != nullptr)
        {
            OwnedReturnTemp copy = *src;
            copy.Value = to;
            bool replaced = false;
            for (auto& e : ownedReturnTemps_)
                if (e.Value == to) { e = copy; replaced = true; break; }
            if (!replaced) ownedReturnTemps_.push_back(copy);
        }
        const OwnedReturnReleaseTemp* release = FindOwnedReturnEntry(from);
        if (release == nullptr) return;
        OwnedReturnReleaseTemp copy = *release;
        copy.Value = to;
        for (auto& e : ownedReturnReleaseTemps_)
            if (e.Value == to) { e = copy; return; }
        ownedReturnReleaseTemps_.push_back(copy);
}

const LLVMBackend::OwnedReturnReleaseTemp* LLVMBackend::FindOwnedReturnEntry(llvm::Value* value) const
{
        if (value == nullptr) return nullptr;
        for (const auto& e : ownedReturnReleaseTemps_)
            if (e.Value == value) return &e;
        return nullptr;
}

const LLVMBackend::OwnedReturnTemp* LLVMBackend::FindOwnedReturnEntryForDiagnostic(llvm::Value* value) const
{
        if (value == nullptr) return nullptr;
        for (const auto& e : ownedReturnTemps_)
            if (e.Value == value) return &e;
        return nullptr;
    }

const std::string* LLVMBackend::FindOwnedReturnTemp(llvm::Value* value) const
{
        const OwnedReturnTemp* e = FindOwnedReturnEntryForDiagnostic(value);
        return e == nullptr ? nullptr : &e->FnName;
    }

void LLVMBackend::RegisterOwnedPtrTemp(llvm::Value* value)
{
        std::string typeName;
        uint64_t allocAlign = 0;
        if (const OwnedReturnReleaseTemp* e = FindOwnedReturnEntry(value); e != nullptr && e->IsOwningPtr)
        {
            typeName = e->TypeName;
            allocAlign = e->AllocAlign;
        }
        else if (const OwnedNewTemp* n = FindOwnedNewTemp(value); n != nullptr && !n->TypeName.empty())
        {
            typeName = n->TypeName;
            allocAlign = n->AllocAlign;
        }
        else return;
        for (const auto& p : pendingOwnedPtrTemps)
            if (p.Value == value) return;   // idempotent: one buffer, one free
        pendingOwnedPtrTemps.push_back({ value, typeName, allocAlign, builder->GetInsertBlock() });
    }

bool LLVMBackend::IsOwningPtrTempValue(llvm::Value* value) const
{
        if (value == nullptr || !value->getType()->isPointerTy()) return false;
        const OwnedReturnReleaseTemp* e = FindOwnedReturnEntry(value);
        if (e != nullptr && e->IsOwningPtr) return true;
        const OwnedNewTemp* n = FindOwnedNewTemp(value);
        return n != nullptr && !n->TypeName.empty();
    }

void LLVMBackend::RegisterNonEscapingOwningPtrArgs(llvm::Value* callResult)
{
        auto* call = llvm::dyn_cast_or_null<llvm::CallInst>(callResult);
        if (call == nullptr) return;
        const llvm::Function* callee = call->getCalledFunction();
        if (callee == nullptr || callee->isDeclaration()) return;
        for (unsigned i = 0; i < call->arg_size(); ++i)
        {
            llvm::Value* argVal = call->getArgOperand(i);
            if (!IsOwningPtrTempValue(argVal)) continue;
            if (ParameterRetainsArgument(callee, i)) continue;
            RegisterOwnedPtrTemp(argVal);
        }
    }

void LLVMBackend::ForgetFunctionEscapeMemo(const llvm::Function* fn)
{
        if (fn == nullptr) return;
        std::erase_if(paramRetainsMemo_, [&](const auto& kv) { return kv.first.first == fn; });
        std::erase_if(paramRetainsInProgress_, [&](const auto& k) { return k.first == fn; });
        std::erase_if(paramRetainsPastCallMemo_, [&](const auto& kv) { return kv.first.first == fn; });
        std::erase_if(paramRetainsPastCallInProgress_, [&](const auto& k) { return k.first == fn; });
        std::erase_if(provableRetainsInProgress_, [&](const auto& k) { return k.first == fn; });
        // The record ledger holds this raw Function*, which LLVM may hand back to a later
        // Function::Create; an entry outliving its callee would be resolved against a stranger.
        std::erase_if(tempUniqueFieldArgs_, [&](const TempUniqueFieldArg& e) { return e.Callee == fn; });
        // Same hazard for the deferred RETURN half: a condition naming this Function* would be
        // re-asked against whatever Function::Create hands the recycled address to next.
        auto namesFn = [&](const std::pair<const llvm::Function*, unsigned>& c) { return c.first == fn; };
        std::erase_if(deferredTempUniqueFieldEscapes_, [&](const DeferredTempUniqueFieldEscape& e) {
            return std::any_of(e.Conds.begin(), e.Conds.end(), namesFn); });
        std::erase_if(pendingLaunderTempUniqueFields_, [&](const PendingLaunderTempUniqueField& e) {
            return std::any_of(e.Conds.begin(), e.Conds.end(), namesFn); });
        std::erase_if(tempUniqueFieldArgs_, [&](const TempUniqueFieldArg& e) {
            return std::any_of(e.LaunderConds.begin(), e.LaunderConds.end(), namesFn); });
        // Same hazard for the borrow-provenance ledger: a recycled Function* would resolve a
        // stranger's calls against this callee's proof, which REJECTS - so drop it here too.
        uniqueFieldBorrowReturns_.erase(fn);
        // The CallInsts INSIDE this body die with it on the discard path, so their addresses
        // recycle too. `fn` is still alive at both call sites, so the parent is still readable.
        std::erase_if(uniqueFieldBorrowResults_, [&](const auto& kv) {
            const auto* inst = llvm::dyn_cast<llvm::Instruction>(kv.first);
            return inst != nullptr && inst->getFunction() == fn;
        });
        std::erase(suspendedFunctions_, fn);
    }

void LLVMBackend::DropModuleEscapeMemo()
{
        paramRetainsMemo_.clear();
        paramRetainsInProgress_.clear();
        paramRetainsPastCallMemo_.clear();
        paramRetainsPastCallInProgress_.clear();
        provableRetainsInProgress_.clear();
        tempUniqueFieldArgs_.clear();
        deferredTempUniqueFieldEscapes_.clear();
        pendingLaunderTempUniqueFields_.clear();
        // Both borrow-provenance ledgers are Function*/CallInst*-keyed, so a module rebuild
        // invalidates them exactly as it does the memos above. Two of the three callers of this
        // helper are module rebuilds OUTSIDE ResetForReanalysis, so clearing here is what covers them.
        uniqueFieldBorrowReturns_.clear();
        uniqueFieldBorrowResults_.clear();
        suspendedFunctions_.clear();
    }

void LLVMBackend::SuppressCallerRelease(llvm::Value* value)
{
        if (value == nullptr) return;
        std::erase_if(ownedReturnReleaseTemps_,
                      [&](const OwnedReturnReleaseTemp& e) { return e.Value == value; });
        std::erase_if(ownedNewTemps_, [&](const OwnedNewTemp& n) { return n.Value == value; });
        UnregisterOwnedPtrTemp(value);
}

bool LLVMBackend::TernaryArmJoinsOwning(llvm::Value* arm)
{
        if (arm == nullptr) return false;
        if (auto* c = llvm::dyn_cast<llvm::Constant>(arm); c != nullptr && c->isNullValue()) return true;
        if (IsOwningPtrTempValue(arm) || IsMovedOutPtrValue(arm)
            || RawArrayResultOwns(arm)) return true;
        // An INTERFACE fat value and a by-value OWNING STRUCT both own through the owning-RETURN
        // release ledger, which the pointer-only IsOwningPtrTempValue cannot see. A plain LOAD of
        // a named local/parameter is never in that
        // ledger, so a borrowed struct arm correctly scores non-owning.
        if (IsInterfaceFatValue(arm) || IsOwningValueStructValue(arm))
            return FindOwnedReturnEntry(arm) != nullptr;
        return false;
    }

bool LLVMBackend::IsOwningValueStructValue(llvm::Value* value)
{
        if (value == nullptr) return false;
        auto* st = llvm::dyn_cast<llvm::StructType>(value->getType());
        if (st == nullptr) return false;
        if (st->hasName() && (st->getName() == "string" || st->getName() == "__iface_fat_ptr"
                              || st->getName() == "__closure_fat_ptr"))
            return false;
        for (const auto& [name, ds] : dataStructures)
            if (ds.StructType == st) return IsOwningValueType(name);
        return false;
    }

void LLVMBackend::RegisterNonOwningStructJoin(llvm::Value* value)
{
        if (value == nullptr) return;
        for (auto* v : nonOwningStructJoins_)
            if (v == value) return;
        nonOwningStructJoins_.push_back(value);
    }

bool LLVMBackend::IsNonOwningStructJoin(llvm::Value* value) const
{
        if (value == nullptr) return false;
        for (auto* v : nonOwningStructJoins_)
            if (v == value) return true;
        return false;
    }

bool LLVMBackend::IsInterfaceFatValue(const llvm::Value* value) const
{
        if (value == nullptr) return false;
        auto* st = llvm::dyn_cast<llvm::StructType>(value->getType());
        return st != nullptr && st->hasName() && st->getName() == "__iface_fat_ptr";
    }

void LLVMBackend::ClearOwnedResultChannels()
{
        lastOwningResult = false;
        lastCallReturnsOwned = false;
        lastAllocAlignment = 0;
        lastCallReturnsAllocAlign = 0;
    }

void LLVMBackend::PropagateMovedBorrowedPtrValue(llvm::Value* trueValue, llvm::Value* falseValue,
                                        llvm::Value* joined)
{
        if (joined == nullptr || !joined->getType()->isPointerTy()) return;
        auto isNullArm = [](llvm::Value* arm) {
            auto* c = llvm::dyn_cast_or_null<llvm::Constant>(arm);
            return c != nullptr && c->isNullValue();
        };
        std::string origin;
        bool trueBorrowMove  = IsMovedBorrowedPtrValue(trueValue, &origin);
        bool falseBorrowMove = IsMovedBorrowedPtrValue(falseValue, trueBorrowMove ? nullptr : &origin);
        if (!trueBorrowMove && !falseBorrowMove) return;
        if (!trueBorrowMove && !isNullArm(trueValue)) return;
        if (!falseBorrowMove && !isNullArm(falseValue)) return;
        RegisterMovedBorrowedPtrValue(joined, origin);
        // Mirror for the field-hop narrowing, off the SAME arm `origin` was taken from, or a
        // diagnostic downstream prescribes `move <param>` for a value that is a field of it.
        if (trueBorrowMove ? IsMovedBorrowedThroughField(trueValue)
                           : IsMovedBorrowedThroughField(falseValue))
            RegisterMovedBorrowedThroughField(joined);
    }

bool LLVMBackend::PropagateTernaryOwnership(llvm::Value* trueValue, llvm::Value* falseValue, llvm::Value* joined)
{
        if (joined == nullptr) return false;
        bool owningStructJoin = IsOwningValueStructValue(joined);
        bool strictJoin = joined->getType()->isPointerTy() || IsInterfaceFatValue(joined)
            || owningStructJoin;
        bool mixedPtrJoin = strictJoin
            && (!TernaryArmJoinsOwning(trueValue) || !TernaryArmJoinsOwning(falseValue));
        // Diagnostic lookup: a suppressed arm must still hand its FnName to the join, or a
        // discarded nested mixed ternary stops being reported.
        if (FindOwnedReturnEntryForDiagnostic(trueValue) != nullptr)
            PropagateOwnedReturnTemp(trueValue, joined);
        else
            PropagateOwnedReturnTemp(falseValue, joined);
        PropagateMovedBorrowedPtrValue(trueValue, falseValue, joined);
        if (mixedPtrJoin)
        {
            SuppressCallerRelease(joined);
            // A struct join carries no runtime owned bit, so suppression must be recorded by VALUE
            // identity: the receiver reads it to borrow instead of adopting.
            if (owningStructJoin) RegisterNonOwningStructJoin(joined);
            return true;
        }
        if (IsOwnedNewTemp(trueValue))
            PropagateOwnedNewTemp(trueValue, joined);
        else
            PropagateOwnedNewTemp(falseValue, joined);
        // A join of `move` arms carries the detachment out on the JOINED value, so a receiver can
        // still recognise the result as owning by value identity rather than by a sticky flag.
        if (IsMovedOutPtrValue(trueValue) || IsMovedOutPtrValue(falseValue))
            RegisterMovedOutPtrValue(joined);
        return false;
    }

void LLVMBackend::PropagateFatInterfaceJoin(llvm::Value* trueValue, llvm::Value* falseValue, llvm::Value* joined)
{
        if (!IsInterfaceFatValue(joined)) return;
        std::string trueIface = FindFatInterfaceValueTypeName(trueValue);
        std::string falseIface = FindFatInterfaceValueTypeName(falseValue);
        std::string ifaceName;
        if (!trueIface.empty() && !falseIface.empty() && trueIface != falseIface)
            ifaceName = kAmbiguousFatInterface;
        else
            ifaceName = !trueIface.empty() ? trueIface : falseIface;
        RegisterFatInterfaceValueTypeName(joined, ifaceName);
    }

bool LLVMBackend::ParameterRetainsArgument(const llvm::Function* fn, unsigned argIndex, int depth)
{
        // A va_arg slot is a C boundary: it cannot retain caller ownership and has no body to walk.
        if (fn == nullptr) return true;
        if (fn->isVarArg() && argIndex >= fn->arg_size()) return false;
        if (argIndex >= fn->arg_size() || depth > kMaxRetainDepth) return true;
        auto key = std::make_pair(fn, argIndex);
        if (auto it = paramRetainsMemo_.find(key); it != paramRetainsMemo_.end()) return it->second;
        // Gate the ANSWER, not just the cache: a half-emitted body has not yet grown the store
        // that escapes, so trusting it would free a pointer the callee goes on to retain.
        if (!FunctionBodyIsComplete(fn)) return true;
        if (!paramRetainsInProgress_.insert(key).second) return true;   // cycle: assume retaining
        bool retains = OwningPtrEscapes(fn->getArg(argIndex), depth);
        paramRetainsInProgress_.erase(key);
        paramRetainsMemo_[key] = retains;
        return retains;
    }

bool LLVMBackend::FunctionBodyIsComplete(const llvm::Function* fn) const
{
        if (fn == nullptr || fn == currentFunction) return false;
        if (std::find(suspendedFunctions_.begin(), suspendedFunctions_.end(), fn)
            != suspendedFunctions_.end()) return false;
        return FunctionBodyIsReadable(fn);
    }

bool LLVMBackend::FunctionBodyIsReadable(const llvm::Function* fn) const
{
        if (fn == nullptr || fn->isDeclaration()) return false;
        for (const auto& bb : *fn)
            if (bb.getTerminator() == nullptr) return false;
        return true;
    }

bool LLVMBackend::ParameterRetainsArgumentPastCall(const llvm::Function* fn, unsigned argIndex,
                                                   int depth)
{
        // Only the variadic portion gets the axiom; declared parameters use the ordinary walk.
        if (fn == nullptr) return true;
        if (fn->isVarArg() && argIndex >= fn->arg_size()) return false;
        if (argIndex >= fn->arg_size() || depth > kMaxRetainDepth) return true;
        auto key = std::make_pair(fn, argIndex);
        if (auto it = paramRetainsPastCallMemo_.find(key); it != paramRetainsPastCallMemo_.end())
            return it->second;
        if (!FunctionBodyIsComplete(fn)) return true;
        if (!paramRetainsPastCallInProgress_.insert(key).second) return true;   // cycle: retaining
        bool retains = OwningPtrEscapes(fn->getArg(argIndex), depth, /*returnIsEscape*/ false);
        paramRetainsPastCallInProgress_.erase(key);
        paramRetainsPastCallMemo_[key] = retains;
        return retains;
    }

/*
 * The tracked pointer's memory is handed to a destructor and then written back over. Proof: the
 * callee is the registered destructor of struct type S, the argument is the tracked address, and
 * a later store in the same block puts an S back at that address. Freeing a field the callee does
 * NOT overwrite is left answering "retains" - no proof, so no caller-side release.
 */
bool LLVMBackend::CallIsOverwrittenFieldDestructor(const llvm::CallBase* call,
                                                   const llvm::Value* tracked) const
{
        if (call == nullptr || call->arg_size() != 1 || call->getArgOperand(0) != tracked)
            return false;
        const llvm::Function* callee = call->getCalledFunction();
        if (callee == nullptr) return false;
        llvm::Type* fieldType = nullptr;
        for (const auto& [name, ds] : dataStructures)
            if (ds.Destructor == callee)
            {
                fieldType = ds.StructType;
                break;
            }
        if (fieldType == nullptr && callee->getName() == "string.dtor")
            fieldType = llvm::StructType::getTypeByName(*context, "string");
        if (fieldType == nullptr) return false;
        for (const llvm::Instruction* i = call->getNextNode(); i != nullptr; i = i->getNextNode())
            if (const auto* st = llvm::dyn_cast<llvm::StoreInst>(i))
                if (st->getPointerOperand() == tracked
                    && st->getValueOperand()->getType() == fieldType) return true;
        return false;
    }

bool LLVMBackend::OwningPtrEscapes(const llvm::Value* root, int depth, bool returnIsEscape)
{
        llvm::SmallPtrSet<const llvm::Value*, 16> visited;
        llvm::SmallVector<const llvm::Value*, 16> work;
        visited.insert(root);
        work.push_back(root);
        while (!work.empty())
        {
            if (visited.size() > kMaxRetainUses) return true;
            const llvm::Value* v = work.pop_back_val();
            for (const llvm::User* u : v->users())
            {
                const auto* inst = llvm::dyn_cast<llvm::Instruction>(u);
                if (inst == nullptr) return true;
                if (llvm::isa<llvm::ICmpInst>(inst)) continue;      // a bool result cannot retain
                if (llvm::isa<llvm::ReturnInst>(inst)) { if (returnIsEscape) return true; continue; }
                if (const auto* st = llvm::dyn_cast<llvm::StoreInst>(inst))
                {
                    if (st->getValueOperand() != v)
                    {
                        // Writing THROUGH the pointer: a scalar, an already-tracked value, a fresh
                        // allocation or a `move` param is fine - a CALLER-owned pointer is not.
                        if (visited.contains(st->getValueOperand())) continue;
                        if (TypeHoldsPointer(st->getValueOperand()->getType())
                            && StoredValueMayBeCallerOwned(st->getValueOperand(), 0)) return true;
                        continue;
                    }
                    // The tracked value is parked in a stack slot (the parameter prologue shape):
                    // track the SLOT, so every later read of it - or of one field of it - is
                    // judged by these same rules. Any other destination is a global or a field.
                    const auto* slot = llvm::dyn_cast<llvm::AllocaInst>(st->getPointerOperand());
                    if (slot == nullptr) return true;
                    if (visited.insert(slot).second) work.push_back(slot);
                    continue;
                }
                if (const auto* ld = llvm::dyn_cast<llvm::LoadInst>(inst))
                {
                    // A scalar read THROUGH the pointer stops here; a pointer (or an aggregate
                    // holding one) dies with the pointee's destructor, so keep following it. A
                    // whole read out of a tracked SLOT is followed whatever its type: a union
                    // lowers to a byte blob the type walk cannot see the pointer through.
                    if ((llvm::isa<llvm::AllocaInst>(v) || TypeHoldsPointer(ld->getType()))
                        && visited.insert(ld).second) work.push_back(ld);
                    continue;
                }
                if (const auto* call = llvm::dyn_cast<llvm::CallBase>(inst))
                {
                    const llvm::Function* callee = call->getCalledFunction();
                    if (callee == nullptr) return true;             // indirect / virtual dispatch
                    if (CallIsPointerOpaqueIntrinsic(callee))
                    {
                        // llvm.mem* with the tracked pointer as SOURCE (operand 1) copies the
                        // pointee's bytes - including any pointer it owns - out of the call.
                        // The destination case is symmetric: caller-owned bytes copied into the
                        // tracked pointee can park an owner there without a StoreInst to inspect.
                        if (callee->getName().starts_with("llvm.mem") && call->arg_size() > 1)
                        {
                            if (call->getArgOperand(1) == v) return true;
                            if (call->getArgOperand(0) == v
                                && TypeHoldsPointer(call->getArgOperand(1)->getType())
                                && StoredValueMayBeCallerOwned(call->getArgOperand(1), depth + 1))
                                return true;
                        }
                        continue;
                    }
                    // The callee destroys a FIELD of the tracked memory and writes a replacement
                    // back over it: a use that ends, not a handle the callee kept.
                    if (CallIsOverwrittenFieldDestructor(call, v)) continue;
                    bool passedAsArg = false;
                    for (unsigned i = 0; i < call->arg_size(); ++i)
                    {
                        if (call->getArgOperand(i) != v) continue;
                        passedAsArg = true;
                        if (!ParameterRetainsArgument(callee, i, depth + 1)) continue;
                        // Retained ONLY by being handed back: the pointer is in MY frame again,
                        // so keep walking the result rather than answering "retains".
                        if (ParameterRetainsArgumentPastCall(callee, i, depth + 1)) return true;
                        if (visited.insert(call).second) work.push_back(call);
                    }
                    if (!passedAsArg) return true;                  // used as the callee operand
                    continue;
                }
                if (const auto* ev = llvm::dyn_cast<llvm::ExtractValueInst>(inst))
                {
                    // Projecting a `string`'s length out of the aggregate copies a scalar; only a
                    // pointer projection can still name the pointee's memory.
                    if (TypeHoldsPointer(ev->getType()) && visited.insert(ev).second)
                        work.push_back(ev);
                    continue;
                }
                if (llvm::isa<llvm::GetElementPtrInst>(inst) || llvm::isa<llvm::BitCastInst>(inst)
                    || llvm::isa<llvm::AddrSpaceCastInst>(inst) || llvm::isa<llvm::PHINode>(inst)
                    || llvm::isa<llvm::SelectInst>(inst))
                {
                    if (visited.insert(inst).second) work.push_back(inst);
                    continue;
                }
                return true;   // ptrtoint, atomics, anything unmodelled: assume it escapes
            }
        }
        return false;
    }

bool LLVMBackend::MemoryOutlivesCall(const llvm::Value* ptr, std::string& destKind, int depth) const
{
        if (ptr == nullptr || depth > kMaxRetainDepth) return false;
        const llvm::Value* obj = llvm::getUnderlyingObject(ptr);
        if (llvm::isa<llvm::GlobalVariable>(obj))
        {
            destKind = "a global";
            return true;
        }
        if (const auto* arg = llvm::dyn_cast<llvm::Argument>(obj))
        {
            destKind = std::format("memory the caller supplied through {}",
                DescribeCalleeParameter(arg->getParent(), arg->getArgNo()));
            return true;
        }
        if (const auto* ld = llvm::dyn_cast<llvm::LoadInst>(obj))
            return MemoryOutlivesCall(ld->getPointerOperand(), destKind, depth + 1)
                || SlotHoldsOutlivingPointer(ld->getPointerOperand(), destKind, depth + 1);
        // getUnderlyingObject stops at a join, so ask the arms: `c ? &g : &g2` is a destination
        // this walk otherwise reads as neither global nor argument, and accepts.
        if (llvm::isa<llvm::SelectInst>(obj) || llvm::isa<llvm::PHINode>(obj))
            return JoinAddressOutlivesCall(llvm::cast<llvm::Instruction>(obj), destKind, depth);
        return false;   // alloca, fresh allocation, unrecognized: no proof, so accept
    }

/*
 * A join of addresses proves the store outlives the call only when EVERY arm proves it. One arm
 * naming a local is a path on which nothing escapes, so `c ? &loc : &g` stays accepted - that
 * accept is what forbids the ANY-arm rule the sibling join walks use on values.
 */
bool LLVMBackend::JoinAddressOutlivesCall(const llvm::Instruction* join, std::string& destKind,
                                          int depth) const
{
        if (join == nullptr || depth > kMaxRetainDepth) return false;
        // Re-entering an in-progress join is a loop back-edge, not an arm naming local memory:
        // answer it as "no counter-example" so the other arms still decide.
        if (!joinAddressInProgress_.insert(join).second) return true;
        llvm::SmallVector<const llvm::Value*, 4> arms;
        if (const auto* sel = llvm::dyn_cast<llvm::SelectInst>(join))
        {
            arms.push_back(sel->getTrueValue());
            arms.push_back(sel->getFalseValue());
        }
        else if (const auto* phi = llvm::dyn_cast<llvm::PHINode>(join))
            for (const llvm::Value* incoming : phi->incoming_values()) arms.push_back(incoming);
        bool proven = !arms.empty();
        bool sameKind = true;
        std::string firstKind;
        for (const llvm::Value* arm : arms)
        {
            std::string armKind;
            if (!MemoryOutlivesCall(arm, armKind, depth + 1)) { proven = false; break; }
            if (armKind.empty()) continue;                  // arm answered by the cycle guard
            if (firstKind.empty()) firstKind = armKind;
            else if (armKind != firstKind) sameKind = false;
        }
        joinAddressInProgress_.erase(join);
        // No arm named a destination (every one was a back-edge): nothing true to report, so no
        // proof rather than a diagnostic that cannot name where the pointer went.
        if (!proven || firstKind.empty()) return false;
        destKind = sameKind ? firstKind : "memory that outlives the call on every arm of a join";
        return true;
    }

bool LLVMBackend::SlotHoldsOutlivingPointer(const llvm::Value* ptr, std::string& destKind,
                                            int depth) const
{
        const auto* slot = llvm::dyn_cast_or_null<llvm::AllocaInst>(ptr);
        if (slot == nullptr || depth > kMaxRetainDepth || !AllocaIsLoadStoreOnly(slot)) return false;
        for (const llvm::User* u : slot->users())
        {
            const auto* st = llvm::dyn_cast<llvm::StoreInst>(u);
            if (st == nullptr || st->getPointerOperand() != slot) continue;
            const llvm::Value* sv = st->getValueOperand();
            if (const auto* arg = llvm::dyn_cast<llvm::Argument>(sv))
            {
                destKind = std::format("memory the caller supplied through {}",
                    DescribeCalleeParameter(arg->getParent(), arg->getArgNo()));
                return true;
            }
            if (llvm::isa<llvm::GlobalVariable>(sv))
            {
                destKind = "a global";
                return true;
            }
            if (const auto* ld = llvm::dyn_cast<llvm::LoadInst>(sv))
                if (MemoryOutlivesCall(ld->getPointerOperand(), destKind, depth + 1)) return true;
            // `Node** p = c > 0 ? &g : &g2;` parks the join in this slot; judge it by its arms.
            if (llvm::isa<llvm::SelectInst>(sv) || llvm::isa<llvm::PHINode>(sv))
                if (JoinAddressOutlivesCall(llvm::cast<llvm::Instruction>(sv), destKind, depth + 1))
                    return true;
        }
        return false;
    }

bool LLVMBackend::ParameterProvablyRetainsArgument(const llvm::Function* fn, unsigned argIndex,
                                                   std::string& destKind, int depth)
{
        if (fn == nullptr || argIndex >= fn->arg_size() || depth > kMaxRetainDepth) return false;
        // A vararg index past the declared parameters never lands on a named parameter, and the
        // arity test above already rejected it - printf-family callees therefore always accept.
        if (!FunctionBodyIsReadable(fn)) return false;
        auto key = std::make_pair(fn, argIndex);
        if (!provableRetainsInProgress_.insert(key).second) return false;   // cycle: no proof
        bool proven = OwningPtrProvablyEscapes(fn->getArg(argIndex), destKind, depth);
        provableRetainsInProgress_.erase(key);
        return proven;
    }

bool LLVMBackend::OwningPtrProvablyEscapes(const llvm::Value* root, std::string& destKind, int depth)
{
        llvm::SmallPtrSet<const llvm::Value*, 16> visited;
        llvm::SmallVector<const llvm::Value*, 16> work;
        visited.insert(root);
        work.push_back(root);
        while (!work.empty())
        {
            if (visited.size() > kMaxRetainUses) return false;   // gave up: no proof
            const llvm::Value* v = work.pop_back_val();
            for (const llvm::User* u : v->users())
            {
                const auto* inst = llvm::dyn_cast<llvm::Instruction>(u);
                if (inst == nullptr) continue;
                if (const auto* st = llvm::dyn_cast<llvm::StoreInst>(inst))
                {
                    // Writing THROUGH the tracked pointer stores something else; only the
                    // tracked value landing in memory can prove the escape.
                    if (st->getValueOperand() != v) continue;
                    const llvm::Value* dest = st->getPointerOperand();
                    if (const auto* slot = llvm::dyn_cast<llvm::AllocaInst>(dest))
                    {
                        if (visited.insert(slot).second) work.push_back(slot);
                        continue;
                    }
                    if (MemoryOutlivesCall(dest, destKind, 0)) return true;
                    continue;
                }
                if (const auto* ld = llvm::dyn_cast<llvm::LoadInst>(inst))
                {
                    // Only follow a read back out of a slot this walk PARKED the value in, and
                    // only while nothing else was ever stored there - otherwise a later
                    // `p = other` would be blamed on the parameter.
                    const auto* slot = llvm::dyn_cast<llvm::AllocaInst>(v);
                    if (slot == nullptr) continue;
                    bool onlyTracked = true;
                    for (const llvm::User* su : slot->users())
                        if (const auto* s2 = llvm::dyn_cast<llvm::StoreInst>(su);
                            s2 != nullptr && s2->getPointerOperand() == slot
                            && !visited.contains(s2->getValueOperand())) onlyTracked = false;
                    if (onlyTracked && visited.insert(ld).second) work.push_back(ld);
                    continue;
                }
                if (const auto* call = llvm::dyn_cast<llvm::CallBase>(inst))
                {
                    const llvm::Function* callee = call->getCalledFunction();
                    if (callee == nullptr) continue;                 // indirect / virtual: no proof
                    if (CallIsPointerOpaqueIntrinsic(callee)) continue;
                    for (unsigned i = 0; i < call->arg_size(); ++i)
                        if (call->getArgOperand(i) == v
                            && ParameterProvablyRetainsArgument(callee, i, destKind, depth + 1))
                            return true;
                    continue;
                }
                if (llvm::isa<llvm::GetElementPtrInst>(inst) || llvm::isa<llvm::BitCastInst>(inst)
                    || llvm::isa<llvm::AddrSpaceCastInst>(inst) || llvm::isa<llvm::PHINode>(inst)
                    || llvm::isa<llvm::SelectInst>(inst))
                {
                    if (visited.insert(inst).second) work.push_back(inst);
                    continue;
                }
                continue;   // return, ptrtoint, anything unmodelled: no proof, so accept
            }
        }
        return false;
    }

bool LLVMBackend::ParameterMayReachReturn(const llvm::Function* fn, unsigned argIndex, int depth)
{
        if (fn == nullptr || argIndex >= fn->arg_size() || depth > kMaxRetainDepth) return false;
        if (fn->getReturnType()->isVoidTy()) return false;
        if (!FunctionBodyIsReadable(fn)) return false;
        auto key = std::make_pair(fn, argIndex);
        if (!mayReachReturnInProgress_.insert(key).second) return false;   // cycle: no proof
        bool reaches = ValueMayReachReturn(fn->getArg(argIndex), depth);
        mayReachReturnInProgress_.erase(key);
        return reaches;
    }

bool LLVMBackend::ValueMayReachReturn(const llvm::Value* root, int depth)
{
        llvm::SmallPtrSet<const llvm::Value*, 16> visited;
        llvm::SmallVector<const llvm::Value*, 16> work;
        visited.insert(root);
        work.push_back(root);
        while (!work.empty())
        {
            if (visited.size() > kMaxRetainUses) return false;   // gave up: no proof
            const llvm::Value* v = work.pop_back_val();
            for (const llvm::User* u : v->users())
            {
                const auto* inst = llvm::dyn_cast<llvm::Instruction>(u);
                if (inst == nullptr) continue;
                if (const auto* ret = llvm::dyn_cast<llvm::ReturnInst>(inst))
                {
                    if (ret->getReturnValue() == v) return true;
                    continue;
                }
                if (const auto* st = llvm::dyn_cast<llvm::StoreInst>(inst))
                {
                    // Park the tracked value in a stack slot - directly, or into a FIELD of one,
                    // which is how a by-value constructor builds its result before returning it.
                    // Writing THROUGH the tracked pointer stores something else.
                    if (st->getValueOperand() != v) continue;
                    const llvm::Value* obj = llvm::getUnderlyingObject(st->getPointerOperand());
                    if (const auto* slot = llvm::dyn_cast<llvm::AllocaInst>(obj))
                        if (visited.insert(slot).second) work.push_back(slot);
                    continue;
                }
                if (llvm::isa<llvm::LoadInst>(inst))
                {
                    // MAY: read back out of a slot this walk parked the value in, whatever else
                    // was stored there. One path handing the pointer back is enough to dangle.
                    if (!llvm::isa<llvm::AllocaInst>(v)) continue;
                    if (visited.insert(inst).second) work.push_back(inst);
                    continue;
                }
                if (const auto* iv = llvm::dyn_cast<llvm::InsertValueInst>(inst))
                {
                    // A by-value constructor lowers to insertvalue + ret with no store at all.
                    if (iv->getInsertedValueOperand() != v) continue;
                    if (visited.insert(iv).second) work.push_back(iv);
                    continue;
                }
                if (const auto* call = llvm::dyn_cast<llvm::CallBase>(inst))
                {
                    const llvm::Function* callee = call->getCalledFunction();
                    if (callee == nullptr) continue;                 // indirect / virtual: no proof
                    if (CallIsPointerOpaqueIntrinsic(callee)) continue;
                    for (unsigned i = 0; i < call->arg_size(); ++i)
                        if (call->getArgOperand(i) == v
                            && ParameterMayReachReturn(callee, i, depth + 1))
                        {
                            if (visited.insert(call).second) work.push_back(call);
                            break;
                        }
                    continue;
                }
                if (llvm::isa<llvm::GetElementPtrInst>(inst) || llvm::isa<llvm::BitCastInst>(inst)
                    || llvm::isa<llvm::AddrSpaceCastInst>(inst) || llvm::isa<llvm::PHINode>(inst)
                    || llvm::isa<llvm::SelectInst>(inst))
                {
                    if (visited.insert(inst).second) work.push_back(inst);
                    continue;
                }
                continue;   // ptrtoint, a read, anything unmodelled: no proof, so accept
            }
        }
        return false;
    }

bool LLVMBackend::ReturnedValueIsExactlyArgument(const llvm::Value* ret, const llvm::Argument* arg,
                                                 int depth) const
{
        if (ret == nullptr || arg == nullptr || depth > kMaxRetainDepth) return false;
        if (ret == arg) return true;
        if (const auto* bc = llvm::dyn_cast<llvm::CastInst>(ret))
            return (llvm::isa<llvm::BitCastInst>(bc) || llvm::isa<llvm::AddrSpaceCastInst>(bc))
                && ReturnedValueIsExactlyArgument(bc->getOperand(0), arg, depth + 1);
        // The parameter-prologue shape: the argument is parked in a slot nothing else is ever
        // stored into, and read back out for the return.
        if (const auto* ld = llvm::dyn_cast<llvm::LoadInst>(ret))
        {
            const auto* slot = llvm::dyn_cast<llvm::AllocaInst>(ld->getPointerOperand());
            if (slot == nullptr || !AllocaIsLoadStoreOnly(slot)) return false;
            bool sawStore = false;
            for (const llvm::User* u : slot->users())
                if (const auto* st = llvm::dyn_cast<llvm::StoreInst>(u);
                    st != nullptr && st->getPointerOperand() == slot)
                {
                    if (!ReturnedValueIsExactlyArgument(st->getValueOperand(), arg, depth + 1))
                        return false;
                    sawStore = true;
                }
            return sawStore;
        }
        // A join is the argument only when EVERY arm is: one arm returning something else means
        // the result and the argument are not one object on that path.
        if (const auto* sel = llvm::dyn_cast<llvm::SelectInst>(ret))
            return ReturnedValueIsExactlyArgument(sel->getTrueValue(), arg, depth + 1)
                && ReturnedValueIsExactlyArgument(sel->getFalseValue(), arg, depth + 1);
        if (const auto* phi = llvm::dyn_cast<llvm::PHINode>(ret))
        {
            if (phi->getNumIncomingValues() == 0) return false;
            for (const llvm::Value* in : phi->incoming_values())
                if (!ReturnedValueIsExactlyArgument(in, arg, depth + 1)) return false;
            return true;
        }
        return false;
    }

bool LLVMBackend::ParameterIsExactlyReturned(const llvm::Function* fn, unsigned argIndex, int depth)
{
        if (fn == nullptr || fn->isVarArg() || argIndex >= fn->arg_size()) return false;
        if (depth > kMaxRetainDepth) return false;
        if (!fn->getReturnType()->isPointerTy()) return false;
        // Unknown-accepts polarity: a callee defined BELOW its call site has no readable body
        // yet, so no proof - the temp keeps leaking rather than being adopted and freed twice.
        if (!FunctionBodyIsComplete(fn)) return false;
        const llvm::Argument* arg = fn->getArg(argIndex);
        if (!arg->getType()->isPointerTy()) return false;
        bool sawReturn = false;
        for (const llvm::BasicBlock& bb : *fn)
            for (const llvm::Instruction& inst : bb)
                if (const auto* ret = llvm::dyn_cast<llvm::ReturnInst>(&inst))
                {
                    sawReturn = true;
                    if (!ReturnedValueIsExactlyArgument(ret->getReturnValue(), arg, 0)) return false;
                }
        if (!sawReturn) return false;
        // ...and the callee kept no OTHER handle on it. Its own return is not an escape here:
        // that is the aliasing the caller is about to adopt, not a second owner.
        return !ParameterRetainsArgumentPastCall(fn, argIndex, depth);
    }

void LLVMBackend::AdoptLaunderedOwningTempResult(llvm::Value* callResult)
{
        auto* call = llvm::dyn_cast_or_null<llvm::CallInst>(callResult);
        if (call == nullptr || !call->getType()->isPointerTy()) return;
        const llvm::Function* callee = call->getCalledFunction();
        if (callee == nullptr) return;
        if (IsOwningPtrTempValue(call)) return;             // already carries its own ownership
        for (unsigned i = 0; i < call->arg_size(); ++i)
        {
            llvm::Value* argVal = call->getArgOperand(i);
            if (!IsOwningPtrTempValue(argVal)) continue;
            if (!ParameterIsExactlyReturned(callee, i)) continue;
            // Move the ledger entry, never copy it: two live entries for one object are two
            // eligible free sites.
            if (const OwnedReturnReleaseTemp* e = FindOwnedReturnEntry(argVal); e != nullptr)
                PropagateOwnedReturnTemp(argVal, call);
            else if (const OwnedNewTemp* n = FindOwnedNewTemp(argVal); n != nullptr)
                RegisterOwnedNewTemp(call, n->TypeName, n->AllocAlign);
            else continue;
            SuppressCallerRelease(argVal);
            return;
        }
    }

void LLVMBackend::RecordUniqueFieldBorrowReturn(const llvm::Function* fn, bool proves,
                                                const std::string& fieldOwner)
{
        if (fn == nullptr) return;
        auto& entry = uniqueFieldBorrowReturns_[fn];
        // Sticky failure in EITHER order: one non-proving return retires the fact for good, so a
        // proving return seen afterwards cannot re-arm it.
        if (!proves) { entry.Failed = true; return; }
        if (entry.SawProof) return;
        entry.SawProof = true;
        entry.FieldOwner = fieldOwner;
    }

void LLVMBackend::MigrateUniqueFieldBorrowReturn(const llvm::Function* oldFn,
                                                 const llvm::Function* newFn)
{
        if (oldFn == nullptr || newFn == nullptr) return;
        auto it = uniqueFieldBorrowReturns_.find(oldFn);
        if (it == uniqueFieldBorrowReturns_.end()) return;
        uniqueFieldBorrowReturns_[newFn] = it->second;
    }

const LLVMBackend::UniqueFieldBorrowReturn* LLVMBackend::FindUniqueFieldBorrowReturn(
    const llvm::Function* fn) const
{
        if (fn == nullptr) return nullptr;
        auto it = uniqueFieldBorrowReturns_.find(fn);
        if (it == uniqueFieldBorrowReturns_.end()) return nullptr;
        return (it->second.SawProof && !it->second.Failed) ? &it->second : nullptr;
    }

void LLVMBackend::RegisterUniqueFieldBorrowResult(llvm::Value* callResult,
                                                  const UniqueFieldBorrowReturn& info)
{
        if (callResult == nullptr) return;
        uniqueFieldBorrowResults_[callResult] = info;
    }

const LLVMBackend::UniqueFieldBorrowReturn* LLVMBackend::FindUniqueFieldBorrowResult(
    const llvm::Value* callResult) const
{
        if (callResult == nullptr) return nullptr;
        auto it = uniqueFieldBorrowResults_.find(callResult);
        return it == uniqueFieldBorrowResults_.end() ? nullptr : &it->second;
    }

std::string LLVMBackend::DescribeUniqueFieldAccess(const NamedVariable& nv)
{
        std::string field = nv.FieldName.empty() ? nv.TypeAndValue.VariableName : nv.FieldName;
        if (field.empty()) return nv.CallerName;
        if (nv.FieldName.empty() || nv.CallerName.empty()) return field;
        return nv.CallerName + "." + field;
    }

void LLVMBackend::RecordTempUniqueFieldArgs(llvm::Value* callResult, const std::string& functionName,
                                            const std::vector<NamedVariable>& args)
{
        auto* call = llvm::dyn_cast_or_null<llvm::CallInst>(callResult);
        if (call == nullptr) return;
        const llvm::Function* callee = call->getCalledFunction();
        if (callee == nullptr) return;
        for (unsigned i = 0; i < call->arg_size(); ++i)
        {
            llvm::Value* argVal = call->getArgOperand(i);
            bool carries = JoinCarriesOwningTempUniqueField(argVal);
            // A chain hop: the argument is itself a call whose callee is still below, so it is
            // only the temp's field if that hop proves. Carry its conditions forward.
            const PendingLaunderTempUniqueField* pendingArg =
                carries ? nullptr : FindPendingLaunderTempUniqueField(argVal);
            if (!carries && pendingArg == nullptr) continue;
            std::string access;
            for (const auto& nv : args)
                if (nv.Primary == argVal) { access = DescribeUniqueFieldAccess(nv); break; }
            TempUniqueFieldArg entry{ callee, i, functionName, access, sourceFileName,
                                      currentLine, currentColumn,
                                      carries && !IsLedgeredOwningTempUniqueField(argVal) };
            if (pendingArg != nullptr) entry.LaunderConds = pendingArg->Conds;
            const LaunderedTempUniqueField* inner = FindLaunderedTempUniqueField(argVal);
            std::string innerAccess = inner != nullptr ? inner->Access
                : (pendingArg != nullptr ? pendingArg->Access : access);
            bool bodyReadable = FunctionBodyIsComplete(callee);
            // The callee may hand the parameter straight back out. Re-ledger the RESULT so the
            // existing escape guards see the laundered value as the temp field it still is.
            if (carries && bodyReadable && ParameterMayReachReturn(callee, i))
            {
                RegisterOwningTempUniqueField(call);
                RegisterLaunderedTempUniqueField(call, functionName, innerAccess);
            }
            else
            {
                // No readable body here, or the hop feeding this one is itself unproven: record
                // the result as a CANDIDATE so the destination sites can defer their own answer.
                std::vector<std::pair<const llvm::Function*, unsigned>> conds =
                    pendingArg != nullptr ? pendingArg->Conds
                                          : std::vector<std::pair<const llvm::Function*, unsigned>>{};
                if (!bodyReadable) conds.push_back({ callee, i });
                else if (!ParameterMayReachReturn(callee, i)) conds.clear();
                RegisterPendingLaunderTempUniqueField(call, conds, functionName, innerAccess);
            }
            std::string destKind;
            // Answer NOW when the body already proves it AND the argument is already known to be
            // the field; otherwise defer (callee defined below, or an unproven chain hop).
            if (carries && entry.LaunderConds.empty() && bodyReadable
                && ParameterProvablyRetainsArgument(callee, i, destKind))
                RejectTempUniqueFieldArgEscape(entry, destKind);
            tempUniqueFieldArgs_.push_back(entry);
        }
    }

bool LLVMBackend::EveryImplementorRetainsInterfaceArg(const std::string& ifaceName,
        const std::string& methodName, size_t arity, unsigned paramIndex,
        std::string& destKind, std::string& implDetail)
{
        std::vector<std::string> impls;
        if (!EnumerateInterfaceImplementors(ifaceName, impls)) return false;
        const InterfaceMethod* method = FindInterfaceMethod(ifaceName, methodName, arity);
        if (method == nullptr || paramIndex >= method->Parameters.size()) return false;
        std::string firstKind, firstImpl;
        for (const std::string& impl : impls)
        {
            llvm::Function* fn = LookupInterfaceMethodImpl(impl, *method);
            // No body to read, or the implementor is emitted later: no proof, so accept.
            if (fn == nullptr || !FunctionBodyIsComplete(fn)) return false;
            std::string kind;
            // +1: the implementor's arg 0 is the receiver the interface parameter list omits.
            if (!ParameterProvablyRetainsArgument(fn, paramIndex + 1, kind)) return false;
            if (firstKind.empty()) { firstKind = kind; firstImpl = impl; }
        }
        if (firstKind.empty()) return false;
        destKind = firstKind;
        implDetail = impls.size() == 1
            ? std::format("'{}.{}' stores it into {}", firstImpl, methodName, firstKind)
            : std::format("all {} implementors store it, e.g. '{}.{}' into {}",
                          impls.size(), firstImpl, methodName, firstKind);
        return true;
    }

bool LLVMBackend::EveryImplementorMayReturnInterfaceArg(const std::string& ifaceName,
        const std::string& methodName, size_t arity, unsigned paramIndex)
{
        std::vector<std::string> impls;
        if (!EnumerateInterfaceImplementors(ifaceName, impls)) return false;
        const InterfaceMethod* method = FindInterfaceMethod(ifaceName, methodName, arity);
        if (method == nullptr || paramIndex >= method->Parameters.size()) return false;
        for (const std::string& impl : impls)
        {
            llvm::Function* fn = LookupInterfaceMethodImpl(impl, *method);
            if (fn == nullptr || !FunctionBodyIsComplete(fn)) return false;
            if (!ParameterMayReachReturn(fn, paramIndex + 1)) return false;
        }
        return true;
    }

/*
 * The interface-dispatch twin of RecordTempUniqueFieldArgs. Runs at the same point in the virtual
 * call as the direct path's recorder: after the move transfer, on the emitted CallInst.
 */
void LLVMBackend::RecordTempUniqueFieldInterfaceArgs(llvm::Value* callResult,
        const std::string& ifaceName, const InterfaceMethod& method,
        const std::vector<NamedVariable>& args)
{
        auto* call = llvm::dyn_cast_or_null<llvm::CallInst>(callResult);
        if (call == nullptr) return;
        for (size_t i = 0; i < method.Parameters.size(); i++)
        {
            const TypeAndValue& param = method.Parameters[i];
            // A sink parameter states the ownership claim at the call site and is answered by the
            // sink path, exactly as in the direct call; only a PLAIN pointer reaches here.
            if (param.IsMove || param.IsUnique || !param.Pointer) continue;
            if (i + 1 >= call->arg_size()) break;
            llvm::Value* argVal = call->getArgOperand((unsigned)(i + 1));
            if (!JoinCarriesOwningTempUniqueField(argVal)) continue;
            std::string access;
            for (const auto& nv : args)
                if (nv.Primary == argVal) { access = DescribeUniqueFieldAccess(nv); break; }
            TempUniqueFieldArg entry{ nullptr, (unsigned)i, ifaceName + "." + method.Name, access,
                                      sourceFileName, currentLine, currentColumn,
                                      !IsLedgeredOwningTempUniqueField(argVal),
                                      ifaceName, method.Name, param.VariableName,
                                      method.Parameters.size() };
            // Return-side laundering, ALL polarity: re-ledgering feeds a REJECTION, so it may only
            // fire when the result IS the temp's field on every implementor that can be dispatched.
            if (EveryImplementorMayReturnInterfaceArg(ifaceName, method.Name,
                                                      method.Parameters.size(), (unsigned)i))
            {
                RegisterOwningTempUniqueField(call);
                const LaunderedTempUniqueField* inner = FindLaunderedTempUniqueField(argVal);
                RegisterLaunderedTempUniqueField(call, entry.CalleeName,
                    inner != nullptr ? inner->Access : access);
            }
            std::string destKind, implDetail;
            // Answer now when every implementor is already emitted, so the diagnostic lands inside
            // any enclosing statement scope; otherwise defer to the end-of-module resolve.
            if (EveryImplementorRetainsInterfaceArg(ifaceName, method.Name,
                    method.Parameters.size(), (unsigned)i, destKind, implDetail))
                RejectTempUniqueFieldInterfaceArgEscape(entry, destKind, implDetail);
            tempUniqueFieldArgs_.push_back(entry);
        }
    }

void LLVMBackend::ResolveTempUniqueFieldArgEscapes()
{
        std::vector<TempUniqueFieldArg> pending;
        pending.swap(tempUniqueFieldArgs_);
        // ONE diagnostic per compile, not one per site: LogError throws out of this loop. The
        // loop shape is about finding the first proven entry, not about reporting them all.
        for (const auto& entry : pending)
        {
            std::string destKind;
            // A chained argument is the temp's field only if every hop below it proves.
            if (!entry.LaunderConds.empty() && !LaunderCondsAllProve(entry.LaunderConds)) continue;
            if (!entry.IfaceName.empty())
            {
                std::string implDetail;
                if (!EveryImplementorRetainsInterfaceArg(entry.IfaceName, entry.MethodName,
                        entry.Arity, entry.ArgIndex, destKind, implDetail)) continue;
                ReportingFileScope fileScope(this, entry.File, entry.Line, entry.Column);
                RejectTempUniqueFieldInterfaceArgEscape(entry, destKind, implDetail);
                continue;
            }
            if (!ParameterProvablyRetainsArgument(entry.Callee, entry.ArgIndex, destKind)) continue;
            // The walk is over, so sourceFileName is the MAIN file again; a call written in an
            // imported module must be reported against the file it was written in. LogError
            // THROWS, so the restore has to be RAII or it never runs.
            ReportingFileScope fileScope(this, entry.File, entry.Line, entry.Column);
            RejectTempUniqueFieldArgEscape(entry, destKind);
        }
    }

bool LLVMBackend::ArgumentIsMethodReceiver(const llvm::Function* fn, unsigned argIndex) const
{
        if (fn == nullptr || argIndex != 0) return false;
        const FunctionSymbol* sym = FindSymbolForFunction(fn);
        if (sym == nullptr || !sym->IsMethod || sym->Parameters.empty()) return false;
        size_t expanded = 0;
        for (const auto& param : sym->Parameters)
            expanded += ParameterCarriesRawArrayCount(param) ? 2u : 1u;
        return fn->arg_size() == expanded || fn->arg_size() == expanded + 1;
    }

std::string LLVMBackend::DescribeCalleeParameter(const llvm::Function* fn, unsigned argIndex) const
{
        if (ArgumentIsMethodReceiver(fn, argIndex)) return "the receiver object";
        if (const FunctionSymbol* sym = FindSymbolForFunction(fn); sym != nullptr)
        {
            size_t expanded = 0;
            for (const auto& param : sym->Parameters)
                expanded += ParameterCarriesRawArrayCount(param) ? 2u : 1u;
            unsigned llvmIndex = sym->IsMethod && fn->arg_size() == expanded + 1 ? 1u : 0u;
            for (const auto& param : sym->Parameters)
            {
                if (llvmIndex == argIndex && !param.VariableName.empty())
                    return std::format("parameter '{}'", param.VariableName);
                llvmIndex += ParameterCarriesRawArrayCount(param) ? 2u : 1u;
            }
        }
        return std::format("parameter #{}", argIndex + 1);
    }

void LLVMBackend::RejectTempUniqueFieldArgEscape(const TempUniqueFieldArg& entry,
                                                 const std::string& destKind)
{
        std::string what = entry.Access.empty()
            ? std::string("a unique field of a temporary")
            : std::format("unique field '{}' of a temporary", entry.Access);
        if (entry.Access.empty() && entry.ThroughJoin)
            what += ", reached through a '?:' / '??' join";
        // A method RECEIVER is not a parameter the caller can re-declare, so `move` is not a
        // remedy for it and the message must not offer one.
        if (ArgumentIsMethodReceiver(entry.Callee, entry.ArgIndex))
        {
            LogError(std::format(
                "call to method '{}': the receiver is {} - '{}' stores the receiver into {}, "
                "which outlives the call, and the temporary's synthesized destructor frees the "
                "pointee at the end of this statement. Bind the whole call result to a local "
                "first and call '{}' on that local.",
                entry.CalleeName, what, entry.CalleeName, destKind, entry.CalleeName));
            return;   // LogError throws; this only says so
        }
        LogError(std::format(
            "call to '{}': cannot pass {} to plain pointer {} - '{}' stores that "
            "pointer into {}, which outlives the call, and the temporary's synthesized destructor "
            "frees the pointee at the end of this statement. Bind the whole call result to a local "
            "first and pass the field off that local, so the pointee outlives the statement - or "
            "declare the parameter 'move' and pass 'move <local>.<field>' to hand ownership over.",
            entry.CalleeName, what, DescribeCalleeParameter(entry.Callee, entry.ArgIndex),
            entry.CalleeName, destKind));
    }

/*
 * The interface-dispatch wording. It must be TRUE of a VIRTUAL site, so it says "every
 * implementor" (the ALL polarity the proof used) and names one, rather than claiming a single
 * callee the call site does not have.
 */
void LLVMBackend::RejectTempUniqueFieldInterfaceArgEscape(const TempUniqueFieldArg& entry,
        const std::string& destKind, const std::string& implDetail)
{
        std::string what = entry.Access.empty()
            ? std::string("a unique field of a temporary")
            : std::format("unique field '{}' of a temporary", entry.Access);
        if (entry.Access.empty() && entry.ThroughJoin)
            what += ", reached through a '?:' / '??' join";
        std::string param = entry.ParamName.empty()
            ? std::format("parameter #{}", entry.ArgIndex + 1)
            : std::format("parameter '{}'", entry.ParamName);
        LogError(std::format(
            "call to interface method '{}': cannot pass {} to plain pointer {} - every "
            "implementor of this method stores that pointer into memory that outlives the call "
            "({}), and the temporary's synthesized destructor frees the pointee at the end of this "
            "statement. Bind the whole call result to a local first and pass the field off that "
            "local, so the pointee outlives the statement - or declare the interface parameter "
            "'move' and pass 'move <local>.<field>' to hand ownership over.",
            entry.CalleeName, what, param, implDetail));
    }

bool LLVMBackend::StoredValueMayBeCallerOwned(const llvm::Value* val, int depth) const
{
        if (val == nullptr || depth > kMaxRetainDepth) return true;
        if (llvm::isa<llvm::Constant>(val)) return llvm::isa<llvm::GlobalValue>(val);
        if (const auto* arg = llvm::dyn_cast<llvm::Argument>(val))
            return !ParameterIsMove(arg->getParent(), arg->getArgNo());
        if (const auto* call = llvm::dyn_cast<llvm::CallBase>(val))
            return !CalleeReturnsOwned(call->getCalledFunction());
        if (const auto* ld = llvm::dyn_cast<llvm::LoadInst>(val))
        {
            const auto* slot = llvm::dyn_cast<llvm::AllocaInst>(ld->getPointerOperand());
            if (slot == nullptr || !AllocaIsLoadStoreOnly(slot)) return true;
            for (const llvm::User* u : slot->users())
                if (const auto* st = llvm::dyn_cast<llvm::StoreInst>(u))
                    if (st->getPointerOperand() == slot
                        && StoredValueMayBeCallerOwned(st->getValueOperand(), depth + 1)) return true;
            return false;
        }
        if (llvm::isa<llvm::PHINode>(val) || llvm::isa<llvm::SelectInst>(val)
            || llvm::isa<llvm::ExtractValueInst>(val) || llvm::isa<llvm::InsertValueInst>(val)
            || llvm::isa<llvm::BitCastInst>(val))
        {
            for (const llvm::Use& op : llvm::cast<llvm::Instruction>(val)->operands())
                if (StoredValueMayBeCallerOwned(op.get(), depth + 1)) return true;
            return false;
        }
        return true;
    }

const LLVMBackend::FunctionSymbol* LLVMBackend::FindSymbolForFunction(const llvm::Function* fn) const
{
        if (fn == nullptr) return nullptr;
        for (const auto& entry : functionTable)
            for (const auto& sym : entry.second)
                if (sym.Function == fn) return &sym;
        return nullptr;
    }

bool LLVMBackend::CalleeReturnsOwned(const llvm::Function* fn) const
{
        const FunctionSymbol* sym = FindSymbolForFunction(fn);
        return sym != nullptr && sym->ReturnsOwned;
    }

bool LLVMBackend::ParameterIsMove(const llvm::Function* fn, unsigned argIndex) const
{
        const FunctionSymbol* sym = FindSymbolForFunction(fn);
        if (sym == nullptr || sym->Recipe.hasLowering) return false;
        if (ArgumentIsMethodReceiver(fn, argIndex)) return false;
        size_t expanded = 0;
        for (const auto& param : sym->Parameters)
            expanded += ParameterCarriesRawArrayCount(param) ? 2u : 1u;
        unsigned llvmIndex = sym->IsMethod && fn->arg_size() == expanded + 1 ? 1u : 0u;
        for (const auto& param : sym->Parameters)
        {
            if (llvmIndex == argIndex) return param.IsMove;
            if (ParameterCarriesRawArrayCount(param) && llvmIndex + 1 == argIndex)
                return false;
            llvmIndex += ParameterCarriesRawArrayCount(param) ? 2u : 1u;
        }
        return false;
    }

bool LLVMBackend::TypeHoldsPointer(const llvm::Type* t) const
{
        if (t == nullptr || t->isPointerTy()) return true;
        // A cflat union lowers to a byte blob, so the field walk below cannot see the pointer
        // arm: answer from the declaration instead.
        if (const auto* st = llvm::dyn_cast<llvm::StructType>(t); st != nullptr && st->hasName())
        {
            auto it = dataStructures.find(st->getName().str());
            if (it != dataStructures.end() && it->second.IsUnion) return true;
        }
        for (unsigned i = 0; i < t->getNumContainedTypes(); ++i)
            if (TypeHoldsPointer(t->getContainedType(i))) return true;
        return false;
    }

bool LLVMBackend::CallIsPointerOpaqueIntrinsic(const llvm::Function* callee) const
{
        llvm::StringRef n = callee->getName();
        return n.starts_with("llvm.dbg.") || n.starts_with("llvm.lifetime.")
            || n.starts_with("llvm.mem");
    }

bool LLVMBackend::AllocaIsLoadStoreOnly(const llvm::AllocaInst* slot) const
{
        for (const llvm::User* u : slot->users())
        {
            if (llvm::isa<llvm::LoadInst>(u)) continue;
            if (const auto* st = llvm::dyn_cast<llvm::StoreInst>(u))
                if (st->getPointerOperand() == slot) continue;
            // Only debug/lifetime markers are inert here: llvm.mem* would copy the parked
            // POINTER VALUE out of the slot, which is an escape.
            if (const auto* call = llvm::dyn_cast<llvm::CallBase>(u))
                if (const llvm::Function* f = call->getCalledFunction(); f != nullptr
                    && (f->getName().starts_with("llvm.dbg.")
                        || f->getName().starts_with("llvm.lifetime."))) continue;
            return false;
        }
        return true;
    }

void LLVMBackend::RegisterOwnedNewTemp(llvm::Value* value, const std::string& typeName, uint64_t allocAlign)
{
        if (value == nullptr || !value->getType()->isPointerTy()) return;
        for (auto& e : ownedNewTemps_)
            if (e.Value == value) return;
        ownedNewTemps_.push_back({ value, typeName, allocAlign });
    }

const LLVMBackend::OwnedNewTemp* LLVMBackend::FindOwnedNewTemp(llvm::Value* value) const
{
        if (value == nullptr) return nullptr;
        for (const auto& e : ownedNewTemps_)
            if (e.Value == value) return &e;
        return nullptr;
    }

bool LLVMBackend::IsOwnedNewTemp(llvm::Value* value) const
{
        return FindOwnedNewTemp(value) != nullptr;
    }

void LLVMBackend::RegisterRawArrayResult(llvm::Value* value, llvm::Value* count,
                                         uint64_t allocAlign, bool owns)
{
        if (value == nullptr || count == nullptr || !value->getType()->isPointerTy()) return;
        for (auto& entry : rawArrayResults_)
            if (entry.Value == value)
            {
                entry.Count = count;
                entry.AllocAlign = allocAlign;
                entry.Owns = entry.Owns || owns;
                return;
            }
        rawArrayResults_.push_back({ value, count, allocAlign, owns });
    }

const LLVMBackend::RawArrayResult* LLVMBackend::FindRawArrayResult(llvm::Value* value) const
{
        if (value == nullptr) return nullptr;
        for (const auto& entry : rawArrayResults_)
            if (entry.Value == value) return &entry;
        return nullptr;
    }

bool LLVMBackend::IsRawArrayResult(llvm::Value* value) const
{
        return FindRawArrayResult(value) != nullptr;
    }

bool LLVMBackend::RawArrayResultOwns(llvm::Value* value) const
{
        const auto* entry = FindRawArrayResult(value);
        return entry != nullptr && entry->Owns;
    }

llvm::Value* LLVMBackend::RawArrayCountOf(llvm::Value* value) const
{
        const auto* entry = FindRawArrayResult(value);
        return entry != nullptr ? entry->Count : nullptr;
    }

void LLVMBackend::PropagateOwnedNewTemp(llvm::Value* from, llvm::Value* to)
{
        const OwnedNewTemp* src = FindOwnedNewTemp(from);
        if (src == nullptr || to == nullptr) return;
        RegisterOwnedNewTemp(to, src->TypeName, src->AllocAlign);
    }

void LLVMBackend::RegisterFatInterfaceValueTypeName(llvm::Value* value, const std::string& ifaceName)
{
        if (value == nullptr || ifaceName.empty() || !IsInterfaceFatValue(value)) return;
        for (auto& entry : fatInterfaceValueTypeNames_)
            if (entry.first == value) { entry.second = ifaceName; return; }
        fatInterfaceValueTypeNames_.push_back({ value, ifaceName });
    }

std::string LLVMBackend::FindFatInterfaceValueTypeName(const llvm::Value* value) const
{
        for (const auto& entry : fatInterfaceValueTypeNames_)
            if (entry.first == value) return entry.second;
        return {};
    }

std::string LLVMBackend::ResolveFatInterfaceSrcName(const llvm::Value* value, const std::string& declaredName) const
{
        return !declaredName.empty() ? declaredName : FindFatInterfaceValueTypeName(value);
    }

void LLVMBackend::RegisterValueElementTypeName(llvm::Value* value, const std::string& typeName)
{
        if (value == nullptr || typeName.empty() || !value->getType()->isPointerTy()) return;
        for (auto& entry : valueElementTypeNames_)
            if (entry.first == value) { entry.second = typeName; return; }
        valueElementTypeNames_.push_back({ value, typeName });
    }

std::string LLVMBackend::FindValueElementTypeName(llvm::Value* value) const
{
        if (value == nullptr) return {};
        for (const auto& entry : valueElementTypeNames_)
            if (entry.first == value) return entry.second;
        return {};
    }

std::string LLVMBackend::FindDeclaredElementTypeNameForStorage(const llvm::Value* storage) const
{
        if (storage == nullptr) return {};
        // Only a SINGLE-LEVEL thin `T*` names the class its loaded value points AT. A `T**`
        // (ElemPointer), a `T[]` view, a simd/const-array slot or an interface slot all load to a
        // bare ptr whose TypeName is the ELEMENT class, so boxing one would attach a class vtable
        // to something that is not an instance of it - silent type confusion.
        auto pick = [](const NamedVariable& nv) -> std::string {
            const auto& tv = nv.TypeAndValue;
            if (!tv.Pointer || tv.ElemPointer || tv.IsArrayView || tv.IsSimd
                || tv.IsInterface || tv.IsInterfacePointer || tv.ConstArraySize != 0)
                return {};
            return tv.TypeName;
        };
        for (const auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            for (const auto& [name, nv] : frame.namedVariable)
                if (nv.Storage == storage) return pick(nv);
            for (const auto& [name, nv] : frame.functionArgument)
                if (nv.Storage == storage) return pick(nv);
        }
        return {};
    }

const LLVMBackend::TypeAndValue* LLVMBackend::FindDeclaredTypeAndValueForStorage(const llvm::Value* storage) const
{
        if (storage == nullptr) return nullptr;
        for (const auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            for (const auto& [name, nv] : frame.namedVariable)
                if (nv.Storage == storage) return &nv.TypeAndValue;
            for (const auto& [name, nv] : frame.functionArgument)
                if (nv.Storage == storage) return &nv.TypeAndValue;
        }
        for (const auto& [name, gVar] : globalNamedVariable)
        {
            if (gVar != storage) continue;
            auto typeIt = globalVariableTypes.find(name);
            return typeIt != globalVariableTypes.end() ? &typeIt->second : nullptr;
        }
        return nullptr;
    }

std::string LLVMBackend::ResolvePointerElementTypeName(llvm::Value* value) const
{
        std::string name = FindValueElementTypeName(value);
        if (!name.empty()) return name;
        if (auto* call = llvm::dyn_cast_or_null<llvm::CallInst>(value))
        {
            const auto* symbol = FindSymbolForFunction(call->getCalledFunction());
            if (symbol == nullptr) return {};
            const auto& tv = symbol->ReturnType;
            if (!tv.Pointer || tv.ElemPointer || tv.IsArrayView || tv.IsSimd
                || tv.IsInterface || tv.IsInterfacePointer || tv.ConstArraySize != 0)
                return {};
            return tv.TypeName;
        }
        auto* load = llvm::dyn_cast_or_null<llvm::LoadInst>(value);
        if (load == nullptr) return {};
        return FindDeclaredElementTypeNameForStorage(load->getPointerOperand());
}

void LLVMBackend::RegisterMovedOutPtrValue(llvm::Value* value)
{
        // A by-value OWNING STRUCT is the third movable shape (its source is zeroed, exactly as a
        // pointer is nulled), so a '?:' join can score a `move` of one owning.
        if (value == nullptr || (!value->getType()->isPointerTy() && !IsInterfaceFatValue(value)
                                 && !IsOwningValueStructValue(value)))
            return;
        for (llvm::Value* v : movedOutPtrValues_)
            if (v == value) return;
        movedOutPtrValues_.push_back(value);
    }

bool LLVMBackend::IsMovedOutPtrValue(llvm::Value* value) const
{
        if (value == nullptr) return false;
        for (llvm::Value* v : movedOutPtrValues_)
            if (v == value) return true;
        return false;
    }

void LLVMBackend::RegisterMovedBorrowedPtrValue(llvm::Value* value, const std::string& originName)
{
        if (value == nullptr || !value->getType()->isPointerTy()) return;
        for (const auto& e : movedBorrowedPtrValues_)
            if (e.first == value) return;
        movedBorrowedPtrValues_.push_back({ value, originName });
    }

void LLVMBackend::RegisterMovedBorrowedThroughField(llvm::Value* value)
{
        if (value == nullptr) return;
        for (auto* v : movedBorrowedThroughFieldValues_)
            if (v == value) return;
        movedBorrowedThroughFieldValues_.push_back(value);
    }

bool LLVMBackend::IsMovedBorrowedThroughField(llvm::Value* value) const
{
        if (value == nullptr) return false;
        for (auto* v : movedBorrowedThroughFieldValues_)
            if (v == value) return true;
        return false;
    }

bool LLVMBackend::IsMovedBorrowedPtrValue(llvm::Value* value, std::string* originOut) const
{
        if (value == nullptr) return false;
        for (const auto& e : movedBorrowedPtrValues_)
            if (e.first == value)
            {
                if (originOut != nullptr) *originOut = e.second;
                return true;
            }
        return false;
    }

void LLVMBackend::ConsumeOwnedNewTemp(llvm::Value* value)
{
        if (value == nullptr) return;
        std::erase_if(ownedNewTemps_, [&](const OwnedNewTemp& e) { return e.Value == value; });
        // Adoption wins over any end-of-expression release already registered for the same
        // value (e.g. `u = cond ? new R() : nullptr` after a comparison registered it).
        UnregisterOwnedPtrTemp(value);
    }

void LLVMBackend::UnregisterOwnedPtrTemp(llvm::Value* value)
{
        if (value == nullptr) return;
        std::erase_if(pendingOwnedPtrTemps,
            [&](const PendingOwnedPtrTemp& p) { return p.Value == value; });
    }

bool LLVMBackend::IsInsertBlockLive() const
{
        auto* b = builder->GetInsertBlock();
        return b != nullptr && b->getTerminator() == nullptr;
    }

bool LLVMBackend::OwnedTempDominatesHere(llvm::BasicBlock* bb, llvm::BasicBlock* curBlock,
                                std::optional<llvm::DominatorTree>& dt) const
{
        if (bb == nullptr || curBlock == nullptr) return false;
        if (bb == curBlock) return true;
        if (bb->getParent() != curBlock->getParent()) return false;
        if (!dt) dt.emplace(*curBlock->getParent());
        return dt->dominates(bb, curBlock);
    }

void LLVMBackend::EmitOwnedStringTempFree(llvm::Value* value)
{
        auto* strTy = llvm::StructType::getTypeByName(*context, "string");
        if (value == nullptr || strTy == nullptr || value->getType() != strTy) return;
        EnsureStringDtorRegistered();
        auto it = dataStructures.find("string");
        if (it == dataStructures.end() || it->second.Destructor == nullptr) return;
        // The destructor takes a string*; spill the SSA value to an entry-block
        // alloca (never a loop body - see AllocaAtEntry) and free through it.
        auto* tmp = AllocaAtEntry(strTy, nullptr, "concat.tmp");
        builder->CreateStore(value, tmp);
        builder->CreateCall(it->second.Destructor->getFunctionType(),
                            it->second.Destructor, { tmp });
    }

void LLVMBackend::FlushOwnedStringTemps()
{
        if (pendingOwnedStringTemps.empty()) return;

        auto* curBlock = builder->GetInsertBlock();
        if (IsInsertBlockLive())
        {
            std::optional<llvm::DominatorTree> domTree;
            for (auto& [value, bb] : pendingOwnedStringTemps)
            {
                if (value == nullptr || !OwnedTempDominatesHere(bb, curBlock, domTree)) continue; // dominance safety
                EmitOwnedStringTempFree(value);
            }
        }
        pendingOwnedStringTemps.clear();
    }

void LLVMBackend::RegisterOwnedClosureTemp(llvm::Value* value)
{
        if (value == nullptr) return;
        pendingOwnedClosureTemps.emplace_back(value, builder->GetInsertBlock());
    }

void LLVMBackend::UnregisterOwnedClosureTemp(llvm::Value* value)
{
        if (value == nullptr) return;
        std::erase_if(pendingOwnedClosureTemps,
            [&](const std::pair<llvm::Value*, llvm::BasicBlock*>& e) { return e.first == value; });
    }

bool LLVMBackend::IsOwnedClosureTemp(llvm::Value* value) const
{
        for (const auto& e : pendingOwnedClosureTemps)
            if (e.first == value) return true;
        return false;
    }

void LLVMBackend::EmitOwnedClosureTempFree(llvm::Value* value)
{
        auto* closureTy = GetClosureFatPtrType();
        if (value == nullptr || closureTy == nullptr || value->getType() != closureTy) return;
        auto* dtor = GetOrCreateFullDestructor("__closure_fat_ptr");
        if (dtor == nullptr) return;
        // The dtor takes a __closure_fat_ptr*; spill the SSA value to an entry-block
        // alloca (never a loop body - see AllocaAtEntry) and free through it.
        auto* tmp = AllocaAtEntry(closureTy, nullptr, "closure.tmp");
        builder->CreateStore(value, tmp);
        builder->CreateCall(dtor->getFunctionType(), dtor, { tmp });
    }

void LLVMBackend::FlushOwnedClosureTemps()
{
        if (pendingOwnedClosureTemps.empty()) return;

        auto* curBlock = builder->GetInsertBlock();
        if (IsInsertBlockLive())
        {
            {
                std::optional<llvm::DominatorTree> domTree;
                for (auto& [value, bb] : pendingOwnedClosureTemps)
                {
                    if (value == nullptr || !OwnedTempDominatesHere(bb, curBlock, domTree)) continue; // dominance safety
                    EmitOwnedClosureTempFree(value);
                }
            }
        }
        pendingOwnedClosureTemps.clear();
    }

void LLVMBackend::RegisterOwnedStructTemp(llvm::Value* alloca, const std::string& typeName)
{
        if (alloca == nullptr || typeName.empty()) return;
        pendingOwnedStructTemps.push_back({ alloca, typeName, builder->GetInsertBlock() });
    }

void LLVMBackend::FlushOwnedStructTemps()
{
        if (pendingOwnedStructTemps.empty()) return;

        auto temps = std::move(pendingOwnedStructTemps);
        pendingOwnedStructTemps.clear();
        // ONE pass in ledger order. A guarded free opens blocks, so the insert block is re-read
        // per temp and the dominator tree is dropped after one - batching the guarded temps last
        // instead would silently reverse two temps' destruction order within a statement.
        std::optional<llvm::DominatorTree> domTree;
        for (auto& t : temps)
        {
            if (t.Alloca == nullptr || !IsInsertBlockLive()) continue;
            if (!OwnedTempDominatesHere(t.Block, builder->GetInsertBlock(), domTree)) continue; // dominance safety
            EmitOwnedStructTempFree(t);
            if (t.LiveFlag != nullptr) domTree.reset();   // new blocks invalidate the cached tree
        }
    }

void LLVMBackend::EmitOwnedPtrTempFree(llvm::Value* ptrVal, const std::string& typeName, uint64_t allocAlign)
{
        auto* ptrTy = llvm::dyn_cast<llvm::PointerType>(ptrVal->getType());
        if (ptrTy == nullptr) return;
        auto* isNull = builder->CreateICmpEQ(ptrVal, llvm::ConstantPointerNull::get(ptrTy));
        auto* cleanupBB = llvm::BasicBlock::Create(*context, "tmpptr.cleanup", builder->GetInsertBlock()->getParent());
        auto* afterBB   = llvm::BasicBlock::Create(*context, "tmpptr.after",   builder->GetInsertBlock()->getParent());
        builder->CreateCondBr(isNull, afterBB, cleanupBB);

        builder->SetInsertPoint(cleanupBB);
        if (auto* dtor = GetFullDestructorForDelete(typeName))
            builder->CreateCall(dtor->getFunctionType(), dtor, { ptrVal });

        // Over-aligned blocks come from the aligned allocator and must be freed through
        // __delete_aligned - the same rule as EmitOwningPtrCleanup and the `delete` site.
        auto* voidPtr = builder->CreateBitCast(ptrVal, builder->getInt8Ty()->getPointerTo());
        uint64_t effAlign = allocAlign;
        if (!typeName.empty())
        {
            TypeAndValue tv{ .TypeName = typeName };
            llvm::Type* t = GetType(tv);
            if (t != nullptr && t->isSized())
                effAlign = std::max(effAlign, GetEffectiveAlignmentForType(typeName, t));
        }
        llvm::Function* del = effAlign > kDefaultNewAlign
            ? GetFunction("__delete_aligned") : GetFunction("operator delete");
        if (del)
            builder->CreateCall(del->getFunctionType(), del, { voidPtr });

        builder->CreateBr(afterBB);
        builder->SetInsertPoint(afterBB);
    }

void LLVMBackend::FlushOwnedPtrTemps()
{
        if (pendingOwnedPtrTemps.empty()) return;

        auto temps = std::move(pendingOwnedPtrTemps);
        pendingOwnedPtrTemps.clear();
        for (auto& t : temps)
        {
            if (t.Value == nullptr || !IsInsertBlockLive()) continue;
            std::optional<llvm::DominatorTree> domTree;
            if (!OwnedTempDominatesHere(t.Block, builder->GetInsertBlock(), domTree)) continue; // dominance safety
            EmitOwnedPtrTempFree(t.Value, t.TypeName, t.AllocAlign);
        }
    }

void LLVMBackend::RegisterBorrowedOwningStructTemp(const NamedVariable& arg)
{
        const std::string& typeName = arg.TypeAndValue.TypeName;
        if (arg.Primary == nullptr || arg.Storage != nullptr || arg.BaseType == nullptr) return;
        if (!llvm::isa<llvm::CallInst>(arg.Primary)) return;   // only a produced temp, not a named local
        if (!arg.BaseType->isStructTy() || arg.Primary->getType() != arg.BaseType) return;
        if (arg.TypeAndValue.Pointer || arg.TypeAndValue.IsAlias || arg.FromOwningTempField) return;
        if (typeName.empty() || typeName == "string" || typeName == "__closure_fat_ptr") return;
        if (!IsOwningValueType(typeName)) return;

        auto* tempAlloca = AllocaAtEntry(arg.BaseType, nullptr, "argtemp");
        builder->CreateStore(arg.Primary, tempAlloca);
        RegisterOwnedStructTemp(tempAlloca, typeName);
    }

LLVMBackend::OwnedTempMark LLVMBackend::MarkOwnedTemps() const
{
        return { pendingOwnedStringTemps.size(), pendingOwnedClosureTemps.size(),
                 pendingOwnedStructTemps.size(), pendingOwnedPtrTemps.size() };
    }

void LLVMBackend::EmitOwnedStructTempFree(const PendingOwnedStructTemp& temp)
{
        auto* dtor = GetOrCreateFullDestructor(temp.TypeName);
        if (dtor == nullptr || temp.Alloca == nullptr) return;
        if (temp.LiveFlag == nullptr)
        {
            builder->CreateCall(dtor->getFunctionType(), dtor, { temp.Alloca });
            return;
        }
        // A hoisted temp is destructed here on EVERY path, so the flag is what says the arm
        // actually ran: a user destructor body dereferences its fields without a null check.
        auto* fn      = builder->GetInsertBlock()->getParent();
        auto* liveBB  = llvm::BasicBlock::Create(*context, "owntemp.live",  fn);
        auto* afterBB = llvm::BasicBlock::Create(*context, "owntemp.after", fn);
        auto* flag    = builder->CreateLoad(builder->getInt1Ty(), temp.LiveFlag);
        builder->CreateCondBr(flag, liveBB, afterBB);
        builder->SetInsertPoint(liveBB);
        builder->CreateCall(dtor->getFunctionType(), dtor, { temp.Alloca });
        builder->CreateBr(afterBB);
        builder->SetInsertPoint(afterBB);
    }

bool LLVMBackend::HoistOwnedStructTempTo(PendingOwnedStructTemp& temp, llvm::BasicBlock* hoistTo)
{
        if (hoistTo == nullptr || hoistTo->getTerminator() == nullptr) return false;
        if (!IsInsertBlockLive()) return false;   // no live point in the arm to set the flag from
        auto* alloca = llvm::dyn_cast_or_null<llvm::AllocaInst>(temp.Alloca);
        if (alloca == nullptr || alloca->getFunction() != hoistTo->getParent()) return false;
        // Only an ENTRY-block alloca provably dominates the join; anything else (a loop-body
        // alloca) would name storage the resume block cannot reach.
        if (alloca->getParent() != &alloca->getFunction()->getEntryBlock()) return false;
        if (temp.Block == hoistTo) return true;

        auto* saveBlock = builder->GetInsertBlock();
        auto savePoint  = builder->GetInsertPoint();
        // First hoist only - re-hoisting to an ENCLOSING branch must not re-arm the flag, since
        // that arm can run without this one (a '??' fallback nested in a taken '?:' arm).
        if (temp.LiveFlag == nullptr)
        {
            temp.LiveFlag = AllocaAtEntry(builder->getInt1Ty(), nullptr, "owntemp.livef");
            builder->CreateStore(builder->getInt1(true), temp.LiveFlag);
        }
        builder->SetInsertPoint(hoistTo->getTerminator());
        builder->CreateStore(llvm::Constant::getNullValue(alloca->getAllocatedType()), alloca);
        builder->CreateStore(builder->getInt1(false), temp.LiveFlag);
        if (saveBlock != nullptr) builder->SetInsertPoint(saveBlock, savePoint);
        temp.Block = hoistTo;
        return true;
    }

void LLVMBackend::FlushOwnedTempsSince(const OwnedTempMark& mark, llvm::Value* keep,
                                       llvm::BasicBlock* hoistTo)
{
        // Struct temps that can outlive the arm are pulled out of the range FIRST, then re-added
        // below, so neither the free loop nor the trim can retire them here.
        std::vector<PendingOwnedStructTemp> hoisted;
        if (hoistTo != nullptr)
        {
            for (size_t i = mark.Structs; i < pendingOwnedStructTemps.size(); )
            {
                auto& t = pendingOwnedStructTemps[i];
                if (t.Alloca != nullptr && t.Alloca != keep && HoistOwnedStructTempTo(t, hoistTo))
                {
                    hoisted.push_back(t);
                    pendingOwnedStructTemps.erase(pendingOwnedStructTemps.begin() + i);
                    continue;
                }
                ++i;
            }
        }

        // Value-based frees first: they emit plain calls into the current block, while the
        // pointer free below opens new blocks and moves the insert point out from under them.
        {
            std::optional<llvm::DominatorTree> domTree;
            auto* curBlock = builder->GetInsertBlock();
            bool live = IsInsertBlockLive();
            for (size_t i = mark.Strings; live && i < pendingOwnedStringTemps.size(); ++i)
            {
                auto& [value, bb] = pendingOwnedStringTemps[i];
                if (value == nullptr || value == keep) continue;
                if (!OwnedTempDominatesHere(bb, curBlock, domTree)) continue;
                EmitOwnedStringTempFree(value);
            }
            for (size_t i = mark.Closures; live && i < pendingOwnedClosureTemps.size(); ++i)
            {
                auto& [value, bb] = pendingOwnedClosureTemps[i];
                if (value == nullptr || value == keep) continue;
                if (!OwnedTempDominatesHere(bb, curBlock, domTree)) continue;
                EmitOwnedClosureTempFree(value);
            }
            // Ledger order, guarded and unguarded alike. A guarded free opens blocks, so the
            // insert block is re-read per temp and the cached tree dropped after one; batching
            // the guarded ones last instead would reverse two temps' destruction order.
            for (size_t i = mark.Structs; live && i < pendingOwnedStructTemps.size(); ++i)
            {
                auto& t = pendingOwnedStructTemps[i];
                if (t.Alloca == nullptr || t.Alloca == keep) continue;
                if (!IsInsertBlockLive()) break;
                if (!OwnedTempDominatesHere(t.Block, builder->GetInsertBlock(), domTree)) continue;
                EmitOwnedStructTempFree(t);
                if (t.LiveFlag != nullptr) domTree.reset();
            }
        }
        auto pairValue   = [](const std::pair<llvm::Value*, llvm::BasicBlock*>& e) { return e.first; };
        auto structValue = [](const PendingOwnedStructTemp& e) { return e.Alloca; };
        auto ptrValue    = [](const PendingOwnedPtrTemp& e) { return e.Value; };

        // Collect the pointer temps before trimming: each free opens blocks, so the insert block
        // and dominator tree are recomputed per temp - exactly as FlushOwnedPtrTemps does.
        std::vector<PendingOwnedPtrTemp> ptrTemps;
        for (size_t i = mark.Ptrs; i < pendingOwnedPtrTemps.size(); ++i)
            if (pendingOwnedPtrTemps[i].Value != keep) ptrTemps.push_back(pendingOwnedPtrTemps[i]);

        TrimOwnedTempsSince(pendingOwnedStringTemps,  mark.Strings,  keep, pairValue);
        TrimOwnedTempsSince(pendingOwnedClosureTemps, mark.Closures, keep, pairValue);
        TrimOwnedTempsSince(pendingOwnedStructTemps,  mark.Structs,  keep, structValue);
        TrimOwnedTempsSince(pendingOwnedPtrTemps,     mark.Ptrs,     keep, ptrValue);

        for (auto& t : ptrTemps)
        {
            if (t.Value == nullptr || !IsInsertBlockLive()) continue;
            std::optional<llvm::DominatorTree> domTree;
            if (!OwnedTempDominatesHere(t.Block, builder->GetInsertBlock(), domTree)) continue;
            EmitOwnedPtrTempFree(t.Value, t.TypeName, t.AllocAlign);
        }

        // Back on the ledger, now keyed to a dominating block: the end-of-statement flush frees
        // them, and an enclosing join's own hoist can re-key them further out.
        for (auto& h : hoisted) pendingOwnedStructTemps.push_back(h);
    }

void LLVMBackend::DiscardOwnedTempsSince(const OwnedTempMark& mark)
{
        auto pairValue   = [](const std::pair<llvm::Value*, llvm::BasicBlock*>& e) { return e.first; };
        auto structValue = [](const PendingOwnedStructTemp& e) { return e.Alloca; };
        auto ptrValue    = [](const PendingOwnedPtrTemp& e) { return e.Value; };
        TrimOwnedTempsSince(pendingOwnedStringTemps,  mark.Strings,  nullptr, pairValue);
        TrimOwnedTempsSince(pendingOwnedClosureTemps, mark.Closures, nullptr, pairValue);
        TrimOwnedTempsSince(pendingOwnedStructTemps,  mark.Structs,  nullptr, structValue);
        TrimOwnedTempsSince(pendingOwnedPtrTemps,     mark.Ptrs,     nullptr, ptrValue);
        // Pending vectors can retain obligations from before the aborted region, so they are
        // trimmed by mark. Detection-only ledgers describe SSA values in the discarded region;
        // clear them wholesale so a later expression can never consult an aborted fact.
        ownedReturnTemps_.clear();
        ownedReturnReleaseTemps_.clear();
        ownedNewTemps_.clear();
        rawArrayResults_.clear();
        valueElementTypeNames_.clear();
        fatInterfaceValueTypeNames_.clear();
        movedOutPtrValues_.clear();
        movedBorrowedPtrValues_.clear();
        movedBorrowedThroughFieldValues_.clear();
}

void LLVMBackend::FlushOwnedTemps()
{
        FlushOwnedStringTemps();
        FlushOwnedClosureTemps();
        FlushOwnedStructTemps();
        // Last: freeing a pointer temp opens blocks, which would move the insert point out from
        // under the value-based flushes above.
        FlushOwnedPtrTemps();
        // Detection-only ledgers: end of a full expression retires its owning results.
        ownedReturnTemps_.clear();
        ownedReturnReleaseTemps_.clear();
        ownedNewTemps_.clear();
        rawArrayResults_.clear();
        valueElementTypeNames_.clear();
        fatInterfaceValueTypeNames_.clear();
        // A named function is one shared llvm::Function constant, so a cast's launder must not
        // outlive its statement (see codeValueDataCasts_). codeValues_ deliberately survives.
        codeValueDataCasts_.clear();
        // Reset the ambient occurrence for the next statement (see currentCastOccurrence_'s
        // comment) - a call-argument's bumped id must never survive past its own statement.
        currentCastOccurrence_ = 0;
        // Same boundary destructs the owning temp, so its unique-field reads retire with it.
        owningTempUniqueFields_.clear();
        launderedTempUniqueFields_.clear();
        pendingLaunderTempUniqueFields_.clear();
        dataValueCodeCasts_.clear();
        movedOutPtrValues_.clear();
        movedBorrowedPtrValues_.clear();
        movedBorrowedThroughFieldValues_.clear();
        nonOwningStructJoins_.clear();
        uniqueFieldReadValues_.clear();
        uniqueFieldReadJoins_.clear();
    }

void LLVMBackend::DropValue(const NamedVariable& namedVar)
{
        // A `static` local's storage outlives the scope (and every later call), so scope exit must
        // not destruct it. Policy: a static local is destructed NEVER - no atexit machinery.
        if (namedVar.IsStaticLocal) return;
        if (namedVar.IsRangeForBorrow) return;
        // A `unique` interface local owns a heap-boxed object: free it via the vtable dtor slot
        // + operator delete (mirrors the owning-pointer path; data field nulled so a prior delete no-ops).
        if (IsOwningUniqueArray(namedVar))
        {
            EmitOwningUniqueArrayCleanup(namedVar);
            return;
        }
        if (IsOwningInterfaceValue(namedVar))
        {
            EmitOwningInterfaceCleanup(namedVar);
            return;
        }
        if (namedVar.TypeAndValue.Pointer && namedVar.IsOwning)
        {
            if (namedVar.RefCountStorage == nullptr)
            {
                EmitOwningPtrCleanup(namedVar);
            }
            else
            {
                auto* cur = builder->CreateLoad(builder->getInt32Ty(), namedVar.RefCountStorage);
                auto* dec = builder->CreateSub(cur, builder->getInt32(1), "refdec");
                builder->CreateStore(dec, namedVar.RefCountStorage);
                EmitConditionalOwningPtrCleanup(namedVar, dec);
            }
            return;
        }
        if (namedVar.TypeAndValue.Pointer) return;
        auto it = dataStructures.find(namedVar.TypeAndValue.TypeName);
        if (it != dataStructures.end())
        {
            // String dtor is emitted unconditionally - the runtime OWNED bit (_len high bit) decides.
            // Legacy IsOwningString skipped genuinely-owned strings the compiler couldn't prove owned, leaking their buffer.
            if (namedVar.TypeAndValue.TypeName == "string")
            {
                if (namedVar.BorrowsOwnedString || namedVar.IsAliasBorrow) return;
                EnsureStringDtorRegistered();
                if (it->second.Destructor == nullptr) return;
                EmitFullDestructorOverStorage(*builder, namedVar.Storage, namedVar.BaseType,
                                              it->second.Destructor);
                return;
            }
            // Non-string struct local: run the full destructor (user dtor + members).
            // Skip an `alias`-bound local - it borrows storage it does not own (double-free).
            if (namedVar.IsAliasBorrow) return;
            // Skip the struct value being moved out via `return` - the caller now owns it.
            if (namedVar.Storage == returnedStructDtorSkipAlloca) return;
            // A fixed-array local (`T[N] a;`) owns every element - destruct all N.
            if (auto* fn = GetOrCreateFullDestructor(namedVar.TypeAndValue.TypeName))
                EmitFullDestructorOverStorage(*builder, namedVar.Storage, namedVar.BaseType, fn);
        }
    }

bool LLVMBackend::OwnsDroppableResource(const NamedVariable& namedVar) const
{
        if (namedVar.IsStaticLocal) return false;   // never dropped; see DropValue
        if (namedVar.IsRangeForBorrow) return false;
        if (IsOwningUniqueArray(namedVar)) return true;
        if (IsOwningInterfaceValue(namedVar)) return true;
        if (namedVar.TypeAndValue.Pointer && namedVar.IsOwning) return true;
        if (namedVar.TypeAndValue.Pointer) return false;
        auto it = dataStructures.find(namedVar.TypeAndValue.TypeName);
        if (it == dataStructures.end()) return false;
        if (namedVar.TypeAndValue.TypeName == "string")
            return !(namedVar.BorrowsOwnedString || namedVar.IsAliasBorrow);
        if (namedVar.IsAliasBorrow) return false;
        if (namedVar.Storage == returnedStructDtorSkipAlloca) return false;
        return true;
    }

void LLVMBackend::EmitDestructorsForScope(const StackState& frame)
{
        if (!IsInsertBlockLive())
            return;

        // Cleanup destructors have no user location. Pin a synthetic location to the function
        // line to avoid the -g verifier rejecting untagged inlinable calls after a branch/return.
        if (currentSubprogram && !builder->getCurrentDebugLocation())
        {
            builder->SetCurrentDebugLocation(llvm::DILocation::get(
                *context, currentSubprogram->getLine(), 0, currentSubprogram));
        }

        for (const auto& [varName, namedVar] : frame.namedVariable)
        {
            DropValue(namedVar);
        }

        // Clean up owning function parameters (move params)
        for (const auto& [varName, namedVar] : frame.functionArgument)
        {
            // Any interface fat-ptr param is handled here, never falling through to EmitOwningPtrCleanup
            // (which would bitcast the {i8*,i8*} slot - invalid IR); only a `unique` (owning) one is freed.
            if (namedVar.TypeAndValue.IsFatInterfaceValue())
            {
                if (IsOwningInterfaceValue(namedVar))
                    EmitOwningInterfaceCleanup(namedVar);
                continue;
            }
            if (namedVar.IsOwning && namedVar.Storage != nullptr)
                EmitOwningPtrCleanup(namedVar);

            // Clean up move string parameters (move string param - non-pointer ownership)
            if (namedVar.IsOwningString && namedVar.Storage != nullptr)
            {
                EnsureStringDtorRegistered();
                auto it = dataStructures.find("string");
                if (it != dataStructures.end() && it->second.Destructor != nullptr)
                    builder->CreateCall(it->second.Destructor->getFunctionType(), it->second.Destructor, { namedVar.Storage });
            }

            // Clean up move struct parameters
            if (namedVar.IsOwningStruct && namedVar.Storage != nullptr)
            {
                if (auto* dtor = GetOrCreateFullDestructor(namedVar.TypeAndValue.TypeName))
                    builder->CreateCall(dtor->getFunctionType(), dtor, { namedVar.Storage });
            }
        }

        // Release locks held by this scope (lock statement). Acquired in argument
        // order, so release in reverse to respect nested lock ordering.
        for (auto it = frame.lockCleanups.rbegin(); it != frame.lockCleanups.rend(); ++it)
        {
            builder->CreateCall(it->UnlockFn->getFunctionType(), it->UnlockFn, { it->MutexPtr });
        }
    }

int LLVMBackend::MintAliasScope()
{
        llvm::MDBuilder mdb(*context);
        if (aliasDomain_ == nullptr)
            aliasDomain_ = mdb.createAnonymousAliasScopeDomain("cflat.view");
        auto* scope = mdb.createAnonymousAliasScope(aliasDomain_, "cflat.view.scope");
        aliasScopes_.push_back(scope);
        return (int)aliasScopes_.size() - 1;
    }

void LLVMBackend::AttachViewNoalias(llvm::Instruction* memInst, int scopeId)
{
        if (memInst == nullptr || scopeId < 0 || scopeId >= (int)aliasScopes_.size())
            return;
        std::vector<llvm::Metadata*> others;
        for (int i = 0; i < (int)aliasScopes_.size(); ++i)
            if (i != scopeId)
                others.push_back(aliasScopes_[i]);
        memInst->setMetadata(llvm::LLVMContext::MD_alias_scope,
            llvm::MDNode::get(*context, { aliasScopes_[scopeId] }));
        if (!others.empty())
            memInst->setMetadata(llvm::LLVMContext::MD_noalias, llvm::MDNode::get(*context, others));
    }

int LLVMBackend::GetOrMintViewScope(const std::string& originKey)
{
        if (originKey.empty())
            return -1;
        auto it = viewScopeByOrigin_.find(originKey);
        if (it != viewScopeByOrigin_.end())
            return it->second;
        int id = MintAliasScope();
        viewScopeByOrigin_.emplace(originKey, id);
        return id;
    }

LLVMBackend::~LLVMBackend()
{
        CompilerManager::Instance().Unregister(this);

        builder.release();
        module.release();

        // context is last to be released.
        context.release();
    }
