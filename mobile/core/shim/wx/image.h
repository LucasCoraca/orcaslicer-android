// Stand-in for wx/image.h: the part of wxImage upstream's TextureLibrary uses (load a PNG / JPEG / BMP, grey it,
// save as PNG into a memory stream). RGB, 3 bytes per pixel, optional alpha, like wxImage. WxImage.cpp.
#pragma once
#include <memory>
#include <vector>

#include "wx/string.h"

enum wxBitmapType { wxBITMAP_TYPE_INVALID = 0, wxBITMAP_TYPE_BMP = 1, wxBITMAP_TYPE_PNG = 15, wxBITMAP_TYPE_JPEG = 17, wxBITMAP_TYPE_ANY = 50 };

class wxOutputStream;

class wxImage
{
public:
    wxImage() = default;
    wxImage(int width, int height, unsigned char *data, bool static_data = false);
    explicit wxImage(const wxString &name, wxBitmapType type = wxBITMAP_TYPE_ANY) { LoadFile(name, type); }

    bool LoadFile(const wxString &name, wxBitmapType type = wxBITMAP_TYPE_ANY);
    bool SaveFile(wxOutputStream &stream, wxBitmapType type) const;
    bool IsOk() const { return m_w > 0 && m_h > 0; }
    int  GetWidth() const { return m_w; }
    int  GetHeight() const { return m_h; }
    unsigned char *GetData() const { return m_rgb ? m_rgb->data() : nullptr; }
    unsigned char *GetAlpha() const { return m_alpha ? m_alpha->data() : nullptr; }
    bool HasAlpha() const { return bool(m_alpha); }
    void ClearAlpha() { m_alpha.reset(); }
    // the luminance with wxImage's default weights (0.299, 0.587, 0.114), in all three channels
    wxImage ConvertToGreyscale(double weight_r = 0.299, double weight_g = 0.587, double weight_b = 0.114) const;

private:
    int m_w{0}, m_h{0};
    std::shared_ptr<std::vector<unsigned char>> m_rgb, m_alpha;
};

inline void wxInitAllImageHandlers() {}
