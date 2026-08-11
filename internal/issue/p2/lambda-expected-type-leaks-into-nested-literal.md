# Nested lambda inference inherits the enclosing expected type

An inferred lambda literal nested inside a declared lambda reads the enclosing
`lambdaExpectedType`. This can misdiagnose a void-bodied inner literal as missing the outer
return value, or a value-returning inner literal as returning from void.

Do not clear the slot locally: `Test/test_function_ptr.cb` has a legal nested expression-body
lambda whose value context currently relies on the same propagation. Fix this by threading
result use and the actual expected type through postfix calls, so an immediately-invoked inner
literal receives its own context and a discarded invocation receives none.
