// Pure data model and aggregation for cflat/optimizationInfo. Deliberately free of any
// `vscode` import so it can be unit tested directly: everything here is a function of the
// server's response, and none of it needs an editor.
//
// `@vscode/l10n` is the standalone bundle loader, NOT the `vscode` module - it runs in plain
// node, and with no bundle configured `t()` returns the source string, so the unit tests see
// the English text unchanged. extension.ts hands it the editor's bundle at activation.
import * as l10n from '@vscode/l10n';

// One entry per source function, as returned by cflat/optimizationInfo. A zero counter
// means "not available" rather than "measured as zero", so zeros are omitted from the lens.
export interface OptFunctionInfo {
    name: string;
    symbol: string;
    startLine: number;
    endLine: number;
    irInstructions: number;
    machineInstructions: number;
    bytes: number;
    stackBytes: number;
    spills: number;
    reloads: number;
    inlinedInto: number;
    eliminated: boolean;
}

// One LLVM optimization remark. `calleeName` / `calleeLine` are resolved server-side:
// `args.Callee` carries the MANGLED symbol, and mangled names must never be parsed here.
// A calleeLine of 0 means the callee is not defined in this file (printf, core library).
export interface OptRemark {
    pass: string;
    name: string;
    kind: string;
    message: string;
    function: string;
    srcLine: number;
    calleeName: string;
    calleeLine: number;
    args: Record<string, string>;
}

export interface OptInfoResult {
    optLevel: number;
    functions: OptFunctionInfo[];
    remarks?: OptRemark[];
    remarksTruncated?: boolean;
}

export interface OptInfoCacheEntry {
    key: string;
    optLevel: number;
    functions: OptFunctionInfo[];
    remarks: OptRemark[];
}

// Translator hints ride along in the `comment` attribute of each l10n.t call. Two rules,
// both learned from the extractor silently dropping them: a plain // comment above the call
// is never harvested, and every array entry must be a LITERAL string - a shared constant or
// an 'a' + 'b' concatenation is discarded without warning. Hence the repetition below.
//
// vscode.l10n does plain {N} substitution with no ICU plural selection, so every counted
// noun needs its singular and plural spelled out as separate localizable strings. Two forms
// is what the platform affords; a language needing more cannot be expressed here.
export function countCallSites(count: number): string {
    return count === 1
        ? l10n.t({
            message: '1 call site',
            comment: ['A source location where a function is called, not a website.']
        })
        : l10n.t({
            message: '{0} call sites',
            args: [count],
            comment: ['Source locations where a function is called, not websites.']
        });
}

export function countFunctions(count: number): string {
    return count === 1
        ? l10n.t({
            message: '1 function',
            comment: ['Counts the functions a body was copied into by inlining.']
        })
        : l10n.t({
            message: '{0} functions',
            args: [count],
            comment: ['Counts the functions a body was copied into by inlining.']
        });
}

export function countInstructions(count: number): string {
    return count === 1
        ? l10n.t({
            message: '1 instr',
            comment: ['Abbreviation of "machine instruction".',
                'Keep the abbreviation short - it sits in a one-line CodeLens.']
        })
        : l10n.t({
            message: '{0} instr',
            args: [count],
            comment: ['Abbreviation of "machine instructions".',
                'Keep the abbreviation short - it sits in a one-line CodeLens.']
        });
}

export function countSpills(count: number): string {
    return count === 1
        ? l10n.t({
            message: '1 spill',
            comment: ['Register allocation term: a value the compiler had to write out to the stack because no CPU register was free.',
                'Use the established term in your language, not "spill" as in a spilled liquid.']
        })
        : l10n.t({
            message: '{0} spills',
            args: [count],
            comment: ['Register allocation term: values the compiler had to write out to the stack because no CPU register was free.',
                'Use the established term in your language, not "spill" as in a spilled liquid.']
        });
}

export function countReloads(count: number): string {
    return count === 1
        ? l10n.t({
            message: '1 reload',
            comment: ['Register allocation term: reading a spilled value back from the stack into a CPU register.',
                'The counterpart of "spill".']
        })
        : l10n.t({
            message: '{0} reloads',
            args: [count],
            comment: ['Register allocation term: reading spilled values back from the stack into CPU registers.',
                'The counterpart of "spill".']
        });
}

