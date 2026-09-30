# Non-default MACOSX_DEPLOYMENT_TARGET misses the core bitcode cache on every compile

Found by the R4 review round 2 (perf timebox 2026-09-29). macOS only.

## Symptom
The core bitcode cache records the deployment target it was built for (R4) and correctly misses when
it differs. But only `--init` / `--init-local` writes that cache, so with `MACOSX_DEPLOYMENT_TARGET=13.3`
every compile re-parses core until the user runs `--init` under the same variable (cold-start cost on
every compile, no error). A malformed value is refused with one LogError, but positioned at `<file>(0,0)`.

## Fix direction
Either keep one core cache per deployment target (key the runtime subdirectory on it, write it on the
first miss like other caches), or document that `--init` must run under the same target. Report the
malformed-variable error without a source position (it is a command-environment error).
