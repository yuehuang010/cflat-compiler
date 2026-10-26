# Function pointer ABI-class pointee crossings remain open

Summary: Nested function-type parameter and return components are fixed by T500. The separate ABI-class crossings below remain open.

Also open (outside the T427 ruling): u32*/int* and LP64 long*/i64* pointees still cross under the ABI-class rule; clang refuses.

Repro shape: bind `function<void(u32*)>` to a `void(int*)` function, and on LP64 bind `function<void(long*)>` to a `void(i64*)` function.

Fix direction: determine whether these pointer-pointee ABI classes require an additional exact comparison, preserving T427's signedness and target-width rulings.