// One rendered summand of the inline cost model.
interface InlineCostContributor {
    order: number;       // canonical position, used as the tie-break
    label: string;
    reading?: string;    // "~338 instructions" on the instruction summand
    value: number;       // credits are already negated
}

// The additive summands of LLVM's inline cost, in canonical order, keyed by the LLVM
// feature names the server forwards in `args`. Several LLVM features describe the same
// user-visible thing (the four switch-lowering penalties, the two argument-setup costs)
// and are summed into one line. `credit: true` means the feature REDUCES the cost, so it
// is displayed negated.
//
// Context counts - num_loops, dead_blocks, simplified_instructions, constant_args,
// constant_offset_ptr_args, is_multiple_blocks, nested_inlines, nested_inline_cost_estimate
// and the lowercase `threshold` - are deliberately absent: they are not summands of the
// cost and are reserved for a later tooltip pass.
const INLINE_COST_CONTRIBUTORS: { keys: string[]; credit?: boolean; instructions?: boolean; label: () => string }[] = [
    {
        keys: ['unsimplified_common_instructions'], instructions: true,
        label: () => l10n.t({
            message: 'Instructions',
            comment: ['Machine/IR instructions in the called function that the optimizer could not fold away.',
                'Appears in a comma-separated list of what made a call expensive to inline.']
        })
    },
    {
        keys: ['call_penalty'],
        label: () => l10n.t({
            message: 'Calls inside callee',
            comment: ['Cost charged for the calls the called function itself makes.',
                'Appears in a comma-separated list of what made a call expensive to inline.']
        })
    },
    {
        keys: ['switch_penalty', 'jump_table_penalty', 'case_cluster_penalty', 'switch_default_dest_penalty'],
        label: () => l10n.t({
            message: 'Switch lowering',
            comment: ['Cost of turning a switch statement into jump tables and comparison chains.',
                'Appears in a comma-separated list of what made a call expensive to inline.']
        })
    },
    {
        keys: ['call_argument_setup', 'lowered_call_arg_setup'],
        label: () => l10n.t({
            message: 'Argument setup',
            comment: ['Cost of preparing the arguments passed to a call.',
                'Appears in a comma-separated list of what made a call expensive to inline.']
        })
    },
    {
        keys: ['indirect_call_penalty'],
        label: () => l10n.t({
            message: 'Indirect calls',
            comment: ['Cost charged for calls through a function pointer, whose target is not known.',
                'Appears in a comma-separated list of what made a call expensive to inline.']
        })
    },
    {
        keys: ['cold_cc_penalty'],
        label: () => l10n.t({
            message: 'Cold calling convention',
            comment: ['Cost charged because the called function uses the calling convention for rarely executed code.',
                'Appears in a comma-separated list of what made a call expensive to inline.']
        })
    },
    {
        keys: ['load_relative_intrinsic'],
        label: () => l10n.t({
            message: 'Relative loads',
            comment: ['Cost of loads made through a relative offset, as used by virtual call tables.',
                'Appears in a comma-separated list of what made a call expensive to inline.']
        })
    },
    {
        keys: ['sroa_savings'], credit: true,
        label: () => l10n.t({
            message: 'Stack promotion savings',
            comment: ['A saving, shown as a negative number: inlining would let the optimizer keep a stack variable in registers.',
                'Appears in a comma-separated list of what made a call expensive to inline.']
        })
    },
    {
        keys: ['load_elimination'], credit: true,
        label: () => l10n.t({
            message: 'Eliminated loads',
            comment: ['A saving, shown as a negative number: inlining would remove loads from memory.',
                'Appears in a comma-separated list of what made a call expensive to inline.']
        })
    },
    {
        // LLVM already records this one negative; shown as-is, not negated.
        keys: ['callsite_cost'],
        label: () => l10n.t({
            message: 'Call overhead removed',
            comment: ['A saving, shown as a negative number: inlining removes the cost of performing the call itself.',
                'Appears in a comma-separated list of what made a call expensive to inline.']
        })
    },
];

