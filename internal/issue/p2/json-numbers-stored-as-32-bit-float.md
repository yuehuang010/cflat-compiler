# `JsonNode` stores every number as a 32-bit `float`, so large values are silently corrupted

Filed 2026-08-21 from an external report (v0.11.0 issue 12 - USD revenue figures and epoch
timestamps out of a REST feed). Reproduced on `39d4b38`, Release.

Severity: **silent data corruption** in a core library. No diagnostic; the value is simply wrong.

## Repro

```cflat
import "json.cb";
extern int main() {
    JsonNode* root = JsonParser.parse("{\"rev\": 394328000000, \"ts\": 1724284800, \"eps\": 2.01}");
    printf("rev as float = %f\n", root.getFloat("rev"));   // -808991232.000000  (expected 394328000000)
    printf("ts  as int   = %d\n", root.getInt("ts"));      // 1724284800 - fits, but only just
    printf("eps          = %.10f\n", root.getFloat("eps")); // 2.0099999905      (expected 2.01)
    delete root;
    return 0;
}
```

`rev` comes back **negative**. A JSON document that round-trips fine through every other parser
produces a wrong number here with no warning.

## Root cause

`cflat/core/json.cb`:

- `float floatVal = 0;` (line 203) - the node's only numeric storage is 32-bit.
- the number parser accumulates into `float acc` / `float frac = 0.1f` (lines 543-562), so
  precision is lost *during parsing*, before storage.
- the accessors expose only `int getInt(...)` (i32, lines 283/344) and `float getFloat(...)`
  (lines 301/362), so even a widened representation would be unreachable from the public API.

Consequences: ~7 significant decimal digits for any number; integers above 2^31 overflow `getInt`;
`2.01` is not representable so it reads back as `2.0099999905`.

## Fix direction

1. Widen storage: `double floatVal` (and `i64` for the integer node kind), and accumulate in
   `double` in the parser. This alone fixes `rev` and `eps`.
2. Add the accessors the widened storage needs: `double getDouble(string key)` /
   `i64 getInt64(string key)`, plus the by-index overloads, on both `JsonNode` and the `IJSON`
   interface.
3. Keep `getFloat`/`getInt` as narrowing convenience wrappers so existing callers still compile.

JSON itself has no integer/float distinction and its interoperable range is defined in terms of
IEEE 754 **double** (RFC 8259 s.6), so double + i64 is the correct target, not a nicety.

Also check the emit side: `visitFloat(string name, float value)` (line 69) has the same 32-bit
narrowing on the way OUT, so a serialize-parse round trip loses precision twice.

## Regression test

Extend the existing JSON test with the three values above asserting exact round-trips
(`394328000000`, `1724284800`, and `2.01` compared with a tight epsilon).
