# Nested template instance used as a field of its enclosing aggregate is incomplete

PRE-EXISTING (T113 Sol review 1, master and fix). A nested template `Inner<int>` used as a field of
the enclosing aggregate is reported incomplete. Repro: scratch/repro_keep/t113/rev_t113_lexical.cb.
Fix direction: instantiate (or predeclare the layout of) a nested template argument use before the
enclosing record's field layout is finalized.
