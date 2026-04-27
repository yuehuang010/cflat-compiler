&#x20;Key ABI workarounds discovered:

&#x20; 1. void\* function parameters get an extra dereference in CFlat. Win32's callback convention passes values directly, so

&#x20;  the trampoline uses \_\_ThreadArgs\* instead of void\* — struct pointer params bypass the extra load.

&#x20; 2. Explicit (void\*) cast on a struct pointer parameter generates a spurious load before the function call. Fixed by

&#x20; storing args in a void\* \_args field on Thread, then free(\_args) in join() — void\* field access loads correctly.

&#x20; 3. sizeof() doesn't work for complex types — used the hardcoded literal 16.