// At most this many summands are listed; the rest collapse into a "+n more".
const MAX_INLINE_COST_CONTRIBUTORS = 6;

// Non-negative integer value of one LLVM feature arg, or 0 when absent or unparseable.
function inlineFeature(args: Record<string, string> | undefined, key: string): number {
    const raw = args?.[key];
    if (raw === undefined) return 0;
    const value = Number(raw);
    return Number.isInteger(value) ? value : 0;
}

// True when the server sent the inline cost breakdown for this remark. Old servers send
// only Cost/Threshold, and those alone must render exactly as they did before.
function hasInlineCostBreakdown(args: Record<string, string> | undefined): boolean {
    if (!args) return false;
    for (const entry of INLINE_COST_CONTRIBUTORS)
        for (const key of entry.keys)
            if (inlineFeature(args, key) !== 0) return true;
    return inlineFeature(args, 'sroa_losses') !== 0
        || inlineFeature(args, 'last_call_to_static_bonus') !== 0;
}

// Renders the cost breakdown that follows an inline decision, or undefined when the remark
// carries no breakdown. Pure - exported so it can be unit tested without a server.
//
// `inlined` picks the shape: a call that WAS inlined only gets its saving, because there
// is nothing to act on; a call that was rejected gets the contributors that sank it.
//
// Sign: an inlined call reports its SAVING (LLVM's cost negated; positive = it paid off). A
// rejected call reports its COST, with the contributors signed to match: costs positive,
// credits negative. Ordering: the cap keeps the summands with the largest MAGNITUDE, then
// the kept ones print from the biggest cost down to the biggest credit.
export interface InlineCostBreakdownParts {
    headline: string;    // "(weight 320, threshold 250)"
    cost: number;
    threshold: number;
    instructions: number;  // the raw instruction summand, 0 when absent
    items: string[];     // "Instructions: 240", ..., possibly ending in "+n more"
    notes: string[];     // whole sentences: missed savings, threshold bonus
}

// LLVM charges this many cost units per IR instruction (InlineConstants::getInstrCost(),
// the `inline-instr-cost` default), so cost / 5 is roughly "how many instructions".
const INLINE_INSTR_COST = 5;

export function approxInstructions(cost: number): string {
    return l10n.t({
        message: '~{0} instructions',
        args: [Math.round(cost / INLINE_INSTR_COST)],
        comment: ['An approximate instruction count derived from an inline cost. Keep the leading ~ (about).']
    });
}


export function inlineCostBreakdownParts(
    args: Record<string, string> | undefined, inlined: boolean): InlineCostBreakdownParts | undefined {
    if (!hasInlineCostBreakdown(args)) return undefined;
    const cost = Number(args?.Cost);
    const threshold = Number(args?.Threshold);
    if (!Number.isInteger(cost) || !Number.isInteger(threshold)) return undefined;

    const instructions = inlineFeature(args, 'unsimplified_common_instructions');
    const notes: string[] = [];
    const bonus = inlineFeature(args, 'last_call_to_static_bonus');
    if (bonus !== 0) notes.push(l10n.t({
        message: 'Includes the last-call-to-static bonus: the function is deleted after inlining.',
        comment: ['Explains a very large saving: this was the only remaining call to a function private to the file.']
    }));

    const headline = l10n.t({
        message: '(weight {0}, threshold {1})',
        args: [cost, threshold],
        comment: ['Follows "inlined" or "not inlined - too costly". {0} is the inline weight LLVM computed for the call (can be negative) and {1} the budget it had to stay under.',
            'Both are plain integers with no unit.']
    });
    if (inlined) return { headline, cost, threshold, instructions, items: [], notes };

    const contributors: InlineCostContributor[] = [];
    for (let order = 0; order < INLINE_COST_CONTRIBUTORS.length; order++) {
        const entry = INLINE_COST_CONTRIBUTORS[order];
        let sum = 0;
        for (const key of entry.keys) sum += inlineFeature(args, key);
        if (sum === 0) continue;
        contributors.push({ order, label: entry.label(), value: entry.credit ? -sum : sum,
            reading: entry.instructions ? approxInstructions(sum) : undefined });
    }
    const ranked = [...contributors].sort((a, b) => Math.abs(b.value) - Math.abs(a.value));
    const shown = ranked.slice(0, MAX_INLINE_COST_CONTRIBUTORS);
    const hidden = ranked.length - shown.length;
    shown.sort((a, b) => (b.value - a.value) || (a.order - b.order));

    const items = shown.map(item => item.reading === undefined
        ? l10n.t({
            message: '{0}: {1}',
            args: [item.label, item.value],
            comment: ['One cost contributor: {0} is its label, e.g. "Switch lowering", {1} a plain integer.']
        })
        : l10n.t({
            message: '{0}: {1} ({2})',
            args: [item.label, item.value, item.reading],
            comment: ['The instruction contributor: {0} is its label, {1} a plain integer, {2} a reading such as "~338 instructions".']
        }));
    if (hidden > 0) items.push(l10n.t({
        message: '+{0} more',
        args: [hidden],
        comment: ['Ends a truncated list of cost contributors. {0} is how many were left out.']
    }));
    const missed = inlineFeature(args, 'sroa_losses');
    if (missed !== 0) notes.push(l10n.t({
        message: 'Missed reduction: {0} (argument address escapes)',
        args: [missed],
        comment: ['{0} is a weight reduction the optimizer could not take because the address of an argument was used, so the value could not be kept in registers.',
            'A plain integer with no unit.']
    }));
    return { headline, cost, threshold, instructions, items, notes };
}

