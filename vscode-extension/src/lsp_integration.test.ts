import { describe, it, expect } from 'vitest';
import { spawnSync } from 'child_process';
import { join } from 'path';

// Runs the Python LSP test suite as a single vitest test.
// The Python runner exits non-zero on failure; vitest picks that up.
describe('LSP integration', () => {
    it('smoke tests pass', () => {
        const script = join(__dirname, '../test/lsp_test.py');
        const result = spawnSync('python', [script], {
            encoding: 'utf8',
            timeout: 60_000,
        });
        if (result.status !== 0) {
            const out = (result.stdout + result.stderr).trim();
            expect.fail(`LSP smoke tests failed:\n${out}`);
        }
    });

    it('fixture tests pass', () => {
        const script = join(__dirname, '../test/lsp_fixture_test.py');
        const result = spawnSync('python', [script], {
            encoding: 'utf8',
            timeout: 120_000,
        });
        if (result.status !== 0) {
            const out = (result.stdout + result.stderr).trim();
            expect.fail(`LSP fixture tests failed:\n${out}`);
        }
    });
});
