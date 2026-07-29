import { describe, it, expect } from 'vitest';
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

describe('LSP integration', () => {
    it('smoke tests pass', () => {
        if (!python) {
            expect.fail('python3 (or python) not found on PATH');
        }
        const script = join(__dirname, '../test/lsp_test.py');
        const result = spawnSync(python, [script], {
            encoding: 'utf8',
            timeout: SMOKE_CHILD_TIMEOUT_MS,
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
        });
        if (result.status !== 0) {
            expect.fail(`LSP fixture tests failed:\n${describeFailure(python, result)}`);
        }
    }, FIXTURE_CHILD_TIMEOUT_MS + 30_000);
});