// One-line form of the parts above, for list contexts.
export function describeInlineCostBreakdown(
    args: Record<string, string> | undefined, inlined: boolean): string | undefined {
    const parts = inlineCostBreakdownParts(args, inlined);
    if (parts === undefined) return undefined;
    if (inlined) return parts.headline;
    const cost = Number(args?.Cost);
    const threshold = Number(args?.Threshold);
    const joinList = (a: string, b: string) => l10n.t({
        message: '{0}, {1}',
        args: [a, b],
        comment: ['Joins two entries of a comma-separated list.']
    });
    const more = parts.items.length > 0 && parts.items[parts.items.length - 1].startsWith('+')
        ? parts.items[parts.items.length - 1] : undefined;
    const entries = more === undefined ? parts.items : parts.items.slice(0, -1);
    let list = entries.length === 0 ? '' : entries.reduce(joinList);
    if (more !== undefined) list = l10n.t({
        message: '{0} {1}',
        args: [list, more],
        comment: ['Appends a "+n more" tail to a list.']
    });

    const sentences: string[] = [list !== ''
        ? l10n.t({
            message: '(weight {0}, threshold {1}): {2}.',
            args: [cost, threshold, list],
            comment: ['Follows "not inlined - too costly". {0} is the inline weight LLVM computed for the call and {1} the budget it had to stay under.',
                '{2} is an already-formatted list such as "Instructions: 240, Argument setup: 14".',
                'Both numbers are plain integers with no unit.']
        })
        : l10n.t({
            message: '(weight {0}, threshold {1}).',
            args: [cost, threshold],
            comment: ['Follows "not inlined - too costly". {0} is the inline weight LLVM computed for the call and {1} the budget it had to stay under.',
                'Both are plain integers with no unit.']
        })];

    sentences.push(...parts.notes);
    return sentences.join(' ');
}

// Human-readable form of one inline decision. The Cost and Threshold LLVM reports in `args`
// are shown ONLY when the server also sent the cost breakdown that explains them: the bare
// numbers are unitless internals of the inline cost model, and knowing a call missed by 258
// units tells the user nothing they can act on. With the breakdown behind them they become
// actionable, so an older server - which sends no breakdown - still renders outcome only.
export function describeInlineDecision(remark: OptRemark): string {
    const breakdown = describeInlineCostBreakdown(remark.args, remark.name === 'Inlined');
    const outcome = describeInlineOutcome(remark);
    if (breakdown === undefined || !inlineOutcomeHasNumbers(remark)) return outcome;
    return l10n.t({
        message: '{0} {1}',
        args: [outcome, breakdown],
        comment: ['{0} is a verdict such as "inlined", {1} its detail such as "(weight 35, threshold 250)".']
    });
}

