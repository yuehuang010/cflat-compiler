/* Fixture for Test/errors/err_extern_collides_with_core.cb.

   Core (cflat/core/cruntime.cb) declares `extern void putn(const char* buf, int len)`, so this
   prototype reaches the linkage name `putn` second and with a different llvm FunctionType. It
   must be diagnosed by the C-import route of the conflicting-declaration check, not dropped.

   `putn` is deliberately NOT a libc name: clang would reject a redeclaration of a builtin
   before the extractor ever sees it, and the test would then pass for the wrong reason. */
int putn(long long a);
