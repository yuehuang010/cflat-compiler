# Assigning a borrowed pointer into a unique array element is accepted and double-frees

Summary: `unique R* arr[2]; arr[0] = <borrow>;` has no borrow check - a plain raw variable or a parameter-borrow-returning
call assigned into a unique array element compiles, and the element release frees memory the source still owns (double
free at scope end). Declarations and plain/field/pointer/global unique assignments are refused since T486.
Master vs expected: master and T486 accept and double-free; expected the same refusal as `unique R* u = <borrow>;`.
Fix direction: route unique array-element stores through the T486 BorrowedParameterCallName / raw-source check.
Found: T486 batched review (2026-10-24), PRE-EXISTING.
