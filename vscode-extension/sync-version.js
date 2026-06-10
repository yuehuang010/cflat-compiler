// Stamps the VS Code extension version from the compiler's single source of
// truth (cflat/Version.h) into package.json, so the extension tracks the
// compiler. Run by build.bat / install.bat before packaging.
//
// The compiler version is MAJOR.MINOR; semver (which vsce requires) needs a
// patch component, so the extension version is MAJOR.MINOR.0.
"use strict";

const fs = require("fs");
const path = require("path");

const versionHeader = path.join(__dirname, "..", "cflat", "Version.h");
const packageJsonPath = path.join(__dirname, "package.json");

const header = fs.readFileSync(versionHeader, "utf8");

function readDefine(name) {
    const m = header.match(new RegExp("#define\\s+" + name + "\\s+(\\d+)"));
    if (!m) {
        console.error(`sync-version: could not find #define ${name} in ${versionHeader}`);
        process.exit(1);
    }
    return parseInt(m[1], 10);
}

const major = readDefine("CFLAT_VERSION_MAJOR");
const minor = readDefine("CFLAT_VERSION_MINOR");
const version = `${major}.${minor}.0`;

const pkg = JSON.parse(fs.readFileSync(packageJsonPath, "utf8"));
if (pkg.version === version) {
    console.log(`sync-version: package.json already at ${version}`);
} else {
    console.log(`sync-version: ${pkg.version} -> ${version}`);
    pkg.version = version;
    fs.writeFileSync(packageJsonPath, JSON.stringify(pkg, null, 2) + "\n");
}
