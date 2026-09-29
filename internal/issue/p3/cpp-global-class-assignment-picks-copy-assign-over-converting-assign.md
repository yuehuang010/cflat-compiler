# Global C++ class destination: `g = 9` picks operator=(const B&) instead of operator=(int)

Summary: for a file-scope global of a C++ class with both `operator=(int)` and `operator=(const B&)`,
`g = 9;` constructs a temporary B and calls the copy assignment (assigns counter 10 in the A7 review probe;
clang calls operator=(int), counter 1). Locals, fields, `*p`, `p->f`, `arr[i]` select operator=(int)
correctly. Found by the A7 reviewer (scratch/repro_keep/a7), same on master as a statement.
Fix direction: the global-destination assignment path must run the same TryDirectCxxAssignOperator
selection as locals before falling back to construct + copy-assign.
