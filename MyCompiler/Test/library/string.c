extern int strlen(const char* s);
extern int strcmp(const char* a, const char* b);
extern int strncmp(const char* a, const char* b, int n);

interface IReadonlyString
{
    int Length();
    const char* Data();
    char CharAt(int index);
    bool Equals(const char* other);
    bool StartsWith(const char* prefix);
    bool EndsWith(const char* suffix);
};

struct String : IReadonlyString
{
    const char* _data = 0;
    int _length = 0;

    void Init(const char* str)
    {
        _data = str;
        _length = strlen(str);
    }

    int Length() { return _length; }

    const char* Data() { return _data; }

    char CharAt(int index) { return _data[index]; }

    bool Equals(const char* other)
    {
        return strcmp(_data, other) == 0;
    }

    bool StartsWith(const char* prefix)
    {
        int prefixLen = strlen(prefix);
        if (prefixLen > _length) return false;
        return strncmp(_data, prefix, prefixLen) == 0;
    }

    bool EndsWith(const char* suffix)
    {
        int suffixLen = strlen(suffix);
        if (suffixLen > _length) return false;
        int offset = _length - suffixLen;
        int i = 0;
        while (i < suffixLen)
        {
            if (_data[offset + i] != suffix[i]) return false;
            i++;
        }
        return true;
    }
};
