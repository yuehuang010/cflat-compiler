# `unique` assignment laundering: deferred residue

The return-identity alias proof and field-overwrite rule landed. The va_arg C-boundary axiom pair
also landed: borrowed pointers of every spelling are legal in variadic slots, while an owning
temporary, move-returned value, explicit move, raw `new`, or owning struct rvalue entering a
variadic slot is an error directing the user to bind the value to an owner first. An owning
struct bound to a local first remains legal by value, plain non-owning structs remain legal, and
string values retain the existing string-to-variadic diagnostic.

## Deferred gap - the `llvm.mem*` DESTINATION analog

The store-through rule (`StoredValueMayBeCallerOwned`) only sees `store` instructions, and the
`llvm.mem*` rule only rejects the tracked pointer as the memcpy SOURCE. A memcpy whose DESTINATION
is the tracked pointer and whose source holds a caller-owned pointer could park that pointer inside
the pointee without any rule firing. This is unreachable today: a whole-struct field assignment
lowers to `store %Inner` (verified on emitted IR). Add the destination analog if struct assignment
ever lowers to a memcpy.
