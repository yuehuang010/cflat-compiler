# Operator parsing gotchas (>> two-token, multi-operand loop)

**How to apply:** When adding new binary operator levels to the grammar, ensure the Parse function handles the multi-operand case (loop over operators, not just `return firstOperand`). The `>>` two-token pattern requires `ctx->children[2]->getText() == ">"` check to distinguish right-shift from two consecutive `>` tokens.
