# Duplicate function definitions on ONE source line compile silently

Filed 2026-10-19 from the c08 report; RESCOPED the same day after probing master (scratch/c12p/, gitignored).
Duplicates on separate lines are already refused ("redefinition of 'f'; this version is already defined at ...",
DiagnoseDuplicateFunctionBody, LLVMBackend_ControlFlowAndFunctions.cpp ~:2980) - free functions, methods, `get()` +
`get() const`, generic struct methods. The gap: `if (... firstLine == line) return;` treats a same-line repeat as
reprocessing of the same definition, so `struct S { int get() { ... } int get() { ... } };` on one line is accepted
(first wins).

Fix direction: key functionBodyOrigin_ on (file, line, column) - needs the definition column plumbed into
CreateFunctionDefinition (~25 callers, MainListener_Aggregates.cpp has 20) or set as a member by the listener before the
call. Low value: affects one-line definitions (mostly test legs). Acceptance: the one-line repro refused; generic
instantiation reuse and re-walks stay silent (test.sh green).
