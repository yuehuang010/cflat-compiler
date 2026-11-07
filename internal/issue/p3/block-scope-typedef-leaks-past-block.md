# A block-scope typedef stays visible after its block and shadows the global one

RULED 2026-11-04 (maintainer): a `typedef` / `using` declared in a block is scoped to that block, as in C, C++ and C#;
`if const` arms stay transparent (conditional compilation, both passes - the parked cflat-fix-tdscope r1 splice). Supersedes
the 4bfa4aab (2026-09-03) "no per-block alias frame" design. Test consequence: test_generics fn_scope_alias_nested_block_leak
(== 5) flips - the later `FnAliasBox<FnAliasBlock>` becomes an err_ leg ("cannot find the type"); the if-const arm legs stay.
Worktree cflat-fix-tdscope (commit 3cefad2d "TDSCOPE r1 temp") resumes on this ruling.

Found 2026-11-03 by the fntype review (box 10), PRE-EXISTING. Silent wrong type.

```cflat
typedef int A;
int f(int x) { return 1; }
int f(double x) { return 2; }
extern int main()
{
    { typedef double A; }
    A y = 1.5;                       // CFlat: double, clang: int
    printf("%d\n", (int)(y * 2));    // CFlat 3, clang 2
    function<int(A)> h = f;          // CFlat binds f(double), clang f(int)
    printf("%d\n", h(1));
    return 0;
}
```

Fix direction: typedef / using declared inside a block must be scoped to that block (pop with the scope), so lookup after
the block finds the global alias.