// A never-inline verdict carries no cost model numbers worth showing.
function inlineOutcomeHasNumbers(remark: OptRemark): boolean {
    return remark.name !== 'NeverInline';
}

// Why a call was rejected, without the "not inlined" prefix: "too costly", "never - noinline
// function attribute", ...
export function describeInlineReason(remark: OptRemark): string {
    switch (remark.name) {
        case 'TooCostly': return l10n.t({
            message: 'too costly',
            comment: ['The optimizer judged the function too large to copy into the caller.']
        });
        case 'NeverInline': return remark.args?.Reason
            ? l10n.t({
                message: 'never - {0}',
                args: [remark.args.Reason],
                comment: ['{0} is an English reason supplied by the LLVM optimizer and cannot be translated - leave the placeholder alone.']
            })
            : l10n.t({
                message: 'never',
                comment: ['The function can never be inlined, whatever its size.']
            });
        default: return remark.name.toLowerCase();
    }
}

// The verdict alone: "inlined", "not inlined - too costly", ...
export function describeInlineOutcome(remark: OptRemark): string {
    // `Reason` and `remark.name` come from LLVM in English and are passed through as data.
    switch (remark.name) {
        case 'Inlined': return (l10n.t({
            message: 'inlined',
            comment: ['The optimizer replaced a call with a copy of the called function\'s body.']
        }));
        case 'TooCostly': return (l10n.t({
            message: 'not inlined - too costly',
            comment: ['The optimizer judged the function too large to copy into the caller.']
        }));
        case 'NeverInline': return remark.args?.Reason
            ? l10n.t({
                message: 'never inlined - {0}',
                args: [remark.args.Reason],
                comment: ['{0} is an English reason supplied by the LLVM optimizer and cannot be translated - leave the placeholder alone.']
            })
            : l10n.t({
                message: 'never inlined',
                comment: ['The function can never be inlined, whatever its size.']
            });
        default: return (l10n.t({
            message: 'not inlined - {0}',
            args: [remark.name.toLowerCase()],
            comment: ['{0} is an English reason supplied by the LLVM optimizer and cannot be translated - leave the placeholder alone.']
        }));
    }
}

// The inline decisions made on one source line about one callee, as markdown lines for the
// hover shown on the call itself. LLVM reports no column, so two calls to the same function
// on one line land on the same remark line; and a caller that was itself inlined elsewhere
// gets its calls evaluated again in every copy. Identical verdicts are therefore collapsed
// into one block; only genuinely different decisions are listed separately.
export function buildCallSiteHover(entry: OptInfoCacheEntry, line: number, calleeName: string): string[] {
    const remarks = entry.remarks.filter(remark => remark.pass === 'inline'
        && remark.srcLine === line && remark.calleeName === calleeName);
    if (remarks.length === 0) return [];
    const unique = new Map<string, OptRemark>();
    for (const remark of remarks) {
        const key = describeInlineDecision(remark);
        if (!unique.has(key)) unique.set(key, remark);
    }

    const lines: string[] = [];
    for (const remark of unique.values()) {
        if (lines.length > 0) lines.push('');
        const parts = inlineOutcomeHasNumbers(remark)
            ? inlineCostBreakdownParts(remark.args, remark.name === 'Inlined') : undefined;
        // "inlined (saving 35)" / "not inlined - too costly (cost 320, threshold 250)" become
        // "**Inlined** at -O2 (saving 35)" / "**Not inlined** at -O2: too costly (cost ...)".
        const inlinedHeading = () => parts === undefined
            ? l10n.t({
                message: '**Inlined** at -O{0}',
                args: [entry.optLevel],
                comment: ['Heading of the hover shown on a call the optimizer inlined.',
                    'Rendered as Markdown: ** ** makes text bold - keep the markers attached to the text they wrap.',
                    '-O0 / -O1 / -O2 is a compiler optimization-level flag. Never translate it and never separate the O from its digit.']
            })
            : l10n.t({
                message: '**Inlined** at -O{0} {1}',
                args: [entry.optLevel, parts.headline],
                comment: ['Heading of the hover shown on a call the optimizer inlined. {1} is a detail such as "(weight -35, threshold 337)".',
                    'Rendered as Markdown: ** ** makes text bold - keep the markers attached to the text they wrap.',
                    '-O0 / -O1 / -O2 is a compiler optimization-level flag. Never translate it and never separate the O from its digit.']
            });
        const rejectedHeading = () => parts === undefined
            ? l10n.t({
                message: '**Not inlined** at -O{0}: {1}',
                args: [entry.optLevel, describeInlineReason(remark)],
                comment: ['Heading of the hover shown on a call the optimizer rejected. {1} is the reason, e.g. "too costly".',
                    'Rendered as Markdown: ** ** makes text bold - keep the markers attached to the text they wrap.',
                    '-O0 / -O1 / -O2 is a compiler optimization-level flag. Never translate it and never separate the O from its digit.']
            })
            : l10n.t({
                message: '**Not inlined** at -O{0}: {1} {2}',
                args: [entry.optLevel, describeInlineReason(remark), parts.headline],
                comment: ['Heading of the hover shown on a call the optimizer rejected. {1} is the reason, e.g. "too costly"; {2} a detail such as "(weight 1730, threshold 225)".',
                    'Rendered as Markdown: ** ** makes text bold - keep the markers attached to the text they wrap.',
                    '-O0 / -O1 / -O2 is a compiler optimization-level flag. Never translate it and never separate the O from its digit.']
            });
        lines.push(remark.name === 'Inlined' ? inlinedHeading() : rejectedHeading());
        if (parts === undefined) continue;
        for (const item of parts.items) lines.push(l10n.t({
            message: '- {0}',
            args: [item],
            comment: ['Markdown list item - keep the leading "- ", it is what makes the list render.']
        }));
        if (parts.notes.length > 0) lines.push('', ...parts.notes);
    }
    return lines;
}

