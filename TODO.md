● Here's what the core library contains across 19 workarounds in 7 files:



&#x20; Systemic issues (affect multiple files):                                                                                - string has no (ptr, len) constructor — every file that builds a string from a raw buffer writes directly to

\_ptr/\_len internal fields (string.cb, filesystem.cb)                                                                    - No implicit const char\* → string coercion in all overload contexts — causes duplicate overloads throughout



&#x20; Windows ABI gaps:

&#x20; - printf/sprintf route through internal UCRT symbols (\_\_acrt\_iob\_func) because CFlat can't call variadic C externs

&#x20; - WIN32\_FIND\_DATAA is a raw i8\[600] blob with magic offset 44 for cFileName — no header includes

&#x20; - FindFirstFileA handle typed as i64 to enable -1 comparison against INVALID\_HANDLE\_VALUE

&#x20; - \_SystemInfo is a hand-mirrored struct; new \_SystemInfo\[1] used as a stack-emulation workaround



&#x20; Generics limitations:

&#x20; - hashset/dictionary only support integer keys — no generic hash trait

&#x20; - list.removeAt can't free pointer elements — no value vs. pointer specialization

&#x20; - sort is O(n²) insertion sort

&#x20; - Error out when using (Interface\*) ptr.  Interfaces are pointers and thus should use star.



&#x20; Thread infrastructure:

&#x20; - \_\_ThreadArgs trampoline struct because CFlat closures can't be passed as Win32 callbacks

&#x20; - malloc used directly (bypasses tracked allocator) for thread args



&#x20; The highest-value fixes would be: adding a string(ptr, len) constructor and improving const char\* coercion — those

&#x20; alone would clean up dozens of lines across the library.





&#x20;Now I understand the root cause. ParseIdentifier doesn't set TypeAndValue for global variables, so calling .alloc() on a

&#x20; BlockAllocator\* global fails. The fix is to add helper functions in runtime.cb that accept BlockAllocator\* as a parameter —

&#x20; function arguments DO carry TypeAndValue, so method dispatch works.



Add a check to ensure circular import doesn't hang.

Investigate why Claude keep using null instead of nullptr.



Make into warning, Uninitialize field "\_SystemInfo::\_oemId".



BlockAllocator should use VirtualAlloc and VirtualFree.

