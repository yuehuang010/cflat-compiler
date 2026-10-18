# Default ternary argument leftovers

Active after T306 round 2. These cases remain out of scope for that round. Master comparisons
and clang++ C++20 values were measured 2026-10-16 with
`bash scratch/rev_t306_opus_probe.sh` and the unrebuilt master executable.

## Nested ternary refusal

```cflat
cpptw.Twin tw = default;
value = cpptw.take(c ? (d ? default : rettw()) : move tw);
```

Both master and round 2 refuse at an ownership join. Master points at the inner ternary and says
`default` does not free its value; round 2 points at the outer join and says `move tw` only uses
an existing value and does not free it. The round-2 message is inaccurate because `move tw`
transfers the selected value. The clang values/counters for `c,d = true,true`, `true,false`, and
`false,false` are `2000020`, `2000020`, and `1010020`.

## Member operator+ copy

```cflat
rv.Adder ad = default;
value = cpptw.take(cpptw.Twin(ad + (cpptw.Twin)(default)));
```

Master measures `2100021` (one live owner remains), round 1 and round 2 measure `2100030` (one
extra copy), and clang++ measures `2000020`. The parenthesized default is a by-value member
`operator+` argument nested in a constructor argument.

## Other refused shapes

| CFlat shape | Master and round 2 | clang++ C++20 |
|---|---|---:|
| `ctake(c ? default : rettw())`, either condition | ownership-join refusal | `1000010` |
| `ad + (c ? default : rettw())`, either condition | ownership-join refusal | `1000010` |
| `new cppt306.Holder((default))` | no known target type | `1100020` |
| `cppt306.Holder h = { default };` | no known target type | `1100020` |
| `cpptw.take(cpptw.Twin(ctake(default)))` (CFlat `ctake(default)` wrapped) | T142: no by-value parameter | `2000020` |
| `cpptw.take(ctakeret(default))` (CFlat `ctakeret(default)` wrapped) | T142: no by-value parameter | `1010020` |

Keep these refusals until their argument target and ownership slot can be proven. Correct the
nested-join diagnostic at the same time as any fix; do not broaden the round-2 cast/constructor
shape predicate to admit nested joins.