// The size and register cost of one function, as markdown lines. This is what the CodeLens
// click pops up. Inlining is deliberately NOT summarised here: the caller appends a
// "Show N call sites" link that opens the References peek, and a count in prose next to a
// link that gives the same count -- and navigates -- is pure redundancy. The per-call
// decisions live on the call sites themselves (buildCallSiteHover). The caller also appends
// the IR and Assembly links, which need a document URI this module deliberately knows
// nothing about.
export function buildFunctionDetail(entry: OptInfoCacheEntry, info: OptFunctionInfo): string[] {
    const lines = [l10n.t({
        message: '**{0}** at -O{1}',
        args: [info.name, entry.optLevel],
        comment: ['Heading of the function-detail hover. {0} is a function name.',
            'Rendered as Markdown: ** ** makes text bold - keep the markers attached to the text they wrap.',
            '-O0 / -O1 / -O2 is a compiler optimization-level flag. Never translate it and never separate the O from its digit.']
    }), ''];
    if (info.eliminated) {
        lines.push(l10n.t({
            message: 'No code emitted - the optimizer inlined or removed this function.',
            comment: ['Replaces the size counters when nothing was emitted for the function.']
        }));
    } else {
        // A zero counter means "not available", never "measured as zero", so a figure the
        // server could not produce is omitted rather than reported as 0.
        //
        // The IR instruction count is deliberately NOT reported. It is a fact about an
        // intermediate representation the user never sees, and it invites comparisons
        // against the machine code that do not hold. Machine-code bytes is the size that
        // means something to them.
        //
        // "Function size" is the size of this function's symbol in the emitted object
        // file. For a generic it is the SUM over every instantiation, because the server
        // aggregates all symbols that map back to one source function.
        //
        // The mangled symbol itself is not shown either. The heading already names the
        // function and the hover is anchored to its definition, so it answers nothing the
        // user is asking; for a generic it would actively mislead, since `symbol` holds
        // only the FIRST instantiation while `bytes` sums them all.
        if (info.bytes > 0) lines.push(l10n.t({
            message: '- Function size: {0} bytes',
            args: [info.bytes],
            comment: ['Size of this function\'s machine code in the object file.',
                'Markdown list item - keep the leading "- ", it is what makes the list render.']
        }));
        if (info.machineInstructions > 0)
            lines.push(l10n.t({
                message: '- Machine instructions: {0}',
                args: [info.machineInstructions],
                comment: ['Count of CPU instructions emitted for this function.',
                    'Markdown list item - keep the leading "- ", it is what makes the list render.']
            }));
        if (info.stackBytes > 0) lines.push(l10n.t({
            message: '- Stack frame: {0} bytes',
            args: [info.stackBytes],
            comment: ['Stack space this function reserves for its locals and spills.',
                'Markdown list item - keep the leading "- ", it is what makes the list render.']
        }));
        lines.push(l10n.t({
            message: '- Register: {0}, {1}',
            args: [countSpills(info.spills), countReloads(info.reloads)],
            comment: ['Register allocator counters; {0} and {1} arrive already formatted, e.g. "2 spills" and "1 reload".',
                'Markdown list item - keep the leading "- ", it is what makes the list render.']
        }));
    }

    const costLine = describeInlineCostRange(entry, info);
    if (costLine !== undefined) lines.push(costLine);

    // inlinedInto is derived from the module itself; the navigable call sites come from the
    // remark stream, which is capped per pass and aggregates overloads by name. So the count
    // can outlive its evidence. Say that plainly - otherwise the reader is told the body was
    // inlined somewhere and given no way to reach it, with nothing explaining the gap.
    if (info.inlinedInto > 0 && collectInlineCallSites(entry, info).length === 0)
        lines.push('', l10n.t({
            message: '_The individual call sites are not available for this function._',
            comment: ['Shown when the body was inlined somewhere but the call sites cannot be listed.',
                'Rendered as Markdown: _ _ makes text italic - keep the markers attached to the text they wrap.']
        }));

    return lines;
}

