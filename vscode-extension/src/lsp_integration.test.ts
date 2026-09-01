import { describe, it, expect } from 'vitest';
import {
    OptFunctionInfo, OptInfoCacheEntry, OptRemark, buildFunctionDetail, buildInlineAnnotations,
    collectInlineCallSites
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
        name: 'helper', symbol: '_helper_i32_i32_', startLine: 1, endLine: 3,
        irInstructions: 0, machineInstructions: 0, bytes: 0, stackBytes: 0,
        spills: 0, reloads: 0, inlinedInto: 1, eliminated: true, ...overrides
    };
}

describe('buildInlineAnnotations', () => {
    it('marks a call site that was inlined', () => {
        const { callSites } = buildInlineAnnotations(entry([remark({ srcLine: 10 })]));
        expect(callSites.map(site => site.line)).toEqual([10]);
        expect(callSites[0].hover.join('\n')).toContain('`helper` inlined');
        // Cost and threshold are unitless inline-model internals with no action attached.
        expect(callSites[0].hover.join('\n')).not.toContain('cost');
    });

    it('leaves a call site that was not inlined unmarked', () => {
        const notInlined = remark({ name: 'TooCostly', kind: 'missed', args: { Cost: '3250', Threshold: '337' } });
        const { callSites, definitions } = buildInlineAnnotations(
            entry([notInlined], [helperFunction({ eliminated: false, inlinedInto: 0 })]));
        expect(callSites).toEqual([]);
        expect(definitions).toEqual([]);
    });

    it('ignores calls to functions defined outside this file', () => {
        const external = remark({ name: 'NeverInline', kind: 'missed', calleeName: 'printf', calleeLine: 0 });
        expect(buildInlineAnnotations(entry([external])).callSites).toEqual([]);
    });

    it('collapses several inlined calls on one line into a single marker', () => {
        const { callSites } = buildInlineAnnotations(entry([
            remark({ srcLine: 10 }),
            remark({ srcLine: 10, calleeName: 'other', calleeLine: 5 })
        ]));
        expect(callSites).toHaveLength(1);
        expect(callSites[0].hover.join('\n')).toContain('other');
    });

    it('reports partial inlining on the definition as N of M', () => {
        const { definitions } = buildInlineAnnotations(entry([
            remark({ srcLine: 10 }),
            remark({ srcLine: 11, name: 'TooCostly', kind: 'missed' })
        ], [helperFunction()]));
        expect(definitions.map(site => site.line)).toEqual([1]);
        const hover = definitions[0].hover.join('\n');
        expect(hover).toContain('inlined at 1 of 2 call sites');
        expect(hover).toContain('too costly');
    });

    it('reports full inlining on the definition without a ratio', () => {
        const { definitions } = buildInlineAnnotations(entry([
            remark({ srcLine: 10 }), remark({ srcLine: 11 })
        ], [helperFunction()]));
        expect(definitions[0].hover.join('\n')).toContain('inlined at 2 call sites');
    });

    it('does not attribute a remark to a different function sharing its start line', () => {
        const { definitions } = buildInlineAnnotations(
            entry([remark()], [helperFunction({ name: 'not_helper' })]));
        expect(definitions).toEqual([]);
    });
});

describe('buildFunctionDetail', () => {
    it('reports the counters without any inline summary', () => {
        // The "Show N call sites" link the caller appends carries both the count and the
        // navigation, so restating it in prose here would be redundant.
        const detail = buildFunctionDetail(entry([
            remark({ srcLine: 43 }),
            remark({ srcLine: 44, name: 'TooCostly', kind: 'missed', args: { Cost: '3250', Threshold: '337' } })
        ], [helperFunction({ eliminated: false, bytes: 96 })]),
            helperFunction({ eliminated: false, bytes: 96 })).join('\n');
        expect(detail).toContain('**helper** at -O2');
        expect(detail).toContain('Function size: 96 bytes');
        expect(detail).not.toContain('call site');
        expect(detail).not.toContain('inlined');
        expect(detail).not.toContain('cost');
        expect(detail).not.toContain('threshold');
        expect(detail).not.toContain('line 43');
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
        expect(detail).not.toContain('_helper_i32_i32_');
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
