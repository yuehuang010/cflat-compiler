# doc/LANGUAGE.md "Unions" snippet (line 408) does not compile: two errors in one block

Found while dogfooding `doc/LANGUAGE.md` (2026-09-16, macOS Release). The block at
`doc/LANGUAGE.md:408` is presented as the recommended pattern for a tagged union, but it fails
to compile for two independent reasons.

## As written (doc/LANGUAGE.md:408)

```c
enum ValueKind { Number, Node };
union ValueData { int number; Node* node; };

struct Value
{
    ValueKind kind = Number;
    ValueData data = default;

    ~Value()
    {
        if (kind == Node && data.node != nullptr)
            delete data.node;
    }
};
```

## Wrong spelling 1 - enum without a backing type (line 408)

`enum ValueKind { Number, Node };` -> `error: found '{' but expected ':'`.
CFlat enums REQUIRE an explicit backing type, as the same document states at line 446
("Enums have an explicit backing type") and as every other enum in the doc (lines 449, 2751,
2780) and in `Test/test_c.cb:469` is written. Correct: `enum ValueKind : int { Number, Node };`

## Wrong spelling 2 - unqualified enum members (lines 413, 419)

`ValueKind kind = Number;` / `kind == Node` use bare member names. Members are accessed with dot
notation (doc line 446 and the example at 449-451): `ValueKind.Number`, `kind == ValueKind.Node`.

## Wrong spelling 3 - `delete` of a union field (line 420)

`delete data.node;` ->
`cannot delete field 'ValueData.node' from outside 'ValueData'. Extract it first with
'T* p = move expr; delete p;' so the ownership transfer is explicit.`
The surrounding prose ("release the selected raw resource in the wrapper destructor") is right,
but the spelling must be the extract form:

```c
    ~Value()
    {
        if (kind == ValueKind.Node && data.node != nullptr)
        {
            Node* p = move data.node;
            delete p;
        }
    }
```

Verified: the corrected block compiles and links clean (exit 0).

## Fix

Rewrite the block at `doc/LANGUAGE.md:408-422` with the three corrections above. No compiler
change needed.
