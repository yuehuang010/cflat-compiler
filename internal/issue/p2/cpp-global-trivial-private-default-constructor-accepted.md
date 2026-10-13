# Global C++ array accepts a private trivial default constructor

PRE-EXISTING: reproduced on the supplied master executable and T74 fix executable.

Header: `namespace n { class S { S() = default; public: int value; }; }`
CFlat: `import cpp "header.hpp"; n.S[2] values; extern int main() { return 0; }`
Both compilers accept this declaration. clang++ -std=c++20 rejects the equivalent
`n::S values[2];` because the default constructor is private.

Cause: CxxElementNeedsDefaultConstruction only considers nontrivial or deleted
default constructors. A private trivial constructor does not queue global array
construction or reach its availability check. A CFlat struct field of this type
is correctly refused by HasUnavailableNestedCxxDefault.

Fix direction: check access independently of constructor triviality before accepting
a directly imported C++ global array. Keep public trivial arrays zero-initialized.
Review probes: scratch/rev_t74_private.hpp, scratch/rev_t74_private.cb,
scratch/rev_t74_private_oracle.cpp and their logs.
