# std::filesystem directory_iterator cannot be iterated from CFlat

## Repro

```cflat
import cpp "filesystem";
import cpp "fstream";
extern int main()
{
    std.filesystem.path root = std.filesystem.path("std_directory_iterator_repro_tmp");
    std.filesystem.remove_all(root);
    std.filesystem.create_directory(root);
    std.filesystem.path file = root;
    file.append("entry.txt");
    std.ofstream out = std.ofstream(file);
    out << "entry";
    out.close();
    std.filesystem.directory_iterator entries = std.filesystem.directory_iterator(root);
    int count = 0;
    for (auto entry in entries) { count = count + 1; }
    std.filesystem.remove_all(root);
    return count == 1 ? 0 : 1;
}
```

## Observed vs expected

Observed: range-for lowering tries to call a `count()` method on the imported directory iterator and fails because no such overload exists.
Expected: C++17 directory iteration visits the directory entries.

## Suspected area

Range-for integration with C++ begin/end iterator ranges.
