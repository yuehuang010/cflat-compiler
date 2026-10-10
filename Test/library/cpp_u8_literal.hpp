#pragma once

inline int cflat_u8_literal_size(const char8_t* text)
{
    int size = 0;
    while (text[size] != u8'\0') ++size;
    return size;
}
