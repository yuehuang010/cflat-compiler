# Generic FUNCTION instantiated with an array-view argument still reports a template line

## Summary

812d9bf re-attributes an array-view type argument that breaks a generic STRUCT / CLASS /
INTERFACE body to the user's argument site, because those instantiations go through
`pendingInstantiations` and carry an origin. Generic FUNCTIONS are instantiated on a different
path and carry no origin, so `f<int[]>()` where `f<T>` spells `sizeof(T)` still reports
"cannot find the type 'int[]'" at a template-body line under the caller file's name (probe
scratch/q02m3_rev3_gfnuse.cb with the template in an imported file: `gfnuse.cb(1,26)` on master
and on 812d9bf).

## Fix direction

Find the generic-function instantiation path (grep `genericFunctionTemplates` instantiation /
`InstantiateGenericFunction` in cflat/MainListener_Generics.cpp) and publish a
`gts.activeInstantiationOrigin` for it the same way ProcessPendingInstantiations does (RAII
ActiveOriginScope, origin = the explicit `<...>` argument list at the call, or the deduced
argument's expression site). ReportArrayViewInstantiationFailure then applies unchanged. Leg in
Test/errors/err_generic_array_view_arg.cb.
