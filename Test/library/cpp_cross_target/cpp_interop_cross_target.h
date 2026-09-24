#pragma once
// `long` is 4 bytes on LLP64 (win64) and 8 on LP64, so this record is 8 vs 16 bytes.
struct CrossTargetLong { char c; long l; };
inline int cross_target_long_size() { return (int)sizeof(CrossTargetLong); }