// The inline cost the optimizer saw for this function across its call sites, as one
// markdown line, or undefined when no remark carried one. The cost is per call site (constant
// arguments simplify the body differently), so a spread is shown as a range. Sites that got
// the last-call-to-static bonus are skipped: their cost has 15000 subtracted and says
// nothing about the body's size.
export function describeInlineCostRange(entry: OptInfoCacheEntry, info: OptFunctionInfo): string | undefined {
    const seen = entry.remarks
        .filter(remark => remark.pass === 'inline' && remark.calleeLine === info.startLine
            && remark.calleeName === info.name && inlineFeature(remark.args, 'last_call_to_static_bonus') === 0)
        .map(remark => ({ cost: Number(remark.args?.Cost),
                          instructions: inlineFeature(remark.args, 'unsimplified_common_instructions') }))
        .filter(item => Number.isInteger(item.cost))
        .sort((a, b) => a.cost - b.cost);
    if (seen.length === 0) return undefined;
    const format = (item: { cost: number; instructions: number }) =>
        item.instructions === 0 ? `${item.cost}` : l10n.t({
            message: '{0} ({1})',
            args: [item.cost, approxInstructions(item.instructions)],
            comment: ['{0} is an inline weight (plain integer), {1} a reading such as "~338 instructions".']
        });
    const low = seen[0];
    const high = seen[seen.length - 1];
    return low.cost === high.cost
        ? l10n.t({
            message: '- Inline weight: {0}',
            args: [format(low)],
            comment: ['What the optimizer estimates copying this function into a caller costs. {0} arrives formatted, e.g. "1730 (~338 instructions)".',
                'Markdown list item - keep the leading "- ", it is what makes the list render.']
        })
        : l10n.t({
            message: '- Inline weight: {0} to {1}',
            args: [format(low), format(high)],
            comment: ['A range of inline cost estimates across call sites; both arrive formatted, e.g. "1690 (~338 instructions)".',
                'Markdown list item - keep the leading "- ", it is what makes the list render.']
        });
}

// Every call site the optimizer made an inlining decision about, ascending by line, for
// the References peek. Duplicates are kept: two calls on one line are two decisions, and
// the peek collapses them itself.
export function collectInlineCallSites(
    entry: OptInfoCacheEntry, info: OptFunctionInfo): { srcLine: number; inlined: boolean }[] {
    return entry.remarks
        .filter(remark => remark.pass === 'inline' && remark.calleeLine === info.startLine
            && remark.calleeName === info.name)
        .map(remark => ({ srcLine: remark.srcLine, inlined: remark.name === 'Inlined' }))
        .sort((a, b) => a.srcLine - b.srcLine);
}
