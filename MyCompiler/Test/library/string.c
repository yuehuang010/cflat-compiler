extern int strlen(const char* s);
extern int strcmp(const char* a, const char* b);
extern int strncmp(const char* a, const char* b, int n);
extern void* malloc(int size);
extern void free(void* ptr);

interface IReadonlyString
{
    int Length();
    const char* Data();
    char CharAt(int index);
    bool Equals(const char* other);
    bool StartsWith(const char* prefix);
    bool EndsWith(const char* suffix);
};

interface IString : IReadonlyString
{
    void Set(const char* str);
    void Append(const char* str);
    void Clear();
    void Free();
};

struct String : IString
{
    char* _data = 0;
    int _length = 0;
    int _capacity = 0;

    void _ensure(int needed)
    {
        if (_capacity >= needed) return;
        int newCap = needed * 2;
        char* newData = (char*)malloc(newCap);
        int i = 0;
        while (i < _length)
        {
            newData[i] = _data[i];
            i++;
        }
        newData[_length] = 0;
        if (_data != 0) free(_data);
        _data = newData;
        _capacity = newCap;
    }

    int Length() { return _length; }

    const char* Data() { return _data; }

    char CharAt(int index) { return _data[index]; }

    bool Equals(const char* other)
    {
        if (_data == 0) return strcmp("", other) == 0;
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

    void Set(const char* str)
    {
        int len = strlen(str);
        _ensure(len + 1);
        int i = 0;
        while (i < len)
        {
            _data[i] = str[i];
            i++;
        }
        _data[len] = 0;
        _length = len;
    }

    void Append(const char* str)
    {
        int addLen = strlen(str);
        int newLen = _length + addLen;
        _ensure(newLen + 1);
        int i = 0;
        while (i < addLen)
        {
            _data[_length + i] = str[i];
            i++;
        }
        _data[newLen] = 0;
        _length = newLen;
    }

    void Clear()
    {
        _length = 0;
        if (_data != 0) _data[0] = 0;
    }

    void Free()
    {
        if (_data != 0)
        {
            free(_data);
            _data = 0;
        }
        _length = 0;
        _capacity = 0;
    }
};

struct StringView : IReadonlyString
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
