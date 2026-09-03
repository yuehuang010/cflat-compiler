import { describe, it, expect } from 'vitest';
import {
    OptFunctionInfo, OptInfoCacheEntry, OptRemark, buildCallSiteHover, buildFunctionDetail,
    collectInlineCallSites, describeInlineCostBreakdown, describeInlineDecision
} from './optimization_info';
import { spawnSync } from 'child_process';
import { join } from 'path';

// Resolve the Python interpreter once: prefer python3 (macOS/Linux ship no
// bare "python"), fall back to python (typical on Windows).
function resolvePython(): string | undefined {
    for (const candidate of ['python3', 'python']) {
        const probe = spawnSync(candidate, ['--version'], { encoding: 'utf8' });
        if (!probe.error && probe.status === 0) {
            return candidate;
        }
    }
    return undefined;
}

const python = resolvePython();

// Guard against spawn failure: result.stdout/stderr are null when the process
// never started, so concatenating them directly would throw a TypeError.
function describeFailure(interpreter: string, result: ReturnType<typeof spawnSync>): string {
    if (result.error) {
        return `failed to run ${interpreter}: ${result.error.message}`;
    }
    const out = `${result.stdout ?? ''}${result.stderr ?? ''}`.trim();
    return out !== '' ? out : `(no output, exit status ${result.status})`;
}

// Runs the Python LSP test suite as a single vitest test.
// The Python runner exits non-zero on failure; vitest picks that up.
//
// Each test's vitest timeout must stay ABOVE its spawnSync timeout, so a hung
// runner is reported as a real failure by the child rather than as an opaque
// vitest timeout. These block the event loop, so vitest cannot interrupt them
// mid-call - it only flags the overrun once spawnSync returns.
const SMOKE_CHILD_TIMEOUT_MS = 60_000;
const FIXTURE_CHILD_TIMEOUT_MS = 120_000;

// The fixtures assert the English SOURCE templates of each diagnostic, so the server must not
// localize: en.json deliberately words several of them differently ("is never called" for
// "is never used"). test_lsp.sh exports the same variable for the same reason.
const CHILD_ENV = { ...process.env, CFLAT_LOCALE: 'pseudo' };

describe('LSP integration', () => {
    it('smoke tests pass', () => {
        if (!python) {
            expect.fail('python3 (or python) not found on PATH');
        }
        const script = join(__dirname, '../test/lsp_test.py');
        const result = spawnSync(python, [script], {
            encoding: 'utf8',
            timeout: SMOKE_CHILD_TIMEOUT_MS,
            env: CHILD_ENV,
        });
        if (result.status !== 0) {
            expect.fail(`LSP smoke tests failed:\n${describeFailure(python, result)}`);
        }
    }, SMOKE_CHILD_TIMEOUT_MS + 30_000);

    it('fixture tests pass', () => {
        if (!python) {
            expect.fail('python3 (or python) not found on PATH');
        }
        const script = join(__dirname, '../test/lsp_fixture_test.py');
        const result = spawnSync(python, [script], {
            encoding: 'utf8',
            timeout: FIXTURE_CHILD_TIMEOUT_MS,
            env: CHILD_ENV,
        });
        if (result.status !== 0) {
            expect.fail(`LSP fixture tests failed:\n${describeFailure(python, result)}`);
        }
    }, FIXTURE_CHILD_TIMEOUT_MS + 30_000);
});

// ---------------------------------------------------------------------------
// Inline annotation aggregation (pure; no server, no editor)
// ---------------------------------------------------------------------------

function remark(fields: Partial<OptRemark> = {}): OptRemark {
    return {
        pass: 'inline', name: 'Inlined', kind: 'passed', message: '', function: 'main',
        srcLine: 10, calleeName: 'helper', calleeLine: 1,
        args: { Cost: '25', Threshold: '337' },
        ...fields
    };
}

function entry(remarks: OptRemark[], functions: OptFunctionInfo[] = []): OptInfoCacheEntry {
    return { key: 'k', optLevel: 2, remarks, functions };
}

function helperFunction(overrides: Partial<OptFunctionInfo> = {}): OptFunctionInfo {
    return {
        name: 'helper', symbol: '_helper$int$.1$int', startLine: 1, endLine: 3,
        irInstructions: 0, machineInstructions: 0, bytes: 0, stackBytes: 0,
        spills: 0, reloads: 0, inlinedInto: 1, eliminated: true, ...overrides
    };
}

