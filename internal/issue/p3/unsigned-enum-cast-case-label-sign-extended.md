# Case label `(E)200` on an enum with u8 underlying type is sign-extended to -56

Found by the ST4 review (2026-10-01), pre-existing on master. Repro:
scratch/repro_keep/st4_preexisting/switch_preexisting.cb

```cflat
enum E : u8 { Hi = 200 };
extern int main() { E e = (E)200; switch (e) { case (E)200: return 0; default: return 1; } }
```

Observed: `case label '-56' does not match the switch operand type 'u8'`. Expected: the cast
constant keeps the unsigned underlying type (200) and matches. Any unsigned enum value >= 128
(u8) / >= 32768 (u16) in a cast case label is affected.
