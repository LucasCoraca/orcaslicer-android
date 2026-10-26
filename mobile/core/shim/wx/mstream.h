// Stand-in for wx/mstream.h: a growing memory buffer wxImage::SaveFile writes into.
#pragma once
#include <algorithm>
#include <cstring>
#include <vector>

class wxOutputStream
{
public:
    virtual ~wxOutputStream() = default;
    void   Write(const void *data, size_t size) { const auto *p = static_cast<const unsigned char *>(data); m_data.insert(m_data.end(), p, p + size); }
    size_t GetLength() const { return m_data.size(); }

protected:
    std::vector<unsigned char> m_data;
};

class wxMemoryOutputStream : public wxOutputStream
{
public:
    size_t CopyTo(void *buffer, size_t size) const
    {
        const size_t n = std::min(size, m_data.size());
        std::memcpy(buffer, m_data.data(), n);
        return n;
    }
};
