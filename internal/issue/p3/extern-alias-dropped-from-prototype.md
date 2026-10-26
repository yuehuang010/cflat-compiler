# Alias qualifier is dropped on extern prototype

Summary: The alias qualifier is silently omitted when registering an extern prototype.
Repro: `extern alias Q ca();`
Master vs expected: master drops the alias; expected the declaration to retain its alias return contract.
Root cause hint: MainListener_Declarations.cpp:7781.
Found: T442 r2 opus review, 2026-10-24 box, PRE-EXISTING