describe('buildCallSiteHover', () => {
    it('describes the decision for the callee under the cursor', () => {
        expect(buildCallSiteHover(entry([remark({ srcLine: 10 })]), 10, 'helper'))
            .toEqual(['**Inlined** at -O2']);
    });

    it('shows an inlined call with its weight, which may be negative', () => {
        const lines = buildCallSiteHover(entry([
            remark({ srcLine: 10, args: { Cost: '-35', Threshold: '337', unsimplified_common_instructions: '10', callsite_cost: '-45' } })
        ]), 10, 'helper');
        expect(lines).toEqual(['**Inlined** at -O2 (weight -35, threshold 337)']);
        const bonus = buildCallSiteHover(entry([
            remark({ srcLine: 10, args: { Cost: '-15025', Threshold: '337', last_call_to_static_bonus: '1' } })
        ]), 10, 'helper');
        expect(bonus).toEqual(['**Inlined** at -O2 (weight -15025, threshold 337)', '',
            'Includes the last-call-to-static bonus: the function is deleted after inlining.']);
    });

    it('explains a rejected call with one contributor per line', () => {
        const lines = buildCallSiteHover(entry([
            remark({ srcLine: 11, name: 'TooCostly', kind: 'missed',
                     args: { Cost: '320', Threshold: '250', unsimplified_common_instructions: '330',
                             callsite_cost: '-10', sroa_losses: '60' } })
        ]), 11, 'helper');
        expect(lines).toEqual([
            '**Not inlined** at -O2: too costly (weight 320, threshold 250)',
            '- Instructions: 330 (~66 instructions)',
            '- Call overhead removed: -10',
            '',
            'Missed reduction: 60 (argument address escapes)'
        ]);
    });

    it('collapses identical verdicts from repeated calls and re-inlined copies', () => {
        const args = { Cost: '1730', Threshold: '225', unsimplified_common_instructions: '1690' };
        const lines = buildCallSiteHover(entry([
            remark({ srcLine: 114, name: 'TooCostly', kind: 'missed', args }),
            remark({ srcLine: 114, name: 'TooCostly', kind: 'missed', args }),
            remark({ srcLine: 114, name: 'TooCostly', kind: 'missed', args, function: 'main' }),
            remark({ srcLine: 114, name: 'TooCostly', kind: 'missed', args, function: 'main' })
        ]), 114, 'helper');
        expect(lines).toEqual([
            '**Not inlined** at -O2: too costly (weight 1730, threshold 225)',
            '- Instructions: 1690 (~338 instructions)'
        ]);
    });

    it('lists genuinely different decisions on one line as separate blocks', () => {
        const lines = buildCallSiteHover(entry([
            remark({ srcLine: 20 }),
            remark({ srcLine: 20, name: 'TooCostly', kind: 'missed', args: { Cost: '3250', Threshold: '337' } })
        ]), 20, 'helper');
        expect(lines).toEqual([
            '**Inlined** at -O2',
            '',
            '**Not inlined** at -O2: too costly'
        ]);
    });

    it('ignores other callees on the line and other lines', () => {
        const remarks = [remark({ srcLine: 10 }), remark({ srcLine: 10, calleeName: 'other', calleeLine: 5 })];
        expect(buildCallSiteHover(entry(remarks), 10, 'other')).toEqual(['**Inlined** at -O2']);
        expect(buildCallSiteHover(entry(remarks), 11, 'helper')).toEqual([]);
        expect(buildCallSiteHover(entry(remarks), 10, 'nothing')).toEqual([]);
    });
});

// ---------------------------------------------------------------------------
// Inline cost breakdown rendering (pure; no server, no editor)
// ---------------------------------------------------------------------------

