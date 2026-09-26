#pragma warning(push)
#pragma warning(disable: 4244 4267)
#include <llvm/IR/CFG.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/CFG.h>
#include <unordered_set>
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

void LLVMBackend::SetTargetLongWidth(bool targetWindows, int platformBits,
                                     bool targetArm64, bool targetMacOS)
{
        longBits_ = (targetWindows || platformBits == 32) ? 32 : 64;
        wcharBits_ = targetWindows ? 16 : 32;
        // Windows wchar_t is unsigned 16-bit; Linux aarch64 wchar_t is unsigned 32-bit;
        // Linux x86-64 and macOS arm64 use signed 32-bit wchar_t.
        wcharSigned_ = !targetWindows && (!targetArm64 || targetMacOS);
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

void LLVMBackend::RegisterAliasValue(llvm::Value* value)
{
        if (value == nullptr) return;
        if (std::find(aliasValues_.begin(), aliasValues_.end(), value) == aliasValues_.end())
            aliasValues_.push_back(value);
    }

bool LLVMBackend::IsAliasValue(const llvm::Value* value) const
{
        return value != nullptr
            && std::find(aliasValues_.begin(), aliasValues_.end(), value) != aliasValues_.end();
    }

void LLVMBackend::PropagateAliasValue(llvm::Value* trueValue, llvm::Value* falseValue,
                                      llvm::Value* joined)
{
        if (joined != nullptr && (IsAliasValue(trueValue) || IsAliasValue(falseValue)))
            RegisterAliasValue(joined);
    }

void LLVMBackend::RegisterTempFieldValue(llvm::Value* value)
{
        if (value == nullptr) return;
        if (std::find(tempFieldValues_.begin(), tempFieldValues_.end(), value) == tempFieldValues_.end())
            tempFieldValues_.push_back(value);
    }

bool LLVMBackend::IsTempFieldValue(const llvm::Value* value) const
{
        return value != nullptr
            && std::find(tempFieldValues_.begin(), tempFieldValues_.end(), value) != tempFieldValues_.end();
    }

void LLVMBackend::PropagateTempFieldValue(llvm::Value* trueValue, llvm::Value* falseValue,
                                          llvm::Value* joined)
{
        if (joined != nullptr && (IsTempFieldValue(trueValue) || IsTempFieldValue(falseValue)))
            RegisterTempFieldValue(joined);
}

void LLVMBackend::RegisterBondedValue(llvm::Value* value, const std::vector<std::string>& sources)
{
        if (value == nullptr || sources.empty()) return;
        auto& recorded = bondedValues_[value];
        for (const auto& source : sources)
            if (std::find(recorded.begin(), recorded.end(), source) == recorded.end())
                recorded.push_back(source);
}

const std::vector<std::string>* LLVMBackend::FindBondedValue(const llvm::Value* value) const
{
        if (value == nullptr) return nullptr;
        auto it = bondedValues_.find(value);
        return it == bondedValues_.end() ? nullptr : &it->second;
}

void LLVMBackend::PropagateBondedValue(llvm::Value* trueValue, llvm::Value* falseValue,
                                       llvm::Value* joined)
{
        if (joined == nullptr) return;
        if (const auto* sources = FindBondedValue(trueValue)) RegisterBondedValue(joined, *sources);
        if (const auto* sources = FindBondedValue(falseValue)) RegisterBondedValue(joined, *sources);
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

bool LLVMBackend::ReportArrayViewInstantiationFailure(const std::string& formedType) const
{
        const auto& origin = gts.activeInstantiationOrigin;
        if (!origin.valid) return false;
        // Only the substituted argument itself ('int[]') or a pointer built from it ('int[]*')
        // belongs to the argument; anything else is an unrelated missing type in the body.
        const std::string* view = nullptr;
        for (const auto& arg : origin.viewArgs)
            if (formedType == arg || formedType == arg + "*") view = &arg;
        if (view == nullptr) return false;

        ReportingFileScope scope(const_cast<LLVMBackend*>(this), origin.file, origin.line, origin.column);
        LogErrorMessage("'{}' cannot be a type argument of '{}': instantiating the body "
                        "forms '{}', which is not a valid type",
                        { *view, origin.templateName, formedType });
        return true;
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

bool LLVMBackend::HasTypeAnnotation(const std::string& name, const std::string& annName) const
{
        return FindTypeAnnotation(name, annName) != nullptr;
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
            auto argIt = frame.functionArgument.find(varName);
            if (argIt != frame.functionArgument.end())
            {
                argIt->second.AllocatedByRawNewArray = value;
                StoreRawArrayLength(argIt->second, rawArrayLength);
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

bool LLVMBackend::HasRawNewArrayProvenance(const NamedVariable& namedVar) const
{
        return namedVar.AllocatedByRawNewArray
            && (namedVar.RawArrayLength != nullptr || namedVar.RawArrayLengthStorage != nullptr)
            && !namedVar.TypeAndValue.IsMove;
    }

void LLVMBackend::SetVariableOwning(const std::string& varName, bool value)
{
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            auto it = frame.namedVariable.find(varName);
            if (it != frame.namedVariable.end())
            {
                it->second.IsOwning = value;
                // Keep an active ownership flag in step at the program point of the ownership event.
                auto* flag = it->second.OwnFlag;
                if (it->second.OwnFlagActive && flag != nullptr && IsInsertBlockLive()
                    && flag->getFunction() == builder->GetInsertBlock()->getParent())
                    builder->CreateStore(builder->getInt1(value), flag);
                return;
            }
        }
    }

void LLVMBackend::ActivateOwnFlag(const std::string& varName)
{
        if (varName.empty() || stackNamedVariable.empty()) return;
        auto& frame = stackNamedVariable.back().namedVariable;
        auto it = frame.find(varName);
        if (it == frame.end() || it->second.OwnFlagInit == nullptr) return;
        it->second.OwnFlagInit->setOperand(0, builder->getInt1(it->second.IsOwning));
        it->second.OwnFlagInit = nullptr;
        it->second.OwnFlagActive = true;
        // `T* b = move a;` adopted a's block: the adoption answers to a's aliases too.
        auto* slot = llvm::dyn_cast_or_null<llvm::AllocaInst>(it->second.Storage);
        if (!it->second.IsOwning || slot == nullptr || !IsInsertBlockLive()) return;
        llvm::StoreInst* last = nullptr;
        for (llvm::User* u : slot->users())
            if (auto* st = llvm::dyn_cast<llvm::StoreInst>(u))
                if (st->getPointerOperand() == slot && st->getParent() == builder->GetInsertBlock()
                    && (last == nullptr || last->comesBefore(st)))
                    last = st;
        if (last != nullptr) GateOwnFlagAdoption(it->second, last->getValueOperand());
    }

void LLVMBackend::GateOwnFlagAdoption(const NamedVariable& nv, llvm::Value* stored)
{
        if (!nv.OwnFlagActive || nv.OwnFlag == nullptr || stored == nullptr || !IsInsertBlockLive()
            || nv.OwnFlag->getFunction() != builder->GetInsertBlock()->getParent()) return;
        auto* dest = llvm::dyn_cast_or_null<llvm::AllocaInst>(nv.Storage);
        auto* ld = llvm::dyn_cast<llvm::LoadInst>(stored->stripPointerCasts());
        if (dest == nullptr || ld == nullptr || !ld->getType()->isPointerTy()) return;
        auto* src = llvm::dyn_cast<llvm::AllocaInst>(ld->getPointerOperand());
        if (src == nullptr || src == dest || src->getFunction() != dest->getFunction()) return;
        // Stored false at entry (adopter does not own: leak, never free) until the post-walk
        // scan proves no alias of the source slot reaches the transferring load.
        auto* gateSlot = AllocaAtEntry(builder->getInt1Ty(), nullptr,
                                       nv.TypeAndValue.VariableName + ".adopt");
        llvm::StoreInst* init = nullptr;
        {
            llvm::IRBuilderBase::InsertPointGuard guard(*builder);
            builder->SetInsertPoint(gateSlot->getParent(), std::next(gateSlot->getIterator()));
            init = builder->CreateStore(builder->getInt1(false), gateSlot);
        }
        NoteOwnSlotLeavingLoad(ld);
        ownAdoptGates_.push_back({ src, dest, init, ld });
        builder->CreateStore(builder->CreateLoad(builder->getInt1Ty(), gateSlot, "own.adopt"),
                             nv.OwnFlag);
    }

LLVMBackend::NamedVariable* LLVMBackend::FindStackVariableByStorage(const llvm::Value* storage)
{
        if (storage == nullptr) return nullptr;
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
            for (auto& [name, nv] : frame.namedVariable)
                if (nv.Storage == storage) return &nv;
        return nullptr;
    }

void LLVMBackend::EmitOwnedPtrRelease(const NamedVariable& namedVar, llvm::Value* replacement,
                                      llvm::Value* rhsOwnerSlot, bool rhsBorrowedParam)
{
        if (!namedVar.OwnFlagActive || namedVar.OwnFlag == nullptr || replacement == nullptr
            || !replacement->getType()->isPointerTy() || !IsInsertBlockLive()
            || namedVar.OwnFlag->getFunction() != builder->GetInsertBlock()->getParent()) return;
        auto* slot = llvm::dyn_cast<llvm::AllocaInst>(namedVar.Storage);
        if (slot == nullptr) return;
        // An opaque / void pointee was never allocated by `new`, so the local can never own it.
        if (!namedVar.TypeAndValue.ElemPointer)
        {
            TypeAndValue pointee{ .TypeName = namedVar.TypeAndValue.TypeName };
            llvm::Type* t = namedVar.TypeAndValue.TypeName.empty() ? nullptr : GetType(pointee);
            if (t == nullptr || !t->isSized()) return;
        }
        // Stored false at entry (no release) until the post-walk alias scan proves it safe.
        auto* gateSlot = AllocaAtEntry(builder->getInt1Ty(), nullptr,
                                       namedVar.TypeAndValue.VariableName + ".release");
        llvm::StoreInst* init = nullptr;
        {
            llvm::IRBuilderBase::InsertPointGuard guard(*builder);   // also keeps the !dbg location
            builder->SetInsertPoint(gateSlot->getParent(), std::next(gateSlot->getIterator()));
            init = builder->CreateStore(builder->getInt1(false), gateSlot);
        }
        // Re-storing the block the local already holds keeps its ownership; any other value is
        // unowned until SetVariableOwning(true) adopts it later in this statement.
        auto* old = builder->CreateLoad(namedVar.BaseType, namedVar.Storage, "own.old");
        ownReleaseGates_.push_back({ slot, init, old });
        auto* same = builder->CreateICmpEQ(old, replacement, "own.same");
        auto* owned = builder->CreateLoad(builder->getInt1Ty(), namedVar.OwnFlag, "own.owned");
        auto* gate = builder->CreateLoad(builder->getInt1Ty(), gateSlot, "own.gate");
        // A field escape already cleared the flag (ClearOwnFlag), so an escaped block is never
        // released here; the replacement block starts a fresh field-escape count of one holder.
        auto* preBlock = builder->GetInsertBlock();
        EmitOwningPtrCleanup(namedVar, replacement, gate);
        RetargetStraightLineBlockFacts(preBlock, builder->GetInsertBlock());
        // An aliased old block is neither released nor disowned (the pre-existing scope-exit
        // behaviour), except that a provable stack address or null is never owned.
        const bool rhsNotHeap = llvm::isa<llvm::ConstantPointerNull>(replacement)
            || IsProvableNonHeapAddress(replacement) || rhsBorrowedParam;
        llvm::Value* aliasedFlag = rhsNotHeap ? builder->getInt1(false) : static_cast<llvm::Value*>(owned);
        llvm::Value* next = builder->CreateSelect(gate, builder->getInt1(false), aliasedFlag);
        // A borrow of an owning local: disown while that owner still holds (and owns) the block;
        // otherwise the round-1 rule (never adopt a block that may be moved or still aliased).
        const NamedVariable* owner = rhsOwnerSlot != nullptr && !rhsNotHeap
            ? FindStackVariableByStorage(rhsOwnerSlot) : nullptr;
        if (owner != nullptr && owner->Storage != namedVar.Storage && owner->BaseType != nullptr
            && owner->BaseType->isPointerTy()
            && llvm::isa<llvm::AllocaInst>(owner->Storage)
            && llvm::cast<llvm::AllocaInst>(owner->Storage)->getFunction()
                == builder->GetInsertBlock()->getParent())
        {
            auto* held = builder->CreateICmpEQ(
                builder->CreateLoad(owner->BaseType, owner->Storage, "own.src"), replacement);
            if (owner->OwnFlagActive && owner->OwnFlag != nullptr)
                held = builder->CreateAnd(
                    held, builder->CreateLoad(builder->getInt1Ty(), owner->OwnFlag, "own.srcowns"));
            else if (!owner->IsOwning)
                held = builder->getInt1(false);
            next = builder->CreateSelect(held, builder->getInt1(false), next);
        }
        builder->CreateStore(builder->CreateSelect(same, owned, next), namedVar.OwnFlag);
        if (namedVar.RefCountStorage != nullptr)
        {
            auto* count = builder->CreateLoad(builder->getInt32Ty(), namedVar.RefCountStorage);
            builder->CreateStore(builder->CreateSelect(same, count, builder->getInt32(1)),
                                 namedVar.RefCountStorage);
        }
    }

void LLVMBackend::RetargetStraightLineBlockFacts(llvm::BasicBlock* from, llvm::BasicBlock* to)
{
        if (from == nullptr || to == nullptr || from == to) return;
        auto retarget = [&](NamedVariable& nv)
        {
            for (llvm::BasicBlock** b : { &nv.OwnedStringBorrowBlock, &nv.DeclarationBlock,
                                          &nv.ReboundBlock, &nv.ExplicitNullBlock, &nv.BondDeclBlock,
                                          &nv.AliasBorrowDeclBlock, &nv.AssignBorrowBlock,
                                          &nv.OwnedElementBorrowBlock })
                if (*b == from) *b = to;
        };
        for (auto& frame : stackNamedVariable)
        {
            for (auto& [name, nv] : frame.namedVariable) retarget(nv);
            for (auto& [name, nv] : frame.functionArgument) retarget(nv);
        }
    }

void LLVMBackend::NoteOwnSlotLeavingLoad(const llvm::Value* load)
{
        if (llvm::isa_and_nonnull<llvm::LoadInst>(load))
            ownSlotLeavingLoads_.emplace_back(const_cast<llvm::Value*>(load));
    }

/*
 * Where may a copy of the block an owning slot holds live on elsewhere? A load of the slot whose
 * value (or a pointer derived from it, or a pointer read out of its pointee) is stored anywhere
 * but back into the slot, handed to a callee that may retain it, or used in an unmodelled way
 * escapes at that load; any use of the slot other than a load or a store into it (`&p`) escapes
 * at that use. Loads that free or null the slot (ownSlotLeavingLoads_) alias nothing.
 */
std::vector<const llvm::Instruction*> LLVMBackend::OwnedSlotAliasPoints(
    const llvm::AllocaInst* slot, const std::unordered_set<const llvm::Value*>& leavingLoads)
{
        std::vector<const llvm::Instruction*> points;
        for (const llvm::User* u : slot->users())
        {
            const auto* inst = llvm::dyn_cast<llvm::Instruction>(u);
            if (inst == nullptr) { points.push_back(slot); continue; }
            if (const auto* st = llvm::dyn_cast<llvm::StoreInst>(inst))
            {
                if (st->getPointerOperand() != slot || st->getValueOperand() == slot)
                    points.push_back(inst);
                continue;
            }
            const auto* ld = llvm::dyn_cast<llvm::LoadInst>(inst);
            if (ld == nullptr) { points.push_back(inst); continue; }
            if (!leavingLoads.contains(ld) && OwnedSlotLoadEscapes(ld, slot))
                points.push_back(ld);
        }
        return points;
    }

bool LLVMBackend::OwnedSlotLoadEscapes(const llvm::LoadInst* root, const llvm::AllocaInst* slot)
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
                // A comparison yields a bool; a return ends the frame before any later rebind.
                if (llvm::isa<llvm::ICmpInst>(inst) || llvm::isa<llvm::ReturnInst>(inst)) continue;
                if (const auto* st = llvm::dyn_cast<llvm::StoreInst>(inst))
                {
                    if (st->getValueOperand() != v) continue;       // writing THROUGH the block
                    // `p = p` stores the slot's own value back; any other destination aliases.
                    if (v == root && st->getPointerOperand() == slot) continue;
                    return true;
                }
                if (const auto* ld = llvm::dyn_cast<llvm::LoadInst>(inst))
                {
                    // A scalar read ends here; a pointer read out of the block may die with it.
                    if (TypeHoldsPointer(ld->getType()) && visited.insert(ld).second)
                        work.push_back(ld);
                    continue;
                }
                if (const auto* call = llvm::dyn_cast<llvm::CallBase>(inst))
                {
                    const llvm::Function* callee = call->getCalledFunction();
                    if (callee == nullptr) return true;
                    if (CallIsPointerOpaqueIntrinsic(callee))
                    {
                        if (callee->getName().starts_with("llvm.mem")) return true;
                        continue;
                    }
                    // An element / field destroyed and written back over (`s[0] = x`).
                    if (CallIsOverwrittenFieldDestructor(call, v)) continue;
                    bool passedAsArg = false;
                    for (unsigned i = 0; i < call->arg_size(); ++i)
                    {
                        if (call->getArgOperand(i) != v) continue;
                        passedAsArg = true;
                        if (ParameterRetainsArgument(callee, i, 0)) return true;
                    }
                    if (!passedAsArg) return true;
                    continue;
                }
                if (llvm::isa<llvm::GetElementPtrInst>(inst) || llvm::isa<llvm::BitCastInst>(inst)
                    || llvm::isa<llvm::AddrSpaceCastInst>(inst) || llvm::isa<llvm::PHINode>(inst)
                    || llvm::isa<llvm::SelectInst>(inst) || llvm::isa<llvm::InsertValueInst>(inst)
                    || llvm::isa<llvm::ExtractValueInst>(inst))
                {
                    if (visited.insert(inst).second) work.push_back(inst);
                    continue;
                }
                return true;
            }
        }
        return false;
    }

void LLVMBackend::ResolveOwnedReleaseGates()
{
        std::vector<OwnReleaseGate> pending;
        pending.swap(ownReleaseGates_);
        std::vector<OwnAdoptGate> adopts;
        adopts.swap(ownAdoptGates_);
        std::unordered_set<const llvm::Value*> leavingLoads;
        for (const auto& load : ownSlotLeavingLoads_)
            if (load) leavingLoads.insert(load);
        ownSlotLeavingLoads_.clear();
        struct SlotAliases
        {
            std::vector<const llvm::Instruction*> Points;
            std::unordered_set<const llvm::BasicBlock*> ReachedByEdge;   // >= 1 CFG edge from a point
            bool Always = false;
        };
        std::unordered_map<const llvm::AllocaInst*, SlotAliases> bySlot;
        auto aliasesOf = [&](const llvm::AllocaInst* slot) -> const SlotAliases&
        {
            auto it = bySlot.find(slot);
            if (it != bySlot.end()) return it->second;
            SlotAliases aliases;
            aliases.Points = OwnedSlotAliasPoints(slot, leavingLoads);
            std::vector<const llvm::BasicBlock*> work;
            for (const auto* point : aliases.Points)
            {
                if (point == slot) aliases.Always = true;
                for (const auto* succ : llvm::successors(point->getParent()))
                    if (aliases.ReachedByEdge.insert(succ).second) work.push_back(succ);
            }
            while (!work.empty())
            {
                const auto* bb = work.back();
                work.pop_back();
                for (const auto* succ : llvm::successors(bb))
                    if (aliases.ReachedByEdge.insert(succ).second) work.push_back(succ);
            }
            return bySlot.emplace(slot, std::move(aliases)).first->second;
        };
        // Flow-sensitive: an alias taken only after the site (never looping back to it)
        // cannot hold the block the site moves.
        auto aliasedAt = [&](const llvm::AllocaInst* slot, const llvm::Instruction* site)
        {
            const auto& aliases = aliasesOf(slot);
            if (aliases.Always || aliases.ReachedByEdge.contains(site->getParent())) return true;
            for (const auto* point : aliases.Points)
                if (point->getParent() == site->getParent() && point->comesBefore(site))
                    return true;
            return false;
        };
        for (auto& gate : pending)
        {
            auto* slot = llvm::dyn_cast_or_null<llvm::AllocaInst>(gate.Slot);
            auto* init = llvm::dyn_cast_or_null<llvm::StoreInst>(gate.Init);
            auto* site = llvm::dyn_cast_or_null<llvm::Instruction>(gate.Site);
            if (slot == nullptr || init == nullptr || site == nullptr) continue;
            if (!aliasedAt(slot, site))
                init->setOperand(0, llvm::ConstantInt::getTrue(init->getContext()));
        }
        // An adopted block may also be reached through an alias of any slot it passed through.
        std::unordered_map<const llvm::AllocaInst*, std::vector<const llvm::AllocaInst*>> adoptedFrom;
        for (auto& gate : adopts)
        {
            auto* src = llvm::dyn_cast_or_null<llvm::AllocaInst>(gate.Source);
            auto* dest = llvm::dyn_cast_or_null<llvm::AllocaInst>(gate.Dest);
            if (src != nullptr && dest != nullptr) adoptedFrom[dest].push_back(src);
        }
        for (auto& gate : adopts)
        {
            auto* src = llvm::dyn_cast_or_null<llvm::AllocaInst>(gate.Source);
            auto* init = llvm::dyn_cast_or_null<llvm::StoreInst>(gate.Init);
            auto* site = llvm::dyn_cast_or_null<llvm::Instruction>(gate.Site);
            if (src == nullptr || init == nullptr || site == nullptr) continue;
            std::unordered_set<const llvm::AllocaInst*> seen{ src };
            std::vector<const llvm::AllocaInst*> work{ src };
            bool aliased = false;
            while (!aliased && !work.empty())
            {
                const auto* slot = work.back();
                work.pop_back();
                if (slot->getFunction() != site->getFunction() || aliasedAt(slot, site))
                    aliased = true;
                else if (auto it = adoptedFrom.find(slot); it != adoptedFrom.end())
                    for (const auto* from : it->second)
                        if (seen.insert(from).second) work.push_back(from);
            }
            if (!aliased) init->setOperand(0, llvm::ConstantInt::getTrue(init->getContext()));
        }
    }

void LLVMBackend::ClearOwnFlag(const std::string& varName)
{
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            auto it = frame.namedVariable.find(varName);
            if (it == frame.namedVariable.end()) continue;
            auto* flag = it->second.OwnFlag;
            if (it->second.OwnFlagActive && flag != nullptr && IsInsertBlockLive()
                && flag->getFunction() == builder->GetInsertBlock()->getParent())
                builder->CreateStore(builder->getInt1(false), flag);
            return;
        }
    }

