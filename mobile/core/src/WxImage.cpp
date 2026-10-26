// wxImage stand-in (shim/wx/image.h): PNG through libpng's simplified API, JPEG through libjpeg, uncompressed BMP.
#include "wx/image.h"
#include "wx/mstream.h"

#include <cmath>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

#include <png.h>
#include <jpeglib.h>

wxImage::wxImage(int width, int height, unsigned char *data, bool)
{
    if (width <= 0 || height <= 0 || data == nullptr) return;
    m_w = width;
    m_h = height;
    m_rgb = std::make_shared<std::vector<unsigned char>>(data, data + size_t(width) * size_t(height) * 3);
}

static bool load_png(const std::vector<unsigned char> &bytes, int &w, int &h, std::vector<unsigned char> &rgb, std::vector<unsigned char> &alpha)
{
    png_image img;
    std::memset(&img, 0, sizeof(img));
    img.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&img, bytes.data(), bytes.size())) return false;
    const bool has_alpha = (img.format & PNG_FORMAT_FLAG_ALPHA) != 0;
    img.format = PNG_FORMAT_RGBA;
    std::vector<unsigned char> rgba(PNG_IMAGE_SIZE(img));
    if (!png_image_finish_read(&img, nullptr, rgba.data(), 0, nullptr)) { png_image_free(&img); return false; }
    w = int(img.width);
    h = int(img.height);
    const size_t n = size_t(w) * size_t(h);
    rgb.resize(n * 3);
    if (has_alpha) alpha.resize(n);
    for (size_t i = 0; i < n; ++i) {
        std::memcpy(&rgb[i * 3], &rgba[i * 4], 3);
        if (has_alpha) alpha[i] = rgba[i * 4 + 3];
    }
    return true;
}

namespace {
struct JpegErr { jpeg_error_mgr mgr; jmp_buf jump; };
void jpeg_fail(j_common_ptr cinfo) { longjmp(reinterpret_cast<JpegErr *>(cinfo->err)->jump, 1); }
}

static bool load_jpeg(const std::vector<unsigned char> &bytes, int &w, int &h, std::vector<unsigned char> &rgb)
{
    jpeg_decompress_struct cinfo;
    JpegErr err;
    cinfo.err = jpeg_std_error(&err.mgr);
    err.mgr.error_exit = jpeg_fail;
    if (setjmp(err.jump)) { jpeg_destroy_decompress(&cinfo); return false; }
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, bytes.data(), (unsigned long) bytes.size());
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);
    w = int(cinfo.output_width);
    h = int(cinfo.output_height);
    rgb.resize(size_t(w) * size_t(h) * 3);
    while (cinfo.output_scanline < cinfo.output_height) {
        JSAMPROW row = &rgb[size_t(cinfo.output_scanline) * size_t(w) * 3];
        jpeg_read_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return true;
}

// BITMAPINFOHEADER, 24 or 32 bits, uncompressed (BI_RGB) or BI_BITFIELDS with the usual masks
static bool load_bmp(const std::vector<unsigned char> &b, int &w, int &h, std::vector<unsigned char> &rgb)
{
    auto u16 = [&](size_t o) { return unsigned(b[o]) | unsigned(b[o + 1]) << 8; };
    auto u32 = [&](size_t o) { return u16(o) | u16(o + 2) << 16; };
    if (b.size() < 54 || b[0] != 'B' || b[1] != 'M') return false;
    const size_t off = u32(10);
    const int width = int(u32(18)), height = int(u32(22));
    const unsigned bpp = u16(28), comp = u32(30);
    if ((bpp != 24 && bpp != 32) || (comp != 0 && comp != 3) || width <= 0 || height == 0) return false;
    const bool bottom_up = height > 0;
    w = width;
    h = std::abs(height);
    const size_t stride = (size_t(w) * bpp / 8 + 3) & ~size_t(3);
    if (off + stride * size_t(h) > b.size()) return false;
    rgb.resize(size_t(w) * size_t(h) * 3);
    for (int y = 0; y < h; ++y) {
        const unsigned char *src = &b[off + stride * size_t(bottom_up ? h - 1 - y : y)];
        for (int x = 0; x < w; ++x) {
            const unsigned char *p = src + size_t(x) * bpp / 8;
            unsigned char *d = &rgb[(size_t(y) * w + x) * 3];
            d[0] = p[2]; d[1] = p[1]; d[2] = p[0];
        }
    }
    return true;
}

bool wxImage::LoadFile(const wxString &name, wxBitmapType)
{
    m_w = m_h = 0;
    m_rgb.reset();
    m_alpha.reset();
    std::ifstream f(name.ToUTF8(), std::ios::binary);
    if (!f) return false;
    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (bytes.size() < 4) return false;
    int w = 0, h = 0;
    std::vector<unsigned char> rgb, alpha;
    bool ok = false;
    if (bytes[0] == 0x89 && bytes[1] == 'P') ok = load_png(bytes, w, h, rgb, alpha);
    else if (bytes[0] == 0xFF && bytes[1] == 0xD8) ok = load_jpeg(bytes, w, h, rgb);
    else if (bytes[0] == 'B' && bytes[1] == 'M') ok = load_bmp(bytes, w, h, rgb);
    if (!ok || w <= 0 || h <= 0) return false;
    m_w = w;
    m_h = h;
    m_rgb = std::make_shared<std::vector<unsigned char>>(std::move(rgb));
    if (!alpha.empty()) m_alpha = std::make_shared<std::vector<unsigned char>>(std::move(alpha));
    return true;
}

bool wxImage::SaveFile(wxOutputStream &stream, wxBitmapType type) const
{
    if (type != wxBITMAP_TYPE_PNG || !IsOk()) return false;
    png_image img;
    std::memset(&img, 0, sizeof(img));
    img.version = PNG_IMAGE_VERSION;
    img.width = png_uint_32(m_w);
    img.height = png_uint_32(m_h);
    std::vector<unsigned char> src;
    if (m_alpha) {
        img.format = PNG_FORMAT_RGBA;
        const size_t n = size_t(m_w) * size_t(m_h);
        src.resize(n * 4);
        for (size_t i = 0; i < n; ++i) { std::memcpy(&src[i * 4], &(*m_rgb)[i * 3], 3); src[i * 4 + 3] = (*m_alpha)[i]; }
    } else
        img.format = PNG_FORMAT_RGB;
    const void *data = m_alpha ? src.data() : m_rgb->data();
    png_alloc_size_t size = 0;
    if (!png_image_write_to_memory(&img, nullptr, &size, 0, data, 0, nullptr)) return false;
    std::vector<unsigned char> out(size);
    if (!png_image_write_to_memory(&img, out.data(), &size, 0, data, 0, nullptr)) return false;
    stream.Write(out.data(), size);
    return true;
}

wxImage wxImage::ConvertToGreyscale(double wr, double wg, double wb) const
{
    wxImage out;
    if (!IsOk()) return out;
    out.m_w = m_w;
    out.m_h = m_h;
    out.m_rgb = std::make_shared<std::vector<unsigned char>>(m_rgb->size());
    const size_t n = size_t(m_w) * size_t(m_h);
    for (size_t i = 0; i < n; ++i) {
        const unsigned char *s = &(*m_rgb)[i * 3];
        // wxImage::ConvertToGreyscale: wxColour::MakeGrey, rounded
        const unsigned char g = (unsigned char) std::lround(wr * s[0] + wg * s[1] + wb * s[2]);
        unsigned char *d = &(*out.m_rgb)[i * 3];
        d[0] = d[1] = d[2] = g;
    }
    out.m_alpha = m_alpha;
    return out;
}
