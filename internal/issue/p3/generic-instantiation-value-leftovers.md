# C++ function-template specialization used as a CFlat function value

A real C++ function-template instance used as a CFlat function value is refused with a diagnostic, so it
does not reach the runtime trap. Binding the C++ specialization as a CFlat function value remains open;
implement it only if the C++ call path can instantiate it cheaply.
