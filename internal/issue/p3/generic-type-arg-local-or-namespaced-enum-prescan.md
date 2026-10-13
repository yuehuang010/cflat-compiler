# Generic struct argument naming a local or bare namespaced enum is not found

Summary: `box<Col>` where Col is a function-local enum, or a namespace enum spelled bare inside its namespace, fails "cannot find the type 'Col'" (generic FUNCTIONS resolve both since T115).

Repro: scratch/repro_keep/t115/r2_onefn_box.cb, r3_ns_box.cb (T115 reviews 2-3).

Root cause: the body pre-scan queues `box$Col` from the bare spelling before the local enum is registered / without namespace resolution; direct MangleGenericInstance sites (ForwardRefScanner, MainListener_PostfixExpression.cpp ~2950/~3849) use the bare spelling.

Fix direction: resolve type arguments through ResolveEnumTypeName (scope-aware) at the pre-scan / pending-instantiation sites as MangledGenericName does since T115; local enums need the pre-scan to defer until the declaration is seen.
