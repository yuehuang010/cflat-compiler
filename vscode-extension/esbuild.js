// Bundles extension.ts and all runtime dependencies into out/extension.js.
// vscode is marked external - VS Code provides it at runtime.
const esbuild = require('esbuild');

const production = process.argv.includes('--production');
const watch = process.argv.includes('--watch');

async function main() {
    const ctx = await esbuild.context({
        entryPoints: ['src/extension.ts'],
        bundle: true,
        outfile: 'out/extension.js',
        external: ['vscode'],
        format: 'cjs',
        platform: 'node',
        target: 'node18',
        minify: production,
        sourcemap: production ? false : 'inline',
    });

    if (watch) {
        await ctx.watch();
        console.log('[esbuild] watching...');
    } else {
        await ctx.rebuild();
        await ctx.dispose();
    }
}

main().catch(err => {
    console.error(err);
    process.exit(1);
});
