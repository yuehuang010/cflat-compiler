// Pure data model and aggregation for cflat/optimizationInfo. Deliberately free of any
// `vscode` import so it can be unit tested directly: everything here is a function of the
// server's response, and none of it needs an editor.

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

export function plural(count: number, noun: string): string {
    return `${count} ${noun}${count === 1 ? '' : 's'}`;
}

// Dim monochrome marker for inlining. Deliberately not a colored or emoji glyph: this is
// ambient information sitting in the middle of the user's code, and it must not compete
// with diagnostics for attention. Only INLINING is marked - a call the optimizer left
// alone gets no marker at all, so an unmarked line means "nothing to see here".
//
// Shape constraints, all learned the hard way: a solid triangle reads as a run/debug
// button; a bare arrowhead reads as navigation; and in a C dialect the math glyphs are
// worse still - a dim trailing '+' in a circle or a subset sign reads as part of the
// expression rather than as an annotation. A diamond flanked by hyphens is none of those.
// It is not valid syntax in this position, so it cannot be misread as code.
//
// Font note: U+25C6 is in Menlo (VS Code's macOS default) but NOT in SF Mono, so a user
// who switches fonts gets a fallback face for it. U+25CA LOZENGE is the coverage-safe
// substitute if that ever matters.
export const INLINE_MARKER = '-\u25C6-';   // HYPHEN, BLACK DIAMOND, HYPHEN

// A rendered inline marker: which line it sits on, and the hover behind it.
export interface InlineAnnotation {
    line: number;        // 1-based, as the server reports it
    hover: string[];     // markdown lines
}

// Human-readable form of one inline decision. LLVM also reports a Cost and a Threshold in
// `args`, and neither is shown: the numbers are unitless internals of the inline cost model,
// and knowing a call missed by 258 units tells the user nothing they can act on. The
// outcome is the whole actionable content of a decision.
export function describeInlineDecision(remark: OptRemark): string {
    switch (remark.name) {
        case 'Inlined': return 'inlined';
        case 'TooCostly': return 'not inlined - too costly';
        case 'NeverInline': return `never inlined${remark.args?.Reason ? ` - ${remark.args.Reason}` : ''}`;
        default: return `not inlined - ${remark.name.toLowerCase()}`;
    }
}

// Split the inline-pass remarks into call-site and definition-site markers.
//
// Only calls to functions defined in THIS file are considered (calleeLine > 0). A call
// into the core library or a varargs routine produces a remark on nearly every printf,
// which would bury the signal. Exported for unit testing - it is pure.
export function buildInlineAnnotations(entry: OptInfoCacheEntry): {
    callSites: InlineAnnotation[];
    definitions: InlineAnnotation[];
} {
    const inlineRemarks = entry.remarks.filter(
        remark => remark.pass === 'inline' && remark.calleeLine > 0 && remark.srcLine > 0);

    // Call sites: one marker per line. srcColumn is always 0 in LLVM's inline remarks, so
    // the line is the finest resolution available and several calls can share one marker.
    const byLine = new Map<number, OptRemark[]>();
    for (const remark of inlineRemarks) {
        const existing = byLine.get(remark.srcLine);
        if (existing) existing.push(remark);
        else byLine.set(remark.srcLine, [remark]);
    }
    const callSites: InlineAnnotation[] = [];
    for (const [line, remarks] of [...byLine.entries()].sort((a, b) => a[0] - b[0])) {
        const inlined = remarks.filter(remark => remark.name === 'Inlined');
        if (inlined.length === 0) continue;
        const hover = [`**Inlined here** at -O${entry.optLevel}`, ''];
        for (const remark of inlined)
            hover.push(`- \`${remark.calleeName}\` ${describeInlineDecision(remark)}`);
        const skipped = remarks.filter(remark => remark.name !== 'Inlined');
        if (skipped.length > 0) {
            hover.push('', 'Also on this line:');
            for (const remark of skipped)
                hover.push(`- \`${remark.calleeName}\` ${describeInlineDecision(remark)}`);
        }
        callSites.push({ line, hover });
    }

    // Definition sites: the same remarks regrouped by callee, so the function's own line
    // reports how much of it the optimizer folded into its callers.
    const byCallee = new Map<number, OptRemark[]>();
    for (const remark of inlineRemarks) {
        const existing = byCallee.get(remark.calleeLine);
        if (existing) existing.push(remark);
        else byCallee.set(remark.calleeLine, [remark]);
    }
    const definitions: InlineAnnotation[] = [];
    for (const info of entry.functions) {
        const remarks = (byCallee.get(info.startLine) ?? [])
            .filter(remark => remark.calleeName === info.name);
        const inlined = remarks.filter(remark => remark.name === 'Inlined');
        if (inlined.length === 0) continue;

        const summary = inlined.length === remarks.length
            ? `inlined at ${plural(inlined.length, 'call site')}`
            : `inlined at ${inlined.length} of ${plural(remarks.length, 'call site')}`;
        const hover = [`**${info.name}** - ${summary} (-O${entry.optLevel})`, ''];
        for (const remark of inlined)
            hover.push(`- line ${remark.srcLine}: ${describeInlineDecision(remark)}`);
        for (const remark of remarks.filter(remark => remark.name !== 'Inlined'))
            hover.push(`- line ${remark.srcLine}: ${describeInlineDecision(remark)}`);
        if (info.eliminated)
            hover.push('', 'No standalone copy of this function survived optimization.');
        definitions.push({ line: info.startLine, hover });
    }
    definitions.sort((a, b) => a.line - b.line);

    return { callSites, definitions };
}

// The size and register cost of one function, as markdown lines. This is what the CodeLens
// click pops up. Inlining is deliberately NOT summarised here: the caller appends a
// "Show N call sites" link that opens the References peek, and a count in prose next to a
// link that gives the same count -- and navigates -- is pure redundancy. The caller also
// appends the IR and Assembly links, which need a document URI this module deliberately
// knows nothing about.
export function buildFunctionDetail(entry: OptInfoCacheEntry, info: OptFunctionInfo): string[] {
    const lines = [`**${info.name}** at -O${entry.optLevel}`, ''];
    if (info.eliminated) {
        lines.push('No code emitted - the optimizer inlined or removed this function.');
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
        if (info.bytes > 0) lines.push(`- Function size: ${info.bytes} bytes`);
        if (info.machineInstructions > 0)
            lines.push(`- Machine instructions: ${info.machineInstructions}`);
        if (info.stackBytes > 0) lines.push(`- Stack frame: ${info.stackBytes} bytes`);
        lines.push(`- Register: ${plural(info.spills, 'spill')}, ${plural(info.reloads, 'reload')}`);
    }

    // inlinedInto is derived from the module itself; the navigable call sites come from the
    // remark stream, which is capped per pass and aggregates overloads by name. So the count
    // can outlive its evidence. Say that plainly - otherwise the reader is told the body was
    // inlined somewhere and given no way to reach it, with nothing explaining the gap.
    if (info.inlinedInto > 0 && collectInlineCallSites(entry, info).length === 0)
        lines.push('', '_The individual call sites are not available for this function._');

    return lines;
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
