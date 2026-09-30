# Member bodies on call (R4) cost +4.4% cold instructions on torch

Found at the R4 landing gate (perf timebox 2026-09-29).

## Symptom
torch train.cb (scratch/cmp/train.cb, scratch/instr_ab.sh, 3 alternating pairs) cold instructions
53.8G -> 56.2G (+4.4%), wall ~3.21 -> 3.30 s (clang++ 3.1-3.3 s). json gains ~28% cold from the same
change, fmt / simdjson unchanged, warm unchanged.

## Leads
Branch-only trace scopes CxxDemandCheck + CxxDemandVerdict were ~35 ms together on one sample (the
trace A/B was confounded by a concurrent build; re-measure quietly). Suspects: per-member demand walk
(MarkFunctionReferenced + PerformPendingInstantiations per called member instead of one batch at type
request), the cohort-key hash / stamp computed per verdict (CxxGroupHeaderHash over every import's
headers for each symbol), and verdict store writes. Compare -ftime-trace master vs branch on a quiet
machine, then batch the demand checks or memoize the cohort hash per compile.

## Status 2026-09-29 (perf timebox 29d)
Verdict stores batched (one merged write per request-group / cohort file per compile, unioned with the on-disk file
at flush so concurrent compiles keep each other's verdicts): torch train.cb cold 56.15G -> 55.84G (-0.55%), json_01
-2.5%, a 46-verdict json stress -5%. The cohort hash was minor. The rest of the +4.4% is clang's own CheckDemand
instantiation work (~460 us per verdict), not bookkeeping; batching it would have to keep per-use diagnostics.
