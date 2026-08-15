# File.readAll() caps string length at i32 (>2GB files yield a wrong-length string)

Created: 2026-08-14 (residual noticed while landing file-offsets-capped-at-2gb)

## Summary

The 64-bit offset fix widened File.size()/tell()/seek()/readBytes/writeBytes to i64,
but readAll() still narrows the size when building the result string:

```cflat
// filesystem.cb readAll()
i64 isz = _fs_ftell(_handle);
...
return _strOwned(buf, (i32)isz);
```

On a >2GB file the full buffer is allocated and read, then the string records a
wrapped/negative length - silent corruption rather than an error.

## Root cause

The string type carries an i32 length by design. This is the same "declared narrower
than C" family as `extern i32 strlen(...)` (cruntime.cb), which the offset issue
explicitly kept out of scope because widening string length ripples through all
string code.

## Fix direction

Either (a) widen string length to i64 (large, touches all string code - same decision
as the strlen note), or (b) short-term: make readAll() fail loudly (return "" or an
error) when size exceeds i32 max instead of truncating. (b) is a 3-line guard and
does not preclude (a).
