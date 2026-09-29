# Instance members and operators behind a private/protected base: generic refusal text

Found 2026-09-28 by fix/inherited-members (pre-existing on master). Refusal is right; the text does
not say the base is inaccessible (clang: "'get' is a private member of 'A'" / "cannot cast to its
private base class"). The static-through-type spelling (`D.K`, `D.K()`) now names the private base
via LLVMBackend::ReportCxxMemberThroughNonPublicBase; these spellings do not reach it:

- `p.get()` on `struct Priv : private A` -> "no overload of 'get' matches the given arguments."
- `p.K()` (static through an instance) -> "Unknown identifier 'K'."
- `p + q`, `-p` -> "no operator '+' / '-' for type 'Priv'".
- `p.v` (field) -> "field 'v' of C++ class 'Priv' is private" (names the wrong class/reason).
Repro header: `struct A { int v = 1; A() = default; A(int n) : v(n) {} static int K() { return 11; }
int get() const { return v; } A operator+(const A&) const; A operator-() const; };
struct Priv : private A { Priv(int n) : A(n) {} };`.
Fix direction: call the helper where each of these refusals is raised for a C++ receiver.