describe('describeInlineDecision cost breakdown', () => {
    it('renders outcome only when the server sent no breakdown', () => {
        // An older server sends Cost/Threshold and nothing else; that must look as it did.
        expect(describeInlineDecision(remark({ name: 'Inlined', args: { Cost: '25', Threshold: '337' } })))
            .toBe('inlined');
        expect(describeInlineDecision(remark({
            name: 'TooCostly', kind: 'missed', args: { Cost: '3250', Threshold: '337' }
        }))).toBe('not inlined - too costly');
        expect(describeInlineCostBreakdown({ Cost: '3250', Threshold: '337' }, false)).toBeUndefined();
        expect(describeInlineCostBreakdown(undefined, true)).toBeUndefined();
    });

    it('explains a too-costly call with its contributors, biggest cost to biggest credit', () => {
        expect(describeInlineDecision(remark({
            name: 'TooCostly', kind: 'missed',
            args: {
                Cost: '320', Threshold: '250',
                unsimplified_common_instructions: '240',
                call_penalty: '50',
                switch_penalty: '25', jump_table_penalty: '15',
                call_argument_setup: '10', lowered_call_arg_setup: '4',
                sroa_savings: '10', callsite_cost: '-14',
                sroa_losses: '60',
                num_loops: '2', dead_blocks: '1', threshold: '250'
            }
        }))).toBe('not inlined - too costly (weight 320, threshold 250): '
            + 'Instructions: 240 (~48 instructions), Calls inside callee: 50, Switch lowering: 40, Argument setup: 14, '
            + 'Stack promotion savings: -10, Call overhead removed: -14. '
            + 'Missed reduction: 60 (argument address escapes)');
    });

    it('caps the list at six contributors and counts the rest', () => {
        expect(describeInlineDecision(remark({
            name: 'TooCostly', kind: 'missed',
            args: {
                Cost: '2800', Threshold: '250',
                unsimplified_common_instructions: '700', call_penalty: '600',
                switch_penalty: '500', call_argument_setup: '400',
                indirect_call_penalty: '300', cold_cc_penalty: '200',
                load_relative_intrinsic: '100',
                sroa_savings: '5', load_elimination: '4', callsite_cost: '-3'
            }
        }))).toBe('not inlined - too costly (weight 2800, threshold 250): '
            + 'Instructions: 700 (~140 instructions), Calls inside callee: 600, Switch lowering: 500, Argument setup: 400, '
            + 'Indirect calls: 300, Cold calling convention: 200 +4 more.');
    });

    it('sums the features that describe one user-visible cost', () => {
        expect(describeInlineDecision(remark({
            name: 'TooCostly', kind: 'missed',
            args: {
                Cost: '20', Threshold: '100',
                switch_penalty: '1', jump_table_penalty: '2',
                case_cluster_penalty: '3', switch_default_dest_penalty: '4',
                call_argument_setup: '3', lowered_call_arg_setup: '4'
            }
        }))).toBe('not inlined - too costly (weight 20, threshold 100): '
            + 'Switch lowering: 10, Argument setup: 7.');
    });

    it('reports the threshold bonus as its own sentence', () => {
        expect(describeInlineDecision(remark({
            name: 'TooCostly', kind: 'missed',
            args: {
                Cost: '400', Threshold: '250',
                unsimplified_common_instructions: '400', last_call_to_static_bonus: '90'
            }
        }))).toBe('not inlined - too costly (weight 400, threshold 250): Instructions: 400 (~80 instructions). '
            + 'Includes the last-call-to-static bonus: the function is deleted after inlining.');
    });

    it('gives an inlined call its weight and no contributor list', () => {
        expect(describeInlineDecision(remark({
            name: 'Inlined',
            args: {
                Cost: '35', Threshold: '250',
                unsimplified_common_instructions: '45', callsite_cost: '-10'
            }
        }))).toBe('inlined (weight 35, threshold 250)');
    });

    it('keeps the reason-carrying branches untouched', () => {
        expect(describeInlineDecision(remark({
            name: 'NeverInline', kind: 'missed', args: { Reason: 'noinline attribute' }
        }))).toBe('never inlined - noinline attribute');
        expect(describeInlineDecision(remark({
            name: 'IndirectCall', kind: 'missed',
            args: { Cost: '10', Threshold: '250', call_penalty: '25' }
        }))).toBe('not inlined - indirectcall (weight 10, threshold 250): Calls inside callee: 25.');
    });
});

