# std::filesystem path slash join is unreachable from CFlat

## Repro

```cflat
import cpp "filesystem";
extern int main()
{
    std.filesystem.path base = std.filesystem.path("dir");
    std.filesystem.path child = base / "file.txt";
    return child.filename().string() == "file.txt" ? 0 : 1;
}
```

## Observed vs expected

Observed: CFlat reports no operator `/` for `std::filesystem::path`.
Expected: the C++17 non-member path join creates the child path.

## Suspected area

Binding non-member C++ operators for imported standard-library types.
