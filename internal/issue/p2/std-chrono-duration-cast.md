# C++ interop cannot bind `std::chrono::duration_cast`

## Repro

```cflat
import cpp "chrono";
extern int main()
{
    std.chrono.milliseconds ms = std.chrono.milliseconds(1500);
    std.chrono.seconds sec = std.chrono.duration_cast<std.chrono.seconds>(ms);
    return sec.count() == 1 ? 0 : 1;
}
```

## Observed vs expected

Observed: CFlat reports that no imported C++ header declares `std::chrono::duration_cast`.
Expected: the imported `<chrono>` declaration binds and returns one second.

## Suspected area

Standard-library header declaration ingestion or nested namespace lookup for chrono templates.