describe('buildFunctionDetail', () => {
    it('reports the counters without any inline summary', () => {
        // The "Show N call sites" link the caller appends carries both the count and the
        // navigation; the decisions themselves live on the call-site hover.
        const detail = buildFunctionDetail(entry([
            remark({ srcLine: 43 }),
            remark({ srcLine: 44, name: 'TooCostly', kind: 'missed', args: { Cost: '3250', Threshold: '337' } })
        ], [helperFunction({ eliminated: false, bytes: 96 })]),
            helperFunction({ eliminated: false, bytes: 96 })).join('\n');
        expect(detail).toContain('**helper** at -O2');
        expect(detail).toContain('Function size: 96 bytes');
        expect(detail).not.toContain('call site');
        expect(detail).not.toContain('inlined');
        expect(detail).not.toContain('saving');
        expect(detail).not.toContain('line 43');
    });

    it('reports the inline cost seen at the call sites, as a range when it varies', () => {
        const detail = buildFunctionDetail(entry([
            remark({ srcLine: 43, args: { Cost: '35', Threshold: '250' } }),
            remark({ srcLine: 44, name: 'TooCostly', kind: 'missed',
                     args: { Cost: '1730', Threshold: '225', unsimplified_common_instructions: '1690' } }),
            remark({ srcLine: 45, args: { Cost: '-15025', Threshold: '337', last_call_to_static_bonus: '1' } })
        ], [helperFunction()]), helperFunction({ eliminated: false, bytes: 96 })).join('\n');
        expect(detail).toContain('- Inline weight: 35 to 1730 (~338 instructions)');
        expect(detail).not.toContain('15025');
        const single = buildFunctionDetail(entry([remark({ srcLine: 43, args: { Cost: '35', Threshold: '250' } })],
            [helperFunction()]), helperFunction()).join('\n');
        expect(single).toContain('- Inline weight: 35');
        expect(buildFunctionDetail(entry([], []), helperFunction()).join('\n')).not.toContain('Inline weight');
    });

    it('says nothing about inlining even when a function was never inlined', () => {
        const missed = remark({ name: 'TooCostly', kind: 'missed' });
        const detail = buildFunctionDetail(
            entry([missed], []), helperFunction({ eliminated: false })).join('\n');
        expect(detail).not.toContain('call site');
        expect(detail).not.toContain('too costly');
    });

    it('omits counters the server could not measure', () => {
        // A zero counter is "not available", not "measured as zero" - printing it as 0
        // would claim a function has no instructions when the server simply had no figure.
        const detail = buildFunctionDetail(entry([], []),
            helperFunction({ eliminated: false, bytes: 0, stackBytes: 0,
                             machineInstructions: 0, irInstructions: 12 })).join('\n');
        expect(detail).not.toContain('Function size');
        expect(detail).not.toContain('Stack frame');
        expect(detail).not.toContain('Machine instructions');
        expect(detail).toContain('Register: 0 spills, 0 reloads');
    });

    it('never reports compiler-internal figures - IR count, mangled symbol', () => {
        const detail = buildFunctionDetail(entry([], []),
            helperFunction({ eliminated: false, bytes: 96, irInstructions: 62 })).join('\n');
        expect(detail).not.toContain('IR');
        expect(detail).not.toContain('62');
        expect(detail).not.toContain('Symbol');
        expect(detail).not.toContain('_helper$int$.1$int');
        expect(detail).toContain('Function size: 96 bytes');
    });

    it('replaces the counters with a plain statement when nothing was emitted', () => {
        const detail = buildFunctionDetail(entry([], []), helperFunction()).join('\n');
        expect(detail).toContain('No code emitted');
        expect(detail).not.toContain('Machine instructions');
    });
});

describe('buildFunctionDetail unbacked inline count', () => {
    it('says so when a body was inlined but no call site is known', () => {
        // inlinedInto comes from the module, the sites from the capped remark stream.
        const detail = buildFunctionDetail(entry([], [helperFunction()]),
            helperFunction({ eliminated: false, inlinedInto: 6 })).join('\n');
        expect(detail).toContain('call sites are not available');
    });

    it('stays silent when the sites are there to link to', () => {
        const detail = buildFunctionDetail(entry([remark({ srcLine: 43 })], [helperFunction()]),
            helperFunction({ eliminated: false, inlinedInto: 1 })).join('\n');
        expect(detail).not.toContain('not available');
    });
});

describe('collectInlineCallSites', () => {
    it('returns every site in line order, keeping two calls on one line', () => {
        const sites = collectInlineCallSites(entry([
            remark({ srcLine: 52 }),
            remark({ srcLine: 40 }),
            remark({ srcLine: 40, name: 'TooCostly', kind: 'missed' })
        ], [helperFunction()]), helperFunction());
        expect(sites.map(site => site.srcLine)).toEqual([40, 40, 52]);
        expect(sites.map(site => site.inlined)).toEqual([true, false, true]);
    });

    it('ignores remarks belonging to a different function', () => {
        const other = remark({ calleeName: 'elsewhere', calleeLine: 99 });
        expect(collectInlineCallSites(entry([other], [helperFunction()]), helperFunction()))
            .toEqual([]);
    });
});
