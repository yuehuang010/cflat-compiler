# hpc/btree.cb and com.cb HResult<T> reject a T with a const field

Summary: after const phase 2 (R5a) the core containers relocate by construct_at and guard assigning methods with
`if const (is_assignable(T))`. Two generics were left: hpc/btree.cb shifts and stores into live default-initialized
fixed-array slots (genuine assignment), and com.cb `HResult<T>.succeed` assigns a field. Instantiating either with a
struct that has a const field fails with #13 "cannot assign a whole '{}': field '{}' is const". No test covers them.

Fix direction: btree - placement-construct into slots that are destroyed or uninitialized (design change to its node
layout), or guard insert with is_assignable and a compile_error; HResult - construct the field in place.