void LLVMBackend::SetVariableOwnsInterfaceBox(const std::string& varName, bool value)
{
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            auto it = frame.namedVariable.find(varName);
            if (it != frame.namedVariable.end())
            {
                it->second.OwnsInterfaceBox = value;
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

void LLVMBackend::EmitOwningPtrDestructor(const NamedVariable& namedVar, llvm::Value* ptrVal,
                                          const std::string& typeName,
                                          llvm::Value* rawArrayCount)
{
        // TypeName names the C++ pointee even when this allocation is an array of pointers.
        // Pointer elements have no C++ object destructor; only release the backing allocation.
        if (namedVar.TypeAndValue.ValuePointerDepth() >= 2) return;
        auto* dtor = GetFullDestructorForDelete(typeName);
        if (dtor == nullptr) return;

        llvm::Value* count = rawArrayCount;
        if (count == nullptr
            && (namedVar.RawArrayLengthStorage != nullptr || namedVar.RawArrayLength != nullptr))
            count = LoadRawArrayLength(namedVar);
        if (count != nullptr)
        {
            count = Upconvert(count, builder->getInt64Ty());
            auto* arrayBB = llvm::BasicBlock::Create(
                *context, "raw_array_dtor_array", builder->GetInsertBlock()->getParent());
            auto* scalarBB = llvm::BasicBlock::Create(
                *context, "raw_array_dtor_scalar", builder->GetInsertBlock()->getParent());
            auto* doneBB = llvm::BasicBlock::Create(
                *context, "raw_array_dtor_done", builder->GetInsertBlock()->getParent());
            builder->CreateCondBr(
                builder->CreateICmpSGE(count, builder->getInt64(0)), arrayBB, scalarBB);
            builder->SetInsertPoint(arrayBB);
            EmitCountedArrayDestruction(ptrVal, typeName, count);
            builder->CreateBr(doneBB);
            builder->SetInsertPoint(scalarBB);
            builder->CreateCall(dtor->getFunctionType(), dtor, { ptrVal });
            builder->CreateBr(doneBB);
            builder->SetInsertPoint(doneBB);
            return;
        }
        builder->CreateCall(dtor->getFunctionType(), dtor, { ptrVal });
    }

void LLVMBackend::EmitOwningPtrCleanup(const NamedVariable& namedVar, llvm::Value* replacement,
                                       llvm::Value* releaseGate)
{
        // Load the current pointer value from the alloca
        auto* ptrVal = builder->CreateLoad(namedVar.BaseType, namedVar.Storage);
        NoteOwnSlotLeavingLoad(ptrVal);

        // Skip if null (pointer may have been moved out)
        llvm::Value* skipCleanup = builder->CreateICmpEQ(
            ptrVal,
            llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(namedVar.BaseType)));
        if (replacement != nullptr && replacement->getType() == ptrVal->getType())
            skipCleanup = builder->CreateOr(
                skipCleanup, builder->CreateICmpEQ(ptrVal, replacement, "move.same"));
        if (namedVar.OwnFlagActive && namedVar.OwnFlag != nullptr
            && namedVar.OwnFlag->getFunction() == builder->GetInsertBlock()->getParent())
            skipCleanup = builder->CreateOr(skipCleanup, builder->CreateNot(builder->CreateLoad(
                builder->getInt1Ty(), namedVar.OwnFlag, "own.flag")));
        if (releaseGate != nullptr)
            skipCleanup = builder->CreateOr(skipCleanup, builder->CreateNot(releaseGate));
        auto* cleanupBB = llvm::BasicBlock::Create(*context, "move.cleanup", builder->GetInsertBlock()->getParent());
        auto* afterBB   = llvm::BasicBlock::Create(*context, "move.after",   builder->GetInsertBlock()->getParent());
        builder->CreateCondBr(skipCleanup, afterBB, cleanupBB);

        builder->SetInsertPoint(cleanupBB);

        // M6 - a foreign C++ pointee with a VIRTUAL destructor is released through the vtable's
        // DELETING destructor, which destroys the derived object and frees its storage in one
        // call. This is the scope-exit leg of the explicit `delete` path and must agree with it.
        // A C++ class `new T[n]` block (a view, or a constant raw array count) is an array: never
        // the deleting destructor of element 0.
        const std::string& ownTypeName = namedVar.TypeAndValue.TypeName;
        const bool cxxAllocator = namedVar.TypeAndValue.ValuePointerDepth() < 2
            && CxxClassUsesCxxAllocator(ownTypeName);
        llvm::Value* cxxArrayCount = nullptr;
        if (cxxAllocator
            && (namedVar.RawArrayLengthStorage != nullptr || namedVar.RawArrayLength != nullptr))
            cxxArrayCount = LoadRawArrayLength(namedVar);
        auto* constCount = llvm::dyn_cast_or_null<llvm::ConstantInt>(cxxArrayCount);
        const bool knownCxxArray = cxxAllocator && (namedVar.TypeAndValue.IsArrayView
            || (constCount != nullptr && !constCount->isNegative()));
        if (!namedVar.TypeAndValue.ElemPointer && !knownCxxArray
            && CxxHasVirtualDestructor(namedVar.TypeAndValue.TypeName)
            && EmitCxxVirtualDelete(namedVar.TypeAndValue.TypeName, ptrVal))
        {
            builder->CreateBr(afterBB);
            builder->SetInsertPoint(afterBB);
            return;
        }

        // Call the full destructor (user dtor + member fields) if the type needs one. Resolve
        // through the delete-site resolver: a pointee still incomplete here (self-referential
        // element) binds the deferred stub instead of silently dropping the call.
        EmitOwningPtrDestructor(namedVar, ptrVal, namedVar.TypeAndValue.TypeName);

        // Free the pointer. An over-aligned block came from the aligned allocator, so it must be
        // freed via __delete_aligned to match. Two sources: the element TYPE's own alignment
        // (`struct alignas(64) T`), recovered here from the static type just like the `delete`
        // path does, and any per-site `new T[n] alignas(N)` excess carried on the local.
        auto* voidPtrTy = cflat_llvm::PointerTo(builder->getInt8Ty());
        auto* voidPtr = builder->CreateBitCast(ptrVal, voidPtrTy);
        // The DECLARED clause counts too: a global has no mutable NamedVariable, so its tracked
        // AllocAlignment is never established and only 'alignas(0, N)' on the declaration records
        // that the block came from the aligned allocator.
        uint64_t effAlign = std::max(namedVar.AllocAlignment, namedVar.TypeAndValue.AllocAlignValue);
        if (!namedVar.TypeAndValue.TypeName.empty() && !namedVar.TypeAndValue.ElemPointer)
        {
            TypeAndValue tv{ .TypeName = namedVar.TypeAndValue.TypeName };
            llvm::Type* t = GetType(tv);
            if (t != nullptr && t->isSized())
                effAlign = std::max(effAlign, GetEffectiveAlignmentForType(tv.TypeName, t));
        }
        // A foreign nontrivial C++ pointee was allocated by the C++ global operator new, so its
        // release must use the matching C++ operator delete - this is the `unique T* p = new T(..)`
        // scope-exit leg, and it has to agree with the explicit `delete p` leg.
        if (cxxAllocator)
        {
            if (namedVar.TypeAndValue.IsArrayView)
                EmitCxxHeapFreeArray(ownTypeName, voidPtr, cxxArrayCount, effAlign);
            else
                EmitCxxHeapFreeCounted(ownTypeName, voidPtr, cxxArrayCount, effAlign);
            builder->CreateBr(afterBB);
            builder->SetInsertPoint(afterBB);
            return;
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
            && (namedVar.TypeAndValue.IsUnique
                || IsCoreUniqueType(namedVar.TypeAndValue.TypeName)
                || namedVar.OwnsInterfaceBox)
            && namedVar.TypeAndValue.IsFatInterfaceValue();
}

void LLVMBackend::EmitOwningInterfaceCleanup(const NamedVariable& namedVar)
{
        auto* fatVal = builder->CreateLoad(GetFatPtrType(), namedVar.Storage);
        DeleteInterfaceValue(fatVal, namedVar.TypeAndValue.TypeName, namedVar.Storage);
    }

bool LLVMBackend::IsOwningUniqueArray(const NamedVariable& namedVar) const
{
        // A core unique wrapper can also own a counted raw `new T[n]` result. Its `_p` field
        // uses the same counted destruction path as a raw pointer; the wrapper has no array
        // syntax, so only explicit raw-array provenance enables this arm.
        if (namedVar.Storage != nullptr && namedVar.BaseType != nullptr
            && namedVar.IsOwning && namedVar.AllocatedByRawNewArray
            && !namedVar.TypeAndValue.Pointer
            && IsCoreUniqueType(namedVar.TypeAndValue.TypeName))
            return namedVar.BaseType->isStructTy();
        // IsOwning is deliberately NOT required: it is set from a scalar's single `new` source and
        // an array has none. `unique` on the declaration is itself the ownership statement here.
        if (namedVar.Storage == nullptr || namedVar.BaseType == nullptr) return false;
        if (namedVar.IsAliasBorrow || namedVar.TypeAndValue.IsAlias) return false;
        if (!namedVar.TypeAndValue.IsUnique
            && !IsCoreUniqueType(namedVar.TypeAndValue.TypeName)) return false;
        if (namedVar.TypeAndValue.ConstArraySize == 0) return false;
        return namedVar.BaseType->isArrayTy();
    }

void LLVMBackend::EmitOwningUniqueArrayCleanup(const NamedVariable& namedVar)
{
        if (!namedVar.TypeAndValue.Pointer && IsCoreUniqueType(namedVar.TypeAndValue.TypeName)
            && namedVar.AllocatedByRawNewArray)
        {
            const auto& uniqueData = GetDataStructure(namedVar.TypeAndValue.TypeName);
            for (size_t i = 0; i < uniqueData.StructFields.size(); i++)
            {
                if (uniqueData.StructFields[i].VariableName != "_p") continue;
                auto* pointerField = CreateStructGEP(
                    namedVar.BaseType, namedVar.Storage, (unsigned)i);
                NamedVariable raw = namedVar;
                raw.Storage = pointerField;
                raw.BaseType = GetType(TypeAndValue{
                    .TypeName = MangledGenericArgument(*this, namedVar.TypeAndValue.TypeName),
                    .Pointer = true});
                raw.TypeAndValue.TypeName = MangledGenericArgument(
                    *this, namedVar.TypeAndValue.TypeName);
                raw.TypeAndValue.Pointer = true;
                raw.TypeAndValue.ConstArraySize = 0;
                EmitOwningPtrCleanup(raw);
                return;
            }
        }
        auto* arrTy = llvm::cast<llvm::ArrayType>(namedVar.BaseType);
        auto* elemTy = arrTy->getElementType();
        uint64_t count = arrTy->getNumElements();
        bool isIface = namedVar.TypeAndValue.IsFatInterfaceValue();
        bool isCoreUniqueValue = !namedVar.TypeAndValue.Pointer
            && IsCoreUniqueType(namedVar.TypeAndValue.TypeName);
        for (uint64_t i = 0; i < count; i++)
        {
            auto* elemPtr = builder->CreateConstInBoundsGEP2_64(arrTy, namedVar.Storage, 0, i, "uniq.elem");
            NamedVariable elem = namedVar;
            elem.Storage = elemPtr;
            elem.BaseType = elemTy;
            elem.TypeAndValue.ConstArraySize = 0;
            if (isIface)
                EmitOwningInterfaceCleanup(elem);
            else if (isCoreUniqueValue && elemTy->isStructTy())
            {
                if (auto* dtor = GetOrCreateFullDestructor(namedVar.TypeAndValue.TypeName))
                    builder->CreateCall(dtor->getFunctionType(), dtor, { elemPtr });
            }
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

bool LLVMBackend::IsProducedTempValue(llvm::Value* value) const
{
        if (value == nullptr) return false;
        if (llvm::isa<llvm::CallBase>(value)) return true;
        return std::find(nullConditionalTempResults_.begin(), nullConditionalTempResults_.end(),
                         value) != nullConditionalTempResults_.end();
}

void LLVMBackend::PropagateProducedTempValue(llvm::Value* from, llvm::Value* to)
{
        if (from == nullptr || to == nullptr || from == to || !IsProducedTempValue(from)) return;
        if (std::find(nullConditionalTempResults_.begin(), nullConditionalTempResults_.end(), to)
            == nullConditionalTempResults_.end())
            nullConditionalTempResults_.push_back(to);
}

void LLVMBackend::PropagateNullConditionalOwnership(llvm::Value* from, llvm::Value* to)
{
        if (from == nullptr || to == nullptr || from == to) return;
        // The merged load stands in for the access arm's produced temp, so the registration
        // sites downstream must see it as one.
        PropagateProducedTempValue(from, to);
        PropagateOwnedReturnTemp(from, to);
        // A closure temp is registered by the CALL and has no consumer-side registration site to
        // re-find it, so its ledger entry must MOVE to the merged value (a string's does not:
        // the produced-temp sites above re-register that one where it matters).
        if (IsOwnedClosureTemp(from))
        {
            UnregisterOwnedClosureTemp(from);
            RegisterOwnedClosureTemp(to);
        }
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

void LLVMBackend::RegisterOwnedPtrTemp(llvm::Value* value, llvm::Value* releaseGate)
{
        std::string typeName;
        llvm::Value* rawArrayCount = RawArrayCountOf(value);
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
        for (auto it = pendingOwnedPtrTemps.begin(); it != pendingOwnedPtrTemps.end(); ++it)
        {
            if (it->Value != value) continue;   // idempotent: one buffer, one free
            // An unproven C++ use gates the free; two different gates cannot both be honoured.
            if (releaseGate == nullptr || it->ReleaseGate == releaseGate) return;
            if (it->ReleaseGate == nullptr) it->ReleaseGate = releaseGate;
            else pendingOwnedPtrTemps.erase(it);
            return;
        }
        pendingOwnedPtrTemps.push_back(
            { value, rawArrayCount, typeName, allocAlign, builder->GetInsertBlock(), releaseGate });
    }

bool LLVMBackend::IsOwningPtrTempValue(llvm::Value* value) const
{
        if (value == nullptr || !value->getType()->isPointerTy()) return false;
        const OwnedReturnReleaseTemp* e = FindOwnedReturnEntry(value);
        if (e != nullptr && e->IsOwningPtr) return true;
        const OwnedNewTemp* n = FindOwnedNewTemp(value);
        return n != nullptr && !n->TypeName.empty();
    }

void LLVMBackend::RegisterNonEscapingOwningPtrArgs(llvm::Value* callResult, bool calleeIsCxx)
{
        auto* call = llvm::dyn_cast_or_null<llvm::CallBase>(callResult);
        if (call == nullptr) return;
        const llvm::Function* callee = call->getCalledFunction();
        if (callee == nullptr) return;   // virtual / indirect dispatch: no body to prove
        if (callee->isDeclaration())
        {
            // A C++ body (inline method, wrapper) is linked in after the walk: free behind a gate
            // that ResolveCxxThisEscapeGates opens only on a proof over that body.
            if (!calleeIsCxx || !callee->hasName() || callee->isVarArg()) return;
            for (unsigned i = 0; i < call->arg_size(); ++i)
                RegisterCxxGatedOwningPtrArg(call->getArgOperand(i), *callee, i);
            return;
        }
        for (unsigned i = 0; i < call->arg_size(); ++i)
        {
            llvm::Value* argVal = call->getArgOperand(i);
            if (!IsOwningPtrTempValue(argVal)) continue;
            if (ParameterRetainsArgument(callee, i)) continue;
            RegisterOwnedPtrTemp(argVal);
        }
    }

void LLVMBackend::RegisterCxxGatedOwningPtrArg(llvm::Value* argVal, const llvm::Function& callee,
                                               unsigned argIndex)
{
        if (!IsOwningPtrTempValue(argVal)) return;
        auto* gateSlot = AllocaAtEntry(builder->getInt1Ty(), nullptr, "cxx.this.release");
        llvm::StoreInst* init = nullptr;
        {
            llvm::IRBuilderBase::InsertPointGuard guard(*builder);
            builder->SetInsertPoint(gateSlot->getParent(), std::next(gateSlot->getIterator()));
            init = builder->CreateStore(builder->getInt1(false), gateSlot);
        }
        cxxThisEscapeGates_.push_back({ init, callee.getName().str(), argIndex });
        RegisterOwnedPtrTemp(argVal, gateSlot);
    }

bool LLVMBackend::RegisterCxxOwningPtrArgsBeforeCall(const llvm::Function* callee,
                                                     llvm::ArrayRef<llvm::Value*> args)
{
        // Same filter as the declaration branch of RegisterNonEscapingOwningPtrArgs; `args` must
        // map 1:1 onto the callee's parameters (a direct, non-lowered call).
        if (callee == nullptr || !callee->isDeclaration() || !callee->hasName() || callee->isVarArg())
            return false;
        for (unsigned i = 0; i < args.size(); ++i)
            RegisterCxxGatedOwningPtrArg(args[i], *callee, i);
        return true;
    }

void LLVMBackend::ResolveCxxThisEscapeGates()
{
        if (cxxThisEscapeGates_.empty()) return;
        std::vector<CxxThisEscapeGate> pending;
        pending.swap(cxxThisEscapeGates_);
        // The link replaced declarations with new Function objects: a memo keyed on a pointer
        // from before it could answer for a stranger at a recycled address.
        paramRetainsMemo_.clear();
        paramRetainsPastCallMemo_.clear();
        NoCurrentFunctionScope noCurrent(this);
        for (const auto& gate : pending)
        {
            auto* init = llvm::dyn_cast_or_null<llvm::StoreInst>(gate.Init);
            if (init == nullptr) continue;
            const llvm::Function* fn = module->getFunction(gate.Callee);
            // Still a declaration (out-of-line member, library symbol): no proof, keep the leak.
            if (fn == nullptr || fn->isDeclaration() || gate.ArgIndex >= fn->arg_size()) continue;
            if (ParameterRetainsArgument(fn, gate.ArgIndex)) continue;
            init->setOperand(0, llvm::ConstantInt::getTrue(init->getContext()));
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
        std::erase_if(borrowingSinkInProgress_, [&](const auto& k) { return k.first == fn; });
        std::erase_if(owningLocalBorrowingHelperArgs_,
                      [&](const auto& e) { return e.Callee == fn; });
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
        borrowingSinkInProgress_.clear();
        tempUniqueFieldArgs_.clear();
        coreUniqueGetterSource_.clear();
        owningLocalBorrowingHelperArgs_.clear();
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
        // A nontrivial C++ value returned by value is held in a tracked sret slot. Its loaded
        // SSA value is an owning temporary even though C++ records are not CFlat owning structs.
        std::unordered_set<const llvm::Value*> visiting;
        auto ownsCxxTemp = [&](auto&& self, const llvm::Value* value) -> bool {
            if (value == nullptr || !visiting.insert(value).second) return false;
            auto* st = llvm::dyn_cast<llvm::StructType>(value->getType());
            if (st == nullptr || !st->hasName()
                || !IsForeignNontrivialCxxClass(st->getName().str()))
            {
                visiting.erase(value);
                return false;
            }
            bool owns = false;
            if (auto* phi = llvm::dyn_cast<llvm::PHINode>(value); phi != nullptr)
            {
                owns = phi->getNumIncomingValues() != 0;
                for (unsigned i = 0; owns && i < phi->getNumIncomingValues(); ++i)
                    owns = self(self, phi->getIncomingValue(i));
            }
            if (!owns)
            {
                NamedVariable temp;
                temp.Primary = const_cast<llvm::Value*>(value);
                if (auto* load = llvm::dyn_cast<llvm::LoadInst>(value))
                    temp.Storage = const_cast<llvm::Value*>(load->getPointerOperand());
                owns = IsOwnedTempValue(temp);
            }
            visiting.erase(value);
            return owns;
        };
        if (ownsCxxTemp(ownsCxxTemp, arm)) return true;
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
            // A joined value carries no runtime owned bit, so suppression must be recorded by
            // VALUE identity: a destination reads it to borrow instead of adopting. The same
            // identity is used to reject a direct interface receiver, whose selected PHI arm
            // cannot be proven safe to release after dispatch.
            bool interfaceArmOwnershipDisagrees = IsInterfaceFatValue(joined)
                && TernaryArmJoinsOwning(trueValue) != TernaryArmJoinsOwning(falseValue);
            if (owningStructJoin || interfaceArmOwnershipDisagrees)
                RegisterNonOwningStructJoin(joined);
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

bool LLVMBackend::ClosureParameterMayEscape(const llvm::Function* fn, unsigned argIndex)
{
        if (fn == nullptr || argIndex >= fn->arg_size() || !FunctionBodyIsComplete(fn)) return true;
        const llvm::Argument* root = fn->getArg(argIndex);
        if (root->getType() != GetClosureFatPtrType()) return true;

        llvm::SmallPtrSet<const llvm::Value*, 32> aggregateSeen;
        llvm::SmallPtrSet<const llvm::Value*, 32> environmentSeen;
        std::vector<std::pair<const llvm::Value*, bool>> work{{root, false}};
        while (!work.empty())
        {
            auto [value, isEnvironment] = work.back();
            work.pop_back();
            auto& seen = isEnvironment ? environmentSeen : aggregateSeen;
            if (!seen.insert(value).second) continue;
            for (const llvm::User* user : value->users())
            {
                const auto* inst = llvm::dyn_cast<llvm::Instruction>(user);
                if (inst == nullptr) return true;
                if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(inst))
                {
                    if (store->getValueOperand() != value) continue;
                    const llvm::Value* destination = llvm::getUnderlyingObject(
                        store->getPointerOperand());
                    if (!llvm::isa<llvm::AllocaInst>(destination)) return true;
                    work.push_back({destination, isEnvironment});
                    continue;
                }
                if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(inst))
                {
                    if (llvm::isa<llvm::AllocaInst>(value))
                        work.push_back({load, isEnvironment});
                    continue;
                }
                if (const auto* extract = llvm::dyn_cast<llvm::ExtractValueInst>(inst))
                {
                    if (extract->getAggregateOperand() != value || extract->getNumIndices() == 0)
                        continue;
                    if (extract->getIndices()[0] == 1)
                        work.push_back({extract, true});
                    continue;
                }
                if (const auto* insert = llvm::dyn_cast<llvm::InsertValueInst>(inst))
                {
                    if (insert->getInsertedValueOperand() == value)
                        work.push_back({insert, false});
                    continue;
                }
                if (const auto* call = llvm::dyn_cast<llvm::CallBase>(inst))
                {
                    const llvm::Function* callee = call->getCalledFunction();
                    if (callee != nullptr
                        && callee->getName().starts_with("__closure_fat_ptr.copy"))
                    {
                        work.push_back({call, false});
                        continue;
                    }
                    if (callee == nullptr && isEnvironment) continue;
                    for (const llvm::Value* arg : call->args())
                        if (arg == value) return true;
                    continue;
                }
                if (llvm::isa<llvm::GetElementPtrInst>(inst)
                    || llvm::isa<llvm::BitCastInst>(inst)
                    || llvm::isa<llvm::AddrSpaceCastInst>(inst)
                    || llvm::isa<llvm::PtrToIntInst>(inst)
                    || llvm::isa<llvm::IntToPtrInst>(inst)
                    || llvm::isa<llvm::BinaryOperator>(inst)
                    || llvm::isa<llvm::PHINode>(inst)
                    || llvm::isa<llvm::SelectInst>(inst))
                {
                    work.push_back({inst, isEnvironment});
                }
            }
        }
        return false;
}

bool LLVMBackend::ParameterRetainsArgument(const llvm::Function* fn, unsigned argIndex, int depth)
{
        // A va_arg slot is a C boundary: it cannot retain caller ownership and has no body to walk.
        if (fn == nullptr) return true;
        if (fn->isVarArg() && argIndex >= fn->arg_size()) return false;
        if (argIndex >= fn->arg_size() || depth > kMaxRetainDepth) return true;
        // Gate before consulting the memo: a half-emitted body can later grow an escaping use.
        if (!FunctionBodyIsComplete(fn)) return true;
        auto key = std::make_pair(fn, argIndex);
        if (auto it = paramRetainsMemo_.find(key); it != paramRetainsMemo_.end()) return it->second;
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
            if (cflat_llvm::GetTerminatorOrNull(&bb) == nullptr) return false;
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

std::optional<unsigned> LLVMBackend::CFlatParameterLLVMIndex(const FunctionSymbol& sym,
                                                              unsigned paramIndex) const
{
        if (sym.Function == nullptr || paramIndex >= sym.Parameters.size()) return std::nullopt;
        size_t expanded = 0;
        for (const auto& param : sym.Parameters)
            expanded += ParameterCarriesRawArrayCount(param) ? 2u : 1u;
        unsigned llvmIndex = sym.IsMethod && sym.Function->arg_size() == expanded + 1 ? 1u : 0u;
        for (unsigned i = 0; i < paramIndex; ++i)
            llvmIndex += ParameterCarriesRawArrayCount(sym.Parameters[i]) ? 2u : 1u;
        if (llvmIndex >= sym.Function->arg_size()) return std::nullopt;
        return llvmIndex;
    }

bool LLVMBackend::ParameterMayReachBorrowingContainerSink(const llvm::Function* fn,
                                                           unsigned argIndex, int depth)
{
        if (fn == nullptr || argIndex >= fn->arg_size() || depth > kMaxRetainDepth) return false;
        if (!FunctionBodyIsReadable(fn)) return false;
        auto key = std::make_pair(fn, argIndex);
        if (!borrowingSinkInProgress_.insert(key).second) return false;
        bool reaches = ValueMayReachBorrowingContainerSink(fn->getArg(argIndex), depth);
        borrowingSinkInProgress_.erase(key);
        return reaches;
    }

bool LLVMBackend::ValueMayReachBorrowingContainerSink(const llvm::Value* root, int depth)
{
        if (root == nullptr || depth > kMaxRetainDepth) return false;
        llvm::SmallPtrSet<const llvm::Value*, 16> visited;
        llvm::SmallVector<const llvm::Value*, 16> work;
        visited.insert(root);
        work.push_back(root);
        while (!work.empty())
        {
            if (visited.size() > kMaxRetainUses) return false;
            const llvm::Value* v = work.pop_back_val();
            for (const llvm::User* u : v->users())
            {
                const auto* inst = llvm::dyn_cast<llvm::Instruction>(u);
                if (inst == nullptr) continue;
                if (const auto* call = llvm::dyn_cast<llvm::CallBase>(inst))
                {
                    const FunctionSymbol* sym = FindSymbolForFunction(call->getCalledFunction());
                    if (sym != nullptr && sym->IsMethod && !sym->Parameters.empty())
                    {
                        const unsigned sinkParam = (unsigned)sym->Parameters.size() - 1;
                        auto sinkArg = CFlatParameterLLVMIndex(*sym, sinkParam);
                        if (sinkArg.has_value()
                            && IsBorrowingContainerElementSink(sym->SourceName, sym->Parameters,
                                                               sinkParam, sym->IsMethod)
                            && *sinkArg < call->arg_size()
                            && call->getArgOperand(*sinkArg) == v
                            && call->arg_size() > 0)
                        {
                            std::string destKind;
                            if (MemoryOutlivesCall(call->getArgOperand(0), destKind, 0))
                                return true;
                        }
                    }
                    // This is deliberately depth-1: a helper called by this function is not
                    // summarized recursively, so two-level forwarding remains accepted.
                    continue;
                }
                if (const auto* st = llvm::dyn_cast<llvm::StoreInst>(inst))
                {
                    if (st->getValueOperand() != v) continue;
                    if (llvm::isa<llvm::AllocaInst>(st->getPointerOperand())
                        && visited.insert(st->getPointerOperand()).second)
                        work.push_back(st->getPointerOperand());
                    continue;
                }
                if (const auto* ld = llvm::dyn_cast<llvm::LoadInst>(inst))
                {
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
                if (llvm::isa<llvm::GetElementPtrInst>(inst) || llvm::isa<llvm::BitCastInst>(inst)
                    || llvm::isa<llvm::AddrSpaceCastInst>(inst) || llvm::isa<llvm::PHINode>(inst)
                    || llvm::isa<llvm::SelectInst>(inst))
                {
                    if (visited.insert(inst).second) work.push_back(inst);
                }
            }
        }
        return false;
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

std::string LLVMBackend::BorrowedSourceName(const NamedVariable& nv)
{
        if (!nv.BorrowedOrigin.empty()) return nv.BorrowedOrigin;
        if (!nv.CallerName.empty()) return nv.CallerName;
        return nv.TypeAndValue.VariableName;
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
            // A blessed unique<X> argument arrived through get()/release(), so match its SOURCE.
            const llvm::Value* matchVal = argVal;
            if (auto srcIt = coreUniqueGetterSource_.find(argVal);
                srcIt != coreUniqueGetterSource_.end())
                matchVal = srcIt->second;
            for (const auto& nv : args)
                if (nv.Primary == argVal || nv.Primary == matchVal)
                { access = DescribeUniqueFieldAccess(nv); break; }
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
        // A generic implementor is registered under its MANGLED name; the diagnostic must spell
        // it back as source, like every other user-facing surface.
        std::string implSpelling = SpellType(*this, TypeAndValue{ .TypeName = firstImpl });
        implDetail = impls.size() == 1
            ? std::format("'{}.{}' stores it into {}", implSpelling, methodName, firstKind)
            : std::format("all {} implementors store it, e.g. '{}.{}' into {}",
                          impls.size(), implSpelling, methodName, firstKind);
        return true;
}

bool LLVMBackend::InterfaceCallMayRetainBondedArg(const std::string& ifaceName,
        const std::string& methodName, size_t arity, unsigned paramIndex)
{
        std::vector<std::string> impls;
        if (!EnumerateInterfaceImplementors(ifaceName, impls)) return true;
        const InterfaceMethod* method = FindInterfaceMethod(ifaceName, methodName, arity);
        if (method == nullptr || paramIndex >= method->Parameters.size()) return true;
        for (const std::string& impl : impls)
        {
            llvm::Function* fn = LookupInterfaceMethodImpl(impl, *method);
            if (fn == nullptr || !FunctionBodyIsComplete(fn)) return true;
            if (fn->getArg(paramIndex + 1)->getType() == GetClosureFatPtrType())
            {
                bool escapes = ClosureParameterMayEscape(fn, paramIndex + 1);
                if (escapes) return true;
            }
            else if (ParameterRetainsArgument(fn, paramIndex + 1))
                return true;
        }
        return false;
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
            if (param.IsMove || !param.Pointer) continue;
            if (i + 1 >= call->arg_size()) break;
            llvm::Value* argVal = call->getArgOperand((unsigned)(i + 1));
            bool carries = JoinCarriesOwningTempUniqueField(argVal);
            // A chain hop: the argument is itself a call whose callee is still below, so it is
            // only the temp's field if that hop proves. Carry its conditions forward, exactly as
            // the direct path does.
            const PendingLaunderTempUniqueField* pendingArg =
                carries ? nullptr : FindPendingLaunderTempUniqueField(argVal);
            if (!carries && pendingArg == nullptr) continue;
            std::string access;
            // A blessed unique<X> argument arrived through get()/release(), so match its SOURCE.
            const llvm::Value* matchVal = argVal;
            if (auto srcIt = coreUniqueGetterSource_.find(argVal);
                srcIt != coreUniqueGetterSource_.end())
                matchVal = srcIt->second;
            for (const auto& nv : args)
                if (nv.Primary == argVal || nv.Primary == matchVal)
                { access = DescribeUniqueFieldAccess(nv); break; }
            if (access.empty() && pendingArg != nullptr) access = pendingArg->Access;
            TempUniqueFieldArg entry{ nullptr, (unsigned)i, ifaceName + "." + method.Name, access,
                                      sourceFileName, currentLine, currentColumn,
                                      carries && !IsLedgeredOwningTempUniqueField(argVal),
                                      ifaceName, method.Name, param.VariableName,
                                      method.Parameters.size() };
            if (pendingArg != nullptr) entry.LaunderConds = pendingArg->Conds;
            const LaunderedTempUniqueField* inner = FindLaunderedTempUniqueField(argVal);
            std::string innerAccess = inner != nullptr ? inner->Access
                : (pendingArg != nullptr ? pendingArg->Access : access);
            // Return-side laundering, ALL polarity: re-ledgering feeds a REJECTION, so it may only
            // fire when the result IS the temp's field on every implementor that can be dispatched.
            if (EveryImplementorMayReturnInterfaceArg(ifaceName, method.Name,
                                                      method.Parameters.size(), (unsigned)i))
            {
                if (entry.LaunderConds.empty())
                {
                    RegisterOwningTempUniqueField(call);
                    RegisterLaunderedTempUniqueField(call, entry.CalleeName, innerAccess);
                }
                else
                {
                    // The incoming hop is still unproven, so the result is only a CANDIDATE
                    // launder: the destination sites defer their own answer on the same conds.
                    RegisterPendingLaunderTempUniqueField(call, entry.LaunderConds,
                                                         entry.CalleeName, innerAccess);
                }
            }
            std::string destKind, implDetail;
            // Answer now when every implementor is already emitted AND the argument is already
            // known to be the field, so the diagnostic lands inside any enclosing statement scope;
            // otherwise defer to the end-of-module resolve.
            if (entry.LaunderConds.empty()
                && EveryImplementorRetainsInterfaceArg(ifaceName, method.Name,
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

void LLVMBackend::ResolveOwningLocalBorrowingHelperArgs()
{
        std::vector<OwningLocalBorrowingHelperArg> pending;
        pending.swap(owningLocalBorrowingHelperArgs_);
        for (const auto& entry : pending)
        {
            if (!ParameterMayReachBorrowingContainerSink(entry.Callee, entry.ParamIndex)) continue;
            ReportingFileScope fileScope(this, entry.File, entry.Line, entry.Column);
            LogErrorMessage(
                "call to '{}': '{}' still owns the object it was given, and the helper parameter "
                "'{}' stores it into a container that only BORROWS its elements and never frees "
                "them, so the stored element would dangle. Declare parameter '{}' as 'move' and "
                "call '{}(move {})', or declare the container's element 'unique T*'.",
                { SpellFunctionSymbol(*this, entry.FunctionName), entry.ArgumentName,
                  entry.ParameterName, entry.ParameterName,
                  SpellFunctionSymbol(*this, entry.FunctionName), entry.ArgumentName });
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
            what += ", reached through a '?:' / '" "?" "?" "' join";
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
            what += ", reached through a '?:' / '" "?" "?" "' join";
        std::string param = entry.ParamName.empty()
            ? std::format("parameter #{}", entry.ArgIndex + 1)
            : std::format("parameter '{}'", entry.ParamName);
        // A generic interface INSTANCE is keyed by its mangled name - spell it back as source.
        std::string spelledMethod = entry.IfaceName.empty()
            ? entry.CalleeName
            : SpellType(*this, TypeAndValue{ .TypeName = entry.IfaceName }) + "." + entry.MethodName;
        LogError(std::format(
            "call to interface method '{}': cannot pass {} to plain pointer {} - every "
            "implementor of this method stores that pointer into memory that outlives the call "
            "({}), and the temporary's synthesized destructor frees the pointee at the end of this "
            "statement. Bind the whole call result to a local first and pass the field off that "
            "local, so the pointee outlives the statement - or declare the interface parameter "
            "'move' and pass 'move <local>.<field>' to hand ownership over.",
            spelledMethod, what, param, implDetail));
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

void LLVMBackend::RegisterViewJoinType(llvm::Value* value, const LLVMBackend::TypeAndValue& elementType)
{
        if (value == nullptr || !value->getType()->isPointerTy() || !elementType.IsArrayView) return;
        for (auto& entry : viewJoinTypes_)
            if (entry.first == value) { entry.second = elementType; return; }
        viewJoinTypes_.push_back({ value, elementType });
    }

const LLVMBackend::TypeAndValue* LLVMBackend::FindViewJoinType(llvm::Value* value) const
{
        if (value == nullptr) return nullptr;
        for (const auto& entry : viewJoinTypes_)
            if (entry.first == value) return &entry.second;
        return nullptr;
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

/*
 * The declared field at LLVM element `index` of a registered named struct, or nullptr.
 * Answers only what it can PROVE: the declared StructFields list and the LLVM element list must
 * be the same list, else the index would blame the wrong field (union, padded layout).
 */
const LLVMBackend::TypeAndValue* LLVMBackend::FindDeclaredFieldOfStructType(
        const llvm::StructType* structType, uint64_t index) const
{
        if (structType == nullptr || !structType->hasName()) return nullptr;
        auto entry = dataStructures.find(structType->getName().str());
        if (entry == dataStructures.end()) return nullptr;
        const auto& fields = entry->second.StructFields;
        if (entry->second.IsUnion || fields.size() != structType->getNumElements()) return nullptr;
        if (index >= fields.size()) return nullptr;
        return &fields[index];
}

/*
 * A struct-FIELD read has no declared local slot: its storage is a struct GEP, so the sibling
 * FindDeclaredTypeAndValueForStorage finds nothing. Recover the field's declared type from the
 * GEP's own source struct type, the same recovery the call door does from a name.
 * Requires a constant two-index GEP over a registered named struct; anything else stays unnamed.
 */
const LLVMBackend::TypeAndValue* LLVMBackend::FindDeclaredFieldTypeAndValueForStorage(
        const llvm::Value* storage) const
{
        const auto* gep = llvm::dyn_cast_or_null<llvm::GetElementPtrInst>(storage);
        if (gep == nullptr || gep->getNumIndices() != 2) return nullptr;
        auto* first = llvm::dyn_cast<llvm::ConstantInt>(gep->getOperand(1));
        auto* second = llvm::dyn_cast<llvm::ConstantInt>(gep->getOperand(2));
        if (first == nullptr || second == nullptr || !first->isZero()) return nullptr;
        return FindDeclaredFieldOfStructType(
            llvm::dyn_cast<llvm::StructType>(gep->getSourceElementType()), second->getZExtValue());
}

/*
 * Same question for a field read off a by-VALUE struct that never reached memory - a call result
 * addressed by `extractvalue` rather than by a GEP. One index only: the aggregate operand names
 * the struct the field is declared on.
 */
const LLVMBackend::TypeAndValue* LLVMBackend::FindDeclaredFieldTypeAndValueForValue(
        const llvm::Value* value) const
{
        const auto* extract = llvm::dyn_cast_or_null<llvm::ExtractValueInst>(value);
        if (extract == nullptr || extract->getNumIndices() != 1) return nullptr;
        return FindDeclaredFieldOfStructType(
            llvm::dyn_cast<llvm::StructType>(extract->getAggregateOperand()->getType()),
            extract->getIndices()[0]);
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

void LLVMBackend::RegisterBorrowedAddressValue(llvm::Value* value)
{
        if (value == nullptr || !value->getType()->isPointerTy()) return;
        borrowedAddressValues_.insert(value);
    }

bool LLVMBackend::IsBorrowedAddressValue(llvm::Value* value) const
{
        if (value == nullptr) return false;
        return borrowedAddressValues_.count(value) != 0;
    }

bool LLVMBackend::TernaryArmIsProvenBorrow(llvm::Value* armValue, llvm::Value* armStorage) const
{
        if (armValue == nullptr || !armValue->getType()->isPointerTy()) return false;
        if (IsBorrowedAddressValue(armValue)) return true;
        if (armStorage == nullptr) return false;
        const NamedVariable* nv = FindVariableByStorage(armStorage);
        if (nv == nullptr) return false;
        // An owning binding is never the borrow side, however its arm VALUE scores in the
        // owning-temp ledgers: `T* p = new T();` reads back as a plain load and owns all the same.
        if (nv->IsOwning || nv->IsNewAllocated || nv->IsOwningStruct || nv->IsOwningString)
            return false;
        return nv->IsBorrowed || nv->IsAliasBorrow || nv->IsRangeForBorrow
            || nv->PointsToBorrowedAddress;
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
        llvm::SmallVector<llvm::Value*, 8> work{ value };
        llvm::SmallPtrSet<llvm::Value*, 16> seen;
        while (!work.empty())
        {
            llvm::Value* current = work.pop_back_val();
            if (current == nullptr || !seen.insert(current).second) continue;
            std::erase_if(pendingOwnedPtrTemps,
                [&](const PendingOwnedPtrTemp& p) { return p.Value == current; });
            if (const auto* join = FindNullCoalesceJoin(current))
                for (const auto& arm : join->Arms) work.push_back(arm.Value);
        }
}

/*
 * Walks `v` back through GEPs, pointer casts, selects and PHIs to the objects it addresses and
 * asks `isTemp` of each. Without `includeSelf`, only an object reached through a GEP counts: the
 * temp pointer itself is an ownership question the adopting sites already answer.
 */
static bool AddressDerivesFromTemp(llvm::Value* v, bool includeSelf,
                                   const std::function<bool(llvm::Value*)>& isTemp)
{
        if (v == nullptr || !v->getType()->isPointerTy()) return false;
        llvm::SmallVector<std::pair<llvm::Value*, bool>, 8> work{ { v, includeSelf } };
        llvm::SmallPtrSet<llvm::Value*, 16> seen;
        while (!work.empty() && seen.size() < 64)
        {
            auto [cur, interior] = work.pop_back_val();
            if (!seen.insert(cur).second) continue;
            if (interior && isTemp(cur)) return true;
            if (auto* gep = llvm::dyn_cast<llvm::GEPOperator>(cur))
                work.push_back({ gep->getPointerOperand(), true });
            else if (auto* sel = llvm::dyn_cast<llvm::SelectInst>(cur))
            {
                work.push_back({ sel->getTrueValue(), interior });
                work.push_back({ sel->getFalseValue(), interior });
            }
            else if (auto* phi = llvm::dyn_cast<llvm::PHINode>(cur))
            {
                for (llvm::Value* in : phi->incoming_values()) work.push_back({ in, interior });
            }
            else if (auto* op = llvm::dyn_cast<llvm::Operator>(cur);
                     op != nullptr && (op->getOpcode() == llvm::Instruction::BitCast
                                       || op->getOpcode() == llvm::Instruction::AddrSpaceCast))
                work.push_back({ op->getOperand(0), interior });
        }
        return false;
    }

bool LLVMBackend::PointsIntoOwnedPtrTemp(llvm::Value* v, bool includeSelf, size_t fromIndex) const
{
        return AddressDerivesFromTemp(v, includeSelf, [&](llvm::Value* base) {
            for (size_t i = fromIndex; i < pendingOwnedPtrTemps.size(); ++i)
                if (pendingOwnedPtrTemps[i].Value == base) return true;
            return false;
        });
    }

void LLVMBackend::RecordPtrToIntOfTemp(llvm::Value* intValue, llvm::Value* ptrOperand)
{
        if (intValue == nullptr || ptrOperand == nullptr) return;
        if (AddressDerivesFromTemp(ptrOperand, /*includeSelf*/ true, [&](llvm::Value* base) {
                for (const auto& p : pendingOwnedPtrTemps)
                    if (p.Value == base) return true;
                return false;
            }))
            ptrToIntOfTemps_.push_back({ intValue, ptrOperand });
    }

/*
 * An integer address escapes unless every use is a compare, possibly after integer widening or
 * narrowing. Reaching `keep` (the value a return or a join carries on) is an escape too.
 */
static bool IntAddressEscapes(llvm::Value* intValue, llvm::Value* keep)
{
        llvm::SmallVector<llvm::Value*, 8> work{ intValue };
        llvm::SmallPtrSet<llvm::Value*, 16> seen;
        while (!work.empty())
        {
            llvm::Value* cur = work.pop_back_val();
            if (!seen.insert(cur).second) continue;
            if (cur == keep || seen.size() > 64) return true;
            for (llvm::User* user : cur->users())
            {
                if (llvm::isa<llvm::ICmpInst>(user)) continue;
                auto* cast = llvm::dyn_cast<llvm::CastInst>(user);
                if (cast != nullptr && cast->getType()->isIntegerTy()
                    && (llvm::isa<llvm::TruncInst>(cast) || llvm::isa<llvm::ZExtInst>(cast)
                        || llvm::isa<llvm::SExtInst>(cast)))
                {
                    work.push_back(cast);
                    continue;
                }
                return true;
            }
        }
        return false;
    }

void LLVMBackend::ClaimIntLaunderedPtrTemps(size_t fromIndex, llvm::Value* keep)
{
        for (const auto& [intValue, ptr] : ptrToIntOfTemps_)
            if (IntAddressEscapes(intValue, keep))
                ClaimOwnedPtrTempsUnder(ptr, /*includeSelf*/ true, fromIndex);
    }

void LLVMBackend::ClaimOwnedPtrTempsUnder(llvm::Value* v, bool includeSelf, size_t fromIndex)
{
        if (v == nullptr || !v->getType()->isPointerTy()) return;
        for (size_t i = fromIndex; i < pendingOwnedPtrTemps.size(); )
        {
            llvm::Value* temp = pendingOwnedPtrTemps[i].Value;
            if (pendingOwnedPtrTemps[i].ConditionalSlot == nullptr && temp != nullptr
                && AddressDerivesFromTemp(v, includeSelf,
                    [&](llvm::Value* base) { return base == temp; }))
            {
                addrClaimedPtrTemps_.push_back(temp);
                pendingOwnedPtrTemps.erase(pendingOwnedPtrTemps.begin() + i);
                continue;
            }
            ++i;
        }
    }

bool LLVMBackend::AddressIntoStatementPtrTemp(llvm::Value* v) const
{
        if (v != nullptr && std::find(addrIntoTempValues_.begin(), addrIntoTempValues_.end(), v)
            != addrIntoTempValues_.end())
            return true;
        return AddressDerivesFromTemp(v, /*includeSelf*/ false, [&](llvm::Value* base) {
            if (std::find(addrIntoTempValues_.begin(), addrIntoTempValues_.end(), base)
                != addrIntoTempValues_.end())
                return true;
            if (std::find(addrClaimedPtrTemps_.begin(), addrClaimedPtrTemps_.end(), base)
                != addrClaimedPtrTemps_.end())
                return true;
            for (const auto& p : pendingOwnedPtrTemps)
                if (p.Value == base) return true;
            return false;
        });
    }

bool LLVMBackend::IsInsertBlockLive() const
{
        auto* b = builder->GetInsertBlock();
        return b != nullptr && cflat_llvm::GetTerminatorOrNull(b) == nullptr;
    }

/*
 * Dominance over a CFG that is still being built. llvm::DominatorTree requires a
 * well-formed function - its DFS asserts on a block with no terminator - but these
 * queries run mid-codegen, where the block being emitted has none yet. `bb` dominates
 * `curBlock` exactly when `curBlock` is unreachable from entry once `bb` is removed;
 * a terminator-less block is treated as having no successors, which is what LLVM 22's
 * walk did in effect.
 */
static bool DominatesInPartialCfg(llvm::BasicBlock* bb, llvm::BasicBlock* curBlock)
{
        llvm::BasicBlock* entry = &curBlock->getParent()->getEntryBlock();
        if (bb == entry) return true;
        std::unordered_set<llvm::BasicBlock*> seen{ entry };
        std::vector<llvm::BasicBlock*> work{ entry };
        while (!work.empty())
        {
            llvm::BasicBlock* n = work.back();
            work.pop_back();
            if (n == curBlock) return false;
            if (cflat_llvm::GetTerminatorOrNull(n) == nullptr) continue;
            for (llvm::BasicBlock* s : llvm::successors(n))
                if (s != bb && seen.insert(s).second) work.push_back(s);
        }
        return true;
    }

static bool FunctionIsWellFormed(llvm::Function* f)
{
        for (llvm::BasicBlock& bb : *f)
            if (cflat_llvm::GetTerminatorOrNull(&bb) == nullptr) return false;
        return true;
    }

bool LLVMBackend::OwnedTempDominatesHere(llvm::BasicBlock* bb, llvm::BasicBlock* curBlock,
                                std::optional<llvm::DominatorTree>& dt) const
{
        if (bb == nullptr || curBlock == nullptr) return false;
        if (bb == curBlock) return true;
        if (bb->getParent() != curBlock->getParent()) return false;
        llvm::Function* f = curBlock->getParent();
        // The tree is only legal once every block is terminated; mid-codegen it is not.
        if (!FunctionIsWellFormed(f)) return DominatesInPartialCfg(bb, curBlock);
        if (!dt) dt.emplace(*f);
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

bool LLVMBackend::IsOwnedTempValue(const NamedVariable& arg) const
{
        auto* value = arg.Primary;
        if (value != nullptr)
        {
            for (const auto& e : pendingOwnedStringTemps)
                if (e.first == value) return true;
            if (IsOwnedClosureTemp(value)) return true;
        }
        for (const auto& e : pendingOwnedStructTemps)
            if ((e.Alloca == arg.Storage && arg.Storage != nullptr) || (value != nullptr && e.Alloca == value))
                return true;
        return false;
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

void LLVMBackend::UnregisterOwnedStructTemp(llvm::Value* value)
{
        if (value == nullptr) return;
        std::erase_if(pendingOwnedStructTemps,
            [&](const PendingOwnedStructTemp& e) { return e.Alloca == value; });
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

void LLVMBackend::EmitOwnedPtrTempFree(llvm::Value* ptrVal, const std::string& typeName,
                                       uint64_t allocAlign, llvm::Value* rawArrayCount,
                                       llvm::Value* releaseGate)
{
        auto* ptrTy = llvm::dyn_cast<llvm::PointerType>(ptrVal->getType());
        if (ptrTy == nullptr) return;
        if (releaseGate != nullptr)
        {
            // Skipped unless the post-link proof stored true into the gate slot.
            auto* fn = builder->GetInsertBlock()->getParent();
            auto* open = builder->CreateLoad(builder->getInt1Ty(), releaseGate, "tmpptr.gate");
            auto* freeBB = llvm::BasicBlock::Create(*context, "tmpptr.gated", fn);
            auto* doneBB = llvm::BasicBlock::Create(*context, "tmpptr.gate.done", fn);
            builder->CreateCondBr(open, freeBB, doneBB);
            builder->SetInsertPoint(freeBB);
            EmitOwnedPtrTempFree(ptrVal, typeName, allocAlign, rawArrayCount);
            if (IsInsertBlockLive()) builder->CreateBr(doneBB);
            builder->SetInsertPoint(doneBB);
            return;
        }
        // A C++-allocated pointee is released exactly as an owning slot at scope exit is (virtual
        // deleting dtor, or dtor + the paired class / global operator delete), so spill and reuse it.
        if (CxxClassUsesCxxAllocator(typeName))
        {
            NamedVariable slotVar;
            slotVar.TypeAndValue = TypeAndValue{ .TypeName = typeName, .Pointer = true };
            slotVar.BaseType = ptrTy;
            slotVar.Storage = AllocaAtEntry(ptrTy, nullptr, "tmpptr.cxx");
            slotVar.RawArrayLength = rawArrayCount;
            slotVar.AllocAlignment = allocAlign;
            builder->CreateStore(ptrVal, slotVar.Storage);
            EmitOwningPtrCleanup(slotVar);
            return;
        }
        auto* isNull = builder->CreateICmpEQ(ptrVal, llvm::ConstantPointerNull::get(ptrTy));
        auto* cleanupBB = llvm::BasicBlock::Create(*context, "tmpptr.cleanup", builder->GetInsertBlock()->getParent());
        auto* afterBB   = llvm::BasicBlock::Create(*context, "tmpptr.after",   builder->GetInsertBlock()->getParent());
        builder->CreateCondBr(isNull, afterBB, cleanupBB);

        builder->SetInsertPoint(cleanupBB);
        NamedVariable tempVar;
        EmitOwningPtrDestructor(tempVar, ptrVal, typeName, rawArrayCount);

        // Over-aligned blocks come from the aligned allocator and must be freed through
        // __delete_aligned - the same rule as EmitOwningPtrCleanup and the `delete` site.
        auto* voidPtr = builder->CreateBitCast(ptrVal, cflat_llvm::PointerTo(builder->getInt8Ty()));
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
        ClaimIntLaunderedPtrTemps(0, nullptr);

        auto temps = std::move(pendingOwnedPtrTemps);
        pendingOwnedPtrTemps.clear();
        for (auto& t : temps)
        {
            if (t.Value == nullptr || !IsInsertBlockLive()) continue;
            std::optional<llvm::DominatorTree> domTree;
            if (!OwnedTempDominatesHere(t.Block, builder->GetInsertBlock(), domTree)) continue; // dominance safety
            if (t.ConditionalSlot != nullptr) EmitOwnedConditionalPtrTempFree(t);
            else EmitOwnedPtrTempFree(t.Value, t.TypeName, t.AllocAlign, t.RawArrayCount, t.ReleaseGate);
        }
    }

bool LLVMBackend::BorrowedOwningStructTempQualifies(const NamedVariable& arg, bool fromTernaryArm)
{
        const std::string& typeName = arg.TypeAndValue.TypeName;
        if (arg.Primary == nullptr || arg.Storage != nullptr || arg.BaseType == nullptr) return false;
        if (!IsProducedTempValue(arg.Primary)) return false;   // only a produced temp, not a named local
        // A ternary PHI is registered per produced arm while the argument is parsed. Do not
        // register the joined value again when the call lowering sees the same argument.
        if (!fromTernaryArm && llvm::isa<llvm::PHINode>(arg.Primary)) return false;
        if (!arg.BaseType->isStructTy() || arg.Primary->getType() != arg.BaseType) return false;
        if (arg.TypeAndValue.Pointer || arg.TypeAndValue.IsAlias || arg.FromOwningTempField) return false;
        if (typeName.empty() || typeName == "string" || typeName == "__closure_fat_ptr") return false;
        return IsOwningValueType(typeName);
    }

void LLVMBackend::RegisterBorrowedOwningStructTemp(const NamedVariable& arg, bool fromTernaryArm)
{
        if (!BorrowedOwningStructTempQualifies(arg, fromTernaryArm)) return;

        auto* tempAlloca = AllocaAtEntry(arg.BaseType, nullptr, "argtemp");
        builder->CreateStore(arg.Primary, tempAlloca);
        RegisterOwnedStructTemp(tempAlloca, arg.TypeAndValue.TypeName);
    }

void LLVMBackend::RegisterBorrowedOwningStructTempAt(const NamedVariable& arg, llvm::Value* slot,
                                                     bool fromTernaryArm)
{
        // The alias-by-pointer arg path already spilled the rvalue into `slot`; registering that
        // very slot (instead of a second copy) is what keeps the temp destructed exactly once.
        if (slot == nullptr || !BorrowedOwningStructTempQualifies(arg, fromTernaryArm)) return;
        RegisterOwnedStructTemp(slot, arg.TypeAndValue.TypeName);
    }

LLVMBackend::OwnedTempMark LLVMBackend::MarkOwnedTemps() const
{
        return { pendingOwnedStringTemps.size(), pendingOwnedClosureTemps.size(),
                 pendingOwnedStructTemps.size(), pendingOwnedPtrTemps.size(), ownedNewTemps_.size() };
    }

void LLVMBackend::HoistOwnedPtrTempsForAddress(const OwnedTempMark& mark, llvm::Value* address,
                                                llvm::BasicBlock* hoistTo, bool includeBareNew)
{
        if (address == nullptr || hoistTo == nullptr) return;
        for (size_t i = includeBareNew ? mark.NewPtrs : ownedNewTemps_.size();
             i < ownedNewTemps_.size(); ++i)
        {
            llvm::Value* value = ownedNewTemps_[i].Value;
            if (AddressDerivesFromTemp(address, /*includeSelf*/ true,
                    [&](llvm::Value* base) { return base == value; }))
                RegisterOwnedPtrTemp(value);
        }
        for (size_t i = mark.Ptrs; i < pendingOwnedPtrTemps.size(); )
        {
            auto temp = pendingOwnedPtrTemps[i];
            if (temp.Value == nullptr
                || !AddressDerivesFromTemp(address, /*includeSelf*/ true,
                    [&](llvm::Value* base) { return base == temp.Value; }))
            {
                ++i;
                continue;
            }
            auto* ptrTy = llvm::dyn_cast<llvm::PointerType>(temp.Value->getType());
            if (ptrTy == nullptr) { ++i; continue; }
            const bool wasHoisted = temp.ConditionalSlot != nullptr;
            if (!wasHoisted)
                temp.ConditionalSlot = AllocaAtEntry(ptrTy, nullptr, "tmpptr.arm");
            {
                llvm::IRBuilderBase::InsertPointGuard guard(*builder);
                builder->SetInsertPoint(cflat_llvm::GetTerminatorOrNull(hoistTo));
                builder->CreateStore(llvm::ConstantPointerNull::get(ptrTy), temp.ConditionalSlot);
            }
            if (!wasHoisted) builder->CreateStore(temp.Value, temp.ConditionalSlot);
            temp.Block = hoistTo;
            pendingOwnedPtrTemps[i] = temp;
            ++i;
        }
    }

void LLVMBackend::EmitOwnedConditionalPtrTempFree(const PendingOwnedPtrTemp& temp)
{
        if (temp.ConditionalSlot == nullptr || !IsInsertBlockLive()) return;
        auto* slot = llvm::cast<llvm::AllocaInst>(temp.ConditionalSlot);
        auto* ptrTy = llvm::cast<llvm::PointerType>(slot->getAllocatedType());
        auto* value = builder->CreateLoad(ptrTy, temp.ConditionalSlot, "tmpptr.arm.value");
        builder->CreateStore(llvm::ConstantPointerNull::get(ptrTy), temp.ConditionalSlot);
        EmitOwnedPtrTempFree(value, temp.TypeName, temp.AllocAlign, temp.RawArrayCount,
                             temp.ReleaseGate);
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
        if (hoistTo == nullptr || cflat_llvm::GetTerminatorOrNull(hoistTo) == nullptr) return false;
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
        builder->SetInsertPoint(cflat_llvm::GetTerminatorOrNull(hoistTo));
        builder->CreateStore(llvm::Constant::getNullValue(alloca->getAllocatedType()), alloca);
        builder->CreateStore(builder->getInt1(false), temp.LiveFlag);
        if (saveBlock != nullptr) builder->SetInsertPoint(saveBlock, savePoint);
        temp.Block = hoistTo;
        return true;
    }

bool LLVMBackend::HoistOwnedStringTempTo(llvm::Value* value, llvm::BasicBlock* hoistTo)
{
        auto* strTy = llvm::StructType::getTypeByName(*context, "string");
        if (value == nullptr || strTy == nullptr || value->getType() != strTy) return false;
        if (hoistTo == nullptr || cflat_llvm::GetTerminatorOrNull(hoistTo) == nullptr) return false;
        auto* inst = llvm::dyn_cast<llvm::Instruction>(value);
        if (inst == nullptr || inst->getFunction() != hoistTo->getParent()) return false;
        if (inst->isTerminator() || inst->getNextNode() == nullptr) return false;
        EnsureStringDtorRegistered();
        auto it = dataStructures.find("string");
        if (it == dataStructures.end() || it->second.Destructor == nullptr) return false;

        auto* saveBlock = builder->GetInsertBlock();
        auto  savePoint = builder->GetInsertPoint();
        auto* slot = AllocaAtEntry(strTy, nullptr, "nc.strtmp");
        // Zero it in the dominating block: on the short-circuit path the store below never runs,
        // and a zeroed string makes the destructor a no-op.
        builder->SetInsertPoint(cflat_llvm::GetTerminatorOrNull(hoistTo));
        builder->CreateStore(llvm::Constant::getNullValue(strTy), slot);
        builder->SetInsertPoint(inst->getNextNode());
        builder->CreateStore(value, slot);
        if (saveBlock != nullptr) builder->SetInsertPoint(saveBlock, savePoint);
        pendingOwnedStructTemps.push_back({ slot, "string", hoistTo, nullptr });
        return true;
    }

void LLVMBackend::FlushOwnedTempsSince(const OwnedTempMark& mark, llvm::Value* keep,
                                       llvm::BasicBlock* hoistTo)
{
        // String temps registered in the branch are SSA values the join cannot name; re-home them
        // to `hoistTo` as alloca-backed struct temps so a joined value derived from one (a
        // `.data()` pointer) is not left dangling by an early free.
        if (hoistTo != nullptr)
        {
            for (size_t i = mark.Strings; i < pendingOwnedStringTemps.size(); )
            {
                auto& [value, bb] = pendingOwnedStringTemps[i];
                if (value != nullptr && value != keep && HoistOwnedStringTempTo(value, hoistTo))
                {
                    pendingOwnedStringTemps.erase(pendingOwnedStringTemps.begin() + i);
                    continue;
                }
                ++i;
            }
        }
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
        // A kept pointer INTO a temp (a `?:` arm `&(new T(x))->f`) outlives this range: claim it.
        ClaimOwnedPtrTempsUnder(keep, /*includeSelf*/ false, mark.Ptrs);
        ClaimIntLaunderedPtrTemps(mark.Ptrs, keep);
        std::vector<PendingOwnedPtrTemp> ptrTemps;
        for (size_t i = mark.Ptrs; i < pendingOwnedPtrTemps.size(); ++i)
            if (pendingOwnedPtrTemps[i].ConditionalSlot == nullptr
                && pendingOwnedPtrTemps[i].Value != keep)
                ptrTemps.push_back(pendingOwnedPtrTemps[i]);

        TrimOwnedTempsSince(pendingOwnedStringTemps,  mark.Strings,  keep, pairValue);
        TrimOwnedTempsSince(pendingOwnedClosureTemps, mark.Closures, keep, pairValue);
        TrimOwnedTempsSince(pendingOwnedStructTemps,  mark.Structs,  keep, structValue);
        size_t ptrWrite = mark.Ptrs;
        for (size_t i = mark.Ptrs; i < pendingOwnedPtrTemps.size(); ++i)
            if (pendingOwnedPtrTemps[i].ConditionalSlot != nullptr
                || ptrValue(pendingOwnedPtrTemps[i]) == keep)
                pendingOwnedPtrTemps[ptrWrite++] = pendingOwnedPtrTemps[i];
        pendingOwnedPtrTemps.resize(ptrWrite);

        for (auto& t : ptrTemps)
        {
            if (t.Value == nullptr || !IsInsertBlockLive()) continue;
            std::optional<llvm::DominatorTree> domTree;
            if (!OwnedTempDominatesHere(t.Block, builder->GetInsertBlock(), domTree)) continue;
            if (t.ConditionalSlot != nullptr) EmitOwnedConditionalPtrTempFree(t);
            else EmitOwnedPtrTempFree(t.Value, t.TypeName, t.AllocAlign, t.RawArrayCount, t.ReleaseGate);
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
        addrClaimedPtrTemps_.clear();
        addrIntoTempValues_.clear();
        ptrToIntOfTemps_.clear();
        nullConditionalTempResults_.clear();
        rawArrayResults_.clear();
        valueElementTypeNames_.clear();
        fatInterfaceValueTypeNames_.clear();
        viewJoinTypes_.clear();
        movedOutPtrValues_.clear();
        movedBorrowedPtrValues_.clear();
        movedBorrowedThroughFieldValues_.clear();
        aliasValues_.clear();
        tempFieldValues_.clear();
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
        addrClaimedPtrTemps_.clear();
        addrIntoTempValues_.clear();
        ptrToIntOfTemps_.clear();
        nullConditionalTempResults_.clear();
        rawArrayResults_.clear();
        valueElementTypeNames_.clear();
        fatInterfaceValueTypeNames_.clear();
        viewJoinTypes_.clear();
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
        aliasValues_.clear();
        tempFieldValues_.clear();
    }

bool LLVMBackend::NeedsConditionalDropFlag(const NamedVariable& namedVar) const
{
        if (namedVar.ConditionalDropFlag != nullptr || namedVar.Storage == nullptr || builder == nullptr)
            return namedVar.ConditionalDropFlag != nullptr;
        const std::string& typeName = namedVar.TypeAndValue.TypeName;
        if (!(IsForeignNontrivialCxxClass(typeName) || HasForeignNontrivialCxxField(typeName)))
            return false;
        auto* moveBlock = builder->GetInsertBlock();
        if (moveBlock == nullptr) return false;
        if (namedVar.DeclarationScopeDepth < stackNamedVariable.size()) return true;
        if (namedVar.DeclarationBlock == nullptr || moveBlock == namedVar.DeclarationBlock)
            return false;

        // A direct successor of a conditional/switch terminator is a branch arm. A join block
        // has multiple predecessors, so a move after the branch remains straight-line cleanup.
        if (llvm::pred_size(moveBlock) != 1) return false;
        auto* predecessor = *llvm::pred_begin(moveBlock);
        auto* terminator = predecessor->getTerminator();
        return terminator != nullptr && terminator->getNumSuccessors() > 1;
    }

llvm::GlobalVariable* LLVMBackend::EnsureGlobalCxxLiveFlag(const NamedVariable& namedVar)
{
        auto* global = llvm::dyn_cast_or_null<llvm::GlobalVariable>(namedVar.Storage);
        if (global == nullptr || !IsForeignNontrivialCxxClass(namedVar.TypeAndValue.TypeName))
            return nullptr;
        const std::string key = global->getName().str();
        if (auto it = globalCxxLiveFlags_.find(key); it != globalCxxLiveFlags_.end())
            return it->second;

        const std::string flagName = "__cflat_cxx_live." + key;
        auto* flag = module->getGlobalVariable(flagName, true);
        if (flag == nullptr)
        {
            flag = new llvm::GlobalVariable(
                *module, builder->getInt1Ty(), false, global->getLinkage(),
                llvm::ConstantInt::getTrue(*context), flagName);
        }
        globalCxxLiveFlags_[key] = flag;
        return flag;
    }

void LLVMBackend::EnsureConditionalDropFlag(NamedVariable& namedVar)
{
        if (namedVar.ConditionalDropFlag != nullptr || !NeedsConditionalDropFlag(namedVar)) return;
        auto* savedBlock = builder->GetInsertBlock();
        auto savedPoint = builder->GetInsertPoint();
        namedVar.ConditionalDropFlag = AllocaAtEntry(
            builder->getInt1Ty(), nullptr, namedVar.TypeAndValue.VariableName + ".dropflag");

        auto* declarationBlock = namedVar.DeclarationBlock;
        if (declarationBlock != nullptr && declarationBlock != savedBlock
            && declarationBlock->getTerminator() != nullptr)
        {
            builder->SetInsertPoint(declarationBlock, declarationBlock->getTerminator()->getIterator());
        }
        else if (declarationBlock != nullptr && declarationBlock != savedBlock)
        {
            builder->SetInsertPoint(declarationBlock);
        }
        // A nested compound can share its LLVM block with the declaration. In that case the
        // first move is the first point at which the lazily-created flag can be initialized.
        builder->CreateStore(builder->getInt1(true), namedVar.ConditionalDropFlag);
        if (savedBlock != nullptr) builder->SetInsertPoint(savedBlock, savedPoint);
    }

void LLVMBackend::RearmConditionalDropFlag(NamedVariable& namedVar)
{
        if (namedVar.ConditionalDropFlag == nullptr || builder == nullptr) return;
        builder->CreateStore(builder->getInt1(true), namedVar.ConditionalDropFlag);
    }

void LLVMBackend::EmitConditionalFullDestructor(const NamedVariable& namedVar, llvm::Function* dtor)
{
        if (dtor == nullptr) return;
        if (namedVar.ConditionalDropFlag == nullptr)
        {
            EmitFullDestructorOverStorage(*builder, namedVar.Storage, namedVar.BaseType, dtor);
            return;
        }
        auto* function = builder->GetInsertBlock()->getParent();
        auto* liveBlock = llvm::BasicBlock::Create(*context, "cxx.drop.live", function);
        auto* afterBlock = llvm::BasicBlock::Create(*context, "cxx.drop.after", function);
        auto* live = builder->CreateLoad(builder->getInt1Ty(), namedVar.ConditionalDropFlag,
                                         "cxx.drop.livef");
        builder->CreateCondBr(live, liveBlock, afterBlock);
        builder->SetInsertPoint(liveBlock);
        EmitFullDestructorOverStorage(*builder, namedVar.Storage, namedVar.BaseType, dtor);
        builder->CreateBr(afterBlock);
        builder->SetInsertPoint(afterBlock);
    }

void LLVMBackend::DropValue(const NamedVariable& namedVar)
{
        // A `static` local's storage outlives the scope (and every later call), so scope exit must
        // not destruct it. Policy: a static local is destructed NEVER - no atexit machinery.
        if (namedVar.IsStaticLocal) return;
        if (namedVar.IsRangeForBorrow) return;
        // A plain pointer parameter can be represented by a raw pointer slot even when its
        // TypeAndValue pointer bit was stripped by an lvalue walk. It borrows the pointee and
        // must never fall through to a value destructor.
        if (!namedVar.IsOwning && namedVar.BaseType != nullptr && namedVar.BaseType->isPointerTy()) return;
        // An owning interface local owns a heap-boxed object: free it via the vtable dtor slot
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
            // A [unique] value is move-only. Its source is consumed completely, so do not run
            // the user destructor a second time after an explicit or inferred whole-value move.
            if (namedVar.IsMoved && HasTypeAnnotation(namedVar.TypeAndValue.TypeName, "unique")) return;
            // A foreign nontrivial C++ local released explicitly (`_ = move x;`) already ran its
            // C++ destructor and had its storage zeroed - running it again is a double destruction.
            // A plain `move x` (into another slot or a by-value parameter) does NOT set this flag:
            // per the M4b ruling the moved-from object is still destroyed at scope exit.
            if (namedVar.ExplicitlyMovedNull
                && (IsForeignNontrivialCxxClass(namedVar.TypeAndValue.TypeName)
                    || HasForeignNontrivialCxxField(namedVar.TypeAndValue.TypeName))
                && namedVar.ConditionalDropFlag == nullptr) return;
            // Skip the struct value being moved out via `return` - the caller now owns it.
            if (namedVar.Storage == returnedStructDtorSkipAlloca) return;
            // A fixed-array local (`T[N] a;`) owns every element - destruct all N.
            if (auto* fn = GetOrCreateFullDestructor(namedVar.TypeAndValue.TypeName))
                EmitConditionalFullDestructor(namedVar, fn);
        }
    }

bool LLVMBackend::OwnsDroppableResource(const NamedVariable& namedVar) const
{
        if (namedVar.IsStaticLocal) return false;   // never dropped; see DropValue
        if (namedVar.IsRangeForBorrow) return false;
        if (!namedVar.IsOwning && namedVar.BaseType != nullptr && namedVar.BaseType->isPointerTy()) return false;
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
        if (namedVar.ExplicitlyMovedNull
            && (IsForeignNontrivialCxxClass(namedVar.TypeAndValue.TypeName)
                || HasForeignNontrivialCxxField(namedVar.TypeAndValue.TypeName))
            && namedVar.ConditionalDropFlag == nullptr) return false;
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

        std::vector<const NamedVariable*> locals;
        locals.reserve(frame.namedVariable.size());
        for (const auto& entry : frame.namedVariable)
            locals.push_back(&entry.second);
        std::ranges::sort(locals, [](const auto* left, const auto* right) {
            return left->DeclSequence > right->DeclSequence;
        });
        for (const auto* namedVar : locals)
            if (namedVar->DeclSequence < unwindSkipSeqFloor_)
                DropValue(*namedVar);

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
                // A foreign C++ move parameter aliases the caller's moved-from object. The
                // caller owns its lifetime, so a move-constructing container store suppresses
                // this callee-side cleanup while leaving the caller's destructor intact.
                if (namedVar.ExplicitlyMovedNull
                    && (IsForeignNontrivialCxxClass(namedVar.TypeAndValue.TypeName)
                        || HasForeignNontrivialCxxField(namedVar.TypeAndValue.TypeName)))
                    continue;
                if (namedVar.IsMoved && HasTypeAnnotation(namedVar.TypeAndValue.TypeName, "unique"))
                    continue;
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

bool LLVMBackend::CalleeMayUnwind(const FunctionSymbol& symbol) const
{
        if (!cppInteropUsed_) return false;
        if (symbol.IsCxx) return !symbol.IsNoexcept;
        if (symbol.IsCInteropDeclaration) return false;
        if (symbol.CannotUnwind
            && std::none_of(symbol.NoUnwindExternDeps.begin(), symbol.NoUnwindExternDeps.end(),
                            [&](const std::string& n) { return cflatExternBodyNames_.count(n) != 0; }))
            return false;
        llvm::Function* fn = symbol.Function;
        if (fn == nullptr) return true;
        if (fn->isIntrinsic() || fn->doesNotThrow()) return false;
        // A body-less `extern` prototype names a C function (malloc, printf); a CFlat
        // function body can reach a throwing C++ callee transitively.
        return !(symbol.External && fn->isDeclaration() && !symbol.HasCFlatBody);
}

/*
 * Proves a complete core body cannot unwind: every call is direct and reaches an intrinsic, a
 * nounwind function, a C prototype without function-pointer parameters, or a core body proven
 * the same way. An indirect call, an invoke, inline asm, a C++ callee, a function whose address
 * escapes as a value (a callback) and any recursion (a cycle is never proven) all refuse.
 */
size_t LLVMBackend::InferCoreNoUnwind()
{
        std::unordered_map<const llvm::Function*, std::vector<FunctionSymbol*>> symbolsOf;
        for (auto& [key, syms] : functionTable)
            for (auto& sym : syms)
                if (sym.Function != nullptr) symbolsOf[sym.Function].push_back(&sym);

        enum class State { Visiting, Proven, Refused };
        std::unordered_map<const llvm::Function*, State> state;
        std::unordered_map<const llvm::Function*, std::set<std::string>> deps;
        std::function<bool(const llvm::Function*)> prove = [&](const llvm::Function* f) -> bool {
            if (f->isIntrinsic() || f->doesNotThrow()) return true;
            if (auto it = state.find(f); it != state.end()) return it->second == State::Proven;
            // A synthesized helper (destructor, implicit ctor) has no symbol; its body decides.
            auto symIt = symbolsOf.find(f);
            if (symIt == symbolsOf.end() && f->isDeclaration()) return false;
            if (symIt != symbolsOf.end())
                for (const FunctionSymbol* sym : symIt->second)
                    if (sym->IsCxx || !sym->IsNoexcept) return false;
            if (f->isDeclaration())
            {
                // A body-less C prototype; a function-pointer parameter may call back into CFlat.
                for (const FunctionSymbol* sym : symIt->second)
                {
                    if (!sym->External || sym->HasCFlatBody) return false;
                    for (const auto& p : sym->Parameters)
                        if (p.IsFunctionPointer) return false;
                }
                deps[f] = { f->getName().str() };
                return true;
            }
            state[f] = State::Visiting;
            bool ok = true;
            for (const auto& bb : *f)
            {
                for (const auto& inst : bb)
                {
                    const auto* call = llvm::dyn_cast<llvm::CallBase>(&inst);
                    if (call != nullptr)
                    {
                        const llvm::Function* callee = call->getCalledFunction();
                        if (llvm::isa<llvm::InvokeInst>(call) || call->isInlineAsm()
                            || callee == nullptr || !prove(callee))
                            ok = false;
                        else if (auto d = deps.find(callee); d != deps.end())
                        {
                            const std::set<std::string> calleeDeps = d->second;
                            deps[f].insert(calleeDeps.begin(), calleeDeps.end());
                        }
                    }
                    for (const auto& op : inst.operands())
                    {
                        if (call != nullptr && &op == &call->getCalledOperandUse()) continue;
                        if (llvm::isa<llvm::Function>(op.get()->stripPointerCasts())) ok = false;
                    }
                    if (!ok) break;
                }
                if (!ok) break;
            }
            state[f] = ok ? State::Proven : State::Refused;
            return ok;
        };

        size_t marked = 0;
        for (auto& [fn, syms] : symbolsOf)
        {
            if (fn->isDeclaration() || !prove(fn)) continue;
            const auto d = deps.find(fn);
            for (FunctionSymbol* sym : syms)
            {
                sym->CannotUnwind = true;
                if (d != deps.end())
                    sym->NoUnwindExternDeps.assign(d->second.begin(), d->second.end());
            }
            ++marked;
        }
        return marked;
}

void LLVMBackend::NoteCFlatExternBody(const std::string& functionName, const std::string& linkageName)
{
        cflatExternBodyNames_.insert(linkageName);
        auto it = functionTable.find(functionName);
        if (it == functionTable.end()) return;
        for (auto& sym : it->second)
            if (sym.External && !sym.IsCxx && !sym.IsCInteropDeclaration && sym.Function != nullptr
                && sym.Function->getName() == linkageName)
                sym.HasCFlatBody = true;
}

uint64_t LLVMBackend::UnwindInitFloorForDepth(size_t depth) const
{
        for (auto it = unwindInitFloors_.rbegin(); it != unwindInitFloors_.rend(); ++it)
            if (it->first == depth) return it->second;
        return UINT64_MAX;
}

bool LLVMBackend::UnwindTempConsumedByCall(llvm::Value* v) const
{
        return v != nullptr && std::find(unwindCallConsumedTemps_.begin(),
                                         unwindCallConsumedTemps_.end(), v)
                                   != unwindCallConsumedTemps_.end();
}

void LLVMBackend::NoteUnwindPartial(UnwindPartialEntry::Kind kind, llvm::Value* v,
                                    const std::string& typeName, uint64_t allocAlign,
                                    llvm::Value* count)
{
        if (!cppInteropUsed_ || v == nullptr || builder->GetInsertBlock() == nullptr) return;
        // Only what the pad would actually release: otherwise the entry just turns later calls
        // into invokes with an empty pad.
        // A finished fixed-array field value is destroyed element by element.
        if (kind == UnwindPartialEntry::Kind::Value
            && ((!v->getType()->isStructTy() && !v->getType()->isArrayTy())
                || !HasNonTrivialDestructor(typeName)))
            return;
        if (kind == UnwindPartialEntry::Kind::Members
            && GetOrCreateFullDestructor(typeName, /*membersOnly=*/true) == nullptr)
            return;
        if (kind == UnwindPartialEntry::Kind::Slot && !HasNonTrivialDestructor(typeName)) return;
        unwindPartial_.push_back({ kind, v, typeName, builder->GetInsertBlock()->getParent(),
                                   allocAlign, count });
}

void LLVMBackend::EmitUnwindPartialRelease(const UnwindPartialEntry& e)
{
        switch (e.K)
        {
        case UnwindPartialEntry::Kind::Value:
        {
            llvm::Function* dtor = GetOrCreateFullDestructor(e.TypeName);
            if (dtor == nullptr) return;
            // The field's finished value is an SSA aggregate; destroy it through a spill.
            auto* tmp = AllocaAtEntry(e.V->getType(), nullptr, "unwind.part");
            builder->CreateStore(e.V, tmp);
            if (e.V->getType()->isArrayTy())
            {
                llvm::Type* elemTy = nullptr;
                const uint64_t n = PeelFixedArrayType(e.V->getType(), elemTy);
                EmitUnwindPartialRelease({ UnwindPartialEntry::Kind::ArrayPrefix, tmp, e.TypeName,
                                           e.Fn, 0, builder->getInt64(n), elemTy });
                return;
            }
            builder->CreateCall(dtor->getFunctionType(), dtor, { tmp });
            return;
        }
        case UnwindPartialEntry::Kind::ArrayPrefix:
        {
            llvm::Function* dtor = GetOrCreateFullDestructor(e.TypeName);
            if (dtor == nullptr || e.Count == nullptr || e.ElemTy == nullptr) return;
            auto destroyAt = [&](llvm::Value* index) {
                builder->CreateCall(dtor->getFunctionType(), dtor,
                    { builder->CreateInBoundsGEP(e.ElemTy, e.V, { index }, "unwind.arrelem") });
            };
            auto* constCount = llvm::dyn_cast<llvm::ConstantInt>(e.Count);
            if (constCount != nullptr && constCount->getZExtValue() <= kMaxUnrolledArrayElements)
            {
                for (uint64_t i = constCount->getZExtValue(); i > 0; --i)
                    destroyAt(builder->getInt64(i - 1));
                return;
            }
            // Newest first: i = count-1 down to 0.
            auto* i64Ty = builder->getInt64Ty();
            llvm::Value* total = builder->CreateZExtOrTrunc(e.Count, i64Ty, "unwind.arrn");
            auto* fn = builder->GetInsertBlock()->getParent();
            auto* preBB = builder->GetInsertBlock();
            auto* loopBB = llvm::BasicBlock::Create(*context, "unwind.arrloop", fn);
            auto* doneBB = llvm::BasicBlock::Create(*context, "unwind.arrdone", fn);
            builder->CreateCondBr(builder->CreateICmpNE(total, builder->getInt64(0)), loopBB, doneBB);
            builder->SetInsertPoint(loopBB);
            auto* idx = builder->CreatePHI(i64Ty, 2, "unwind.arri");
            idx->addIncoming(total, preBB);
            auto* prev = builder->CreateSub(idx, builder->getInt64(1), "unwind.arrprev");
            destroyAt(prev);
            idx->addIncoming(prev, builder->GetInsertBlock());
            builder->CreateCondBr(builder->CreateICmpNE(prev, builder->getInt64(0)), loopBB, doneBB);
            builder->SetInsertPoint(doneBB);
            return;
        }
        case UnwindPartialEntry::Kind::Slot:
            if (llvm::Function* dtor = GetOrCreateFullDestructor(e.TypeName))
                builder->CreateCall(dtor->getFunctionType(), dtor, { e.V });
            return;
        case UnwindPartialEntry::Kind::Members:
            if (llvm::Function* dtor = GetOrCreateFullDestructor(e.TypeName, /*membersOnly=*/true))
                builder->CreateCall(dtor->getFunctionType(), dtor, { e.V });
            return;
        case UnwindPartialEntry::Kind::CxxHeap:
            EmitCxxHeapFree(e.TypeName, e.V);
            return;
        case UnwindPartialEntry::Kind::CxxHeapArray:
            EmitCxxHeapFreeArray(e.TypeName, e.V, e.Count, e.AllocAlign);
            return;
        case UnwindPartialEntry::Kind::CflatHeap:
        {
            // Free only: the constructor never returned, so there is no object to destroy.
            uint64_t effAlign = e.AllocAlign;
            TypeAndValue tv{ .TypeName = e.TypeName };
            if (llvm::Type* t = GetType(tv); t != nullptr && t->isSized())
                effAlign = std::max(effAlign, GetEffectiveAlignmentForType(e.TypeName, t));
            llvm::Function* del = effAlign > kDefaultNewAlign
                ? GetFunction("__delete_aligned") : GetFunction("operator delete");
            if (del != nullptr)
                builder->CreateCall(del->getFunctionType(), del,
                    { builder->CreateBitCast(e.V, cflat_llvm::PointerTo(builder->getInt8Ty())) });
            return;
        }
        }
}

void LLVMBackend::NoteUnwindArrayPrefix(llvm::Value* base, llvm::Type* elemTy, llvm::Value* count,
                                        const std::string& elemTypeName)
{
        if (!cppInteropUsed_ || base == nullptr || elemTy == nullptr || count == nullptr
            || builder->GetInsertBlock() == nullptr)
            return;
        // Element 0 has nothing built before it: no entry, so no invoke with an empty pad.
        if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(count); c != nullptr && c->isZero()) return;
        if (!HasNonTrivialDestructor(elemTypeName)) return;
        unwindPartial_.push_back({ UnwindPartialEntry::Kind::ArrayPrefix, base, elemTypeName,
                                   builder->GetInsertBlock()->getParent(), 0, count, elemTy });
}

void LLVMBackend::EmitArrayConstructionWalk(llvm::Value* base, llvm::Type* elemTy, uint64_t n,
                                            const std::string& elemTypeName,
                                            const std::function<void(llvm::Value*)>& emitElem)
{
        EmitFixedArrayElementWalk(*builder, base, elemTy, n,
            [&](llvm::Value* elemPtr, llvm::Value* index) {
                UnwindPartialScope builtElements(*this);
                NoteUnwindArrayPrefix(base, elemTy, index, elemTypeName);
                emitElem(elemPtr);
            });
}

bool LLVMBackend::FrameOwesUnwindCleanup()
{
        auto* insertBlock = builder->GetInsertBlock();
        llvm::Function* fn = insertBlock->getParent();
        for (const auto& e : unwindPartial_)
            if (e.Fn == fn) return true;
        if (fn != currentFunction) return false;
        {
            std::optional<llvm::DominatorTree> domTree;
            for (const auto& t : pendingOwnedStructTemps)
                if (t.Alloca != nullptr && OwnedTempDominatesHere(t.Block, insertBlock, domTree))
                    return true;
            for (const auto& [v, bb] : pendingOwnedStringTemps)
                if (v != nullptr && !UnwindTempConsumedByCall(v)
                    && OwnedTempDominatesHere(bb, insertBlock, domTree))
                    return true;
            for (const auto& [v, bb] : pendingOwnedClosureTemps)
                if (v != nullptr && !UnwindTempConsumedByCall(v)
                    && OwnedTempDominatesHere(bb, insertBlock, domTree))
                    return true;
            for (const auto& t : pendingOwnedPtrTemps)
                if (t.Value != nullptr && !UnwindTempConsumedByCall(t.Value)
                    && OwnedTempDominatesHere(t.Block, insertBlock, domTree))
                    return true;
        }
        for (auto it = stackNamedVariable.rbegin(); it != stackNamedVariable.rend(); ++it)
        {
            if (!it->lockCleanups.empty()) return true;
            const uint64_t floor = UnwindInitFloorForDepth((size_t)(stackNamedVariable.rend() - it));
            for (const auto& [name, nv] : it->namedVariable)
                if (nv.DeclSequence < floor && OwnsDroppableResource(nv)) return true;
            for (const auto& [name, nv] : it->functionArgument)
            {
                if (nv.Storage == nullptr) continue;
                if (nv.TypeAndValue.IsFatInterfaceValue() ? IsOwningInterfaceValue(nv)
                        : (nv.IsOwning || nv.IsOwningString || nv.IsOwningStruct))
                    return true;
            }
            if (it->isFunction) break;
        }
        return false;
}

llvm::CallBase* LLVMBackend::CreateCallOrInvoke(llvm::FunctionType* fnTy, llvm::Value* callee,
                                                llvm::ArrayRef<llvm::Value*> args, bool mayUnwind,
                                                const llvm::Twine& name)
{
        if (!mayUnwind || !cppInteropUsed_ || emittingUnwindCleanup_ || !IsInsertBlockLive()
            || currentFunction == nullptr)
            return builder->CreateCall(fnTy, callee, args, name);
        // A synthesized helper emitted out of line (a struct copy) owes only its partial
        // construction; the scope walk and statement temps belong to currentFunction.
        llvm::Function* fn = builder->GetInsertBlock()->getParent();
        const bool ownFrame = fn == currentFunction;
        llvm::Function* personality = GetTargetEhPersonality();
        if (personality == nullptr
            || (fn->hasPersonalityFn() && fn->getPersonalityFn() != personality)
            || !FrameOwesUnwindCleanup())
            return builder->CreateCall(fnTy, callee, args, name);

        auto* padBB = llvm::BasicBlock::Create(*context, "unwind.cleanup", fn);
        auto* contBB = llvm::BasicBlock::Create(*context, "invoke.cont", fn);
        auto* invoke = builder->CreateInvoke(fnTy, callee, contBB, padBB, args, name);
        if (!fn->hasPersonalityFn()) fn->setPersonalityFn(personality);

        const llvm::DebugLoc savedLoc = builder->getCurrentDebugLocation();
        builder->SetInsertPoint(padBB);
        emittingUnwindCleanup_ = true;
        // A diagnostic thrown mid-pad must not leave normal scope exits skipping locals.
        struct PadStateReset
        {
            LLVMBackend& b;
            ~PadStateReset() { b.emittingUnwindCleanup_ = false; b.unwindSkipSeqFloor_ = UINT64_MAX; }
        } padStateReset{ *this };
        const bool funclet = targetWindows_;
        llvm::Value* pad = nullptr;
        if (funclet)
            pad = builder->CreateCleanupPad(llvm::ConstantTokenNone::get(*context), {}, "unwind.pad");
        else
        {
            auto* lpTy = llvm::StructType::get(*context, { builder->getPtrTy(), builder->getInt32Ty() });
            auto* lp = builder->CreateLandingPad(lpTy, 0, "unwind.lp");
            lp->setCleanup(true);
            pad = lp;
        }
        // The object under construction first (newest entry first), as C++ unwinds a
        // constructor's finished subobjects before the full-expression's temporaries.
        for (auto it = unwindPartial_.rbegin(); it != unwindPartial_.rend(); ++it)
            if (it->Fn == fn && IsInsertBlockLive()) EmitUnwindPartialRelease(*it);
        if (ownFrame)
        {
            // Then the statement's temps still OWNED at the invoke: in FlushOwnedStructTemps'
            // ledger order, and never an argument this call hands to a sink parameter.
            auto* invokeBlock = invoke->getParent();
            for (const auto& t : pendingOwnedStructTemps)
            {
                std::optional<llvm::DominatorTree> domTree;
                if (t.Alloca == nullptr || !IsInsertBlockLive()) continue;
                if (!OwnedTempDominatesHere(t.Block, invokeBlock, domTree)) continue;
                EmitOwnedStructTempFree(t);
            }
            {
                std::optional<llvm::DominatorTree> domTree;
                for (const auto& [v, bb] : pendingOwnedStringTemps)
                    if (v != nullptr && IsInsertBlockLive() && !UnwindTempConsumedByCall(v)
                        && OwnedTempDominatesHere(bb, invokeBlock, domTree))
                        EmitOwnedStringTempFree(v);
                for (const auto& [v, bb] : pendingOwnedClosureTemps)
                    if (v != nullptr && IsInsertBlockLive() && !UnwindTempConsumedByCall(v)
                        && OwnedTempDominatesHere(bb, invokeBlock, domTree))
                        EmitOwnedClosureTempFree(v);
            }
            for (const auto& t : pendingOwnedPtrTemps)
            {
                std::optional<llvm::DominatorTree> domTree;
                if (t.Value == nullptr || !IsInsertBlockLive() || UnwindTempConsumedByCall(t.Value))
                    continue;
                if (!OwnedTempDominatesHere(t.Block, invokeBlock, domTree)) continue;
                if (t.ConditionalSlot != nullptr) EmitOwnedConditionalPtrTempFree(t);
                else EmitOwnedPtrTempFree(t.Value, t.TypeName, t.AllocAlign, t.RawArrayCount, t.ReleaseGate);
            }
            for (auto it = stackNamedVariable.rbegin(); it != stackNamedVariable.rend(); ++it)
            {
                unwindSkipSeqFloor_ = UnwindInitFloorForDepth((size_t)(stackNamedVariable.rend() - it));
                EmitDestructorsForScope(*it);
                if (it->isFunction) break;
            }
        }
        if (IsInsertBlockLive())
        {
            if (funclet)
                builder->CreateCleanupRet(llvm::cast<llvm::CleanupPadInst>(pad));
            else
                builder->CreateResume(pad);
        }
        builder->SetInsertPoint(contBB);
        builder->SetCurrentDebugLocation(savedLoc);
        return invoke;
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
        // An LSP slot replaced after a crash skipped its analysis' scope guards (SEH unwinds no
        // destructors): release its header-cache root or it stays in progress forever.
        EndActiveRoot();

        builder.release();
        module.release();

        // context is last to be released.
        context.release();
    }
