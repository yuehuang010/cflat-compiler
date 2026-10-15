# `return move p->f;` of a plain CFlat struct field is refused (q06 move-through-borrow leg)

Found by T173 (Haiku, 2026-10-14), confirmed by its Sol review, pre-existing on master. The implicit-this
spelling `return move f;` of a non-copyable C++ class field was fixed by T178 (the bare name now takes the
field path of `return move this->f;`, MainListener_Statements.cpp EmitReturnExpression).

Still open: `struct PF { int value = 17; }; struct PH { PF f = default; }; PF ptrMove(PH* p) { return move p->f; }`
is refused with "cannot 'move' field 'PH.f' through borrowed parameter 'p'. Declare the source parameter 'move p'...".
clang accepts it (std::move). The refusal comes from the q06 leg in ParseMoveExpression (MainListener_Expressions.cpp,
`!FieldIsOwningValue`): the ruling "move required, move allowed" covers OWNING-VALUE fields (destructor or owning
field) only. A non-owning value field is refused the same way as a scalar (`move p->x` for `int x`), so widening
it is a ruling, not a bug fix. Needs a maintainer ruling: legal only in a return, or any non-owning value field?
Pinned today: Test/errors/err_move_field_borrowed_param.cb (pointer fields only).
