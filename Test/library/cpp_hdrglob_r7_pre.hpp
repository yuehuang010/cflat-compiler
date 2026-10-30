#pragma once

namespace hdrglob_r7 {
inline int sequence = 0;
inline int note(int digit) { sequence = sequence * 10 + digit; return digit; }
inline int sequence_now() { return sequence; }
}
