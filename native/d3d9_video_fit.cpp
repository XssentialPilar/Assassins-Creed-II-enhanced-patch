#include "d3d9_video_fit.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>

namespace {

template <typename Interface>
struct Release {
    void operator()(Interface* object) const noexcept { object->Release(); }
};

struct MovieVertex {
    float x, y, z, u, v;
};

static_assert(sizeof(MovieVertex) == 20);

constexpr DWORD surround_vertex[] = {
#include "movie_surround_vertex.inc"
};
constexpr DWORD surround_pixel[] = {
#include "movie_surround_pixel.inc"
};

bool movie_declaration(IDirect3DDevice9* device) {
    IDirect3DVertexDeclaration9* raw = nullptr;
    if (FAILED(device->GetVertexDeclaration(&raw)) || !raw) {
        return false;
    }
    const std::unique_ptr<IDirect3DVertexDeclaration9, Release<IDirect3DVertexDeclaration9>> declaration(raw);
    UINT count = 0;
    if (FAILED(declaration->GetDeclaration(nullptr, &count)) || count != 3) {
        return false;
    }
    D3DVERTEXELEMENT9 elements[3]{};
    if (FAILED(declaration->GetDeclaration(elements, &count))) {
        return false;
    }
    return elements[0].Stream == 0 && elements[0].Offset == 0 &&
           elements[0].Type == D3DDECLTYPE_FLOAT3 && elements[0].Usage == D3DDECLUSAGE_POSITION &&
           elements[1].Stream == 0 && elements[1].Offset == 12 &&
           elements[1].Type == D3DDECLTYPE_FLOAT2 && elements[1].Usage == D3DDECLUSAGE_TEXCOORD;
}

bool movie_texture(IDirect3DDevice9* device, D3DSURFACE_DESC& description) {
    IDirect3DBaseTexture9* raw = nullptr;
    if (FAILED(device->GetTexture(0, &raw)) || !raw) {
        return false;
    }
    const std::unique_ptr<IDirect3DBaseTexture9, Release<IDirect3DBaseTexture9>> texture(raw);
    if (texture->GetType() != D3DRTYPE_TEXTURE ||
        FAILED(static_cast<IDirect3DTexture9*>(raw)->GetLevelDesc(0, &description))) {
        return false;
    }
    return description.Format == D3DFMT_A8R8G8B8 &&
           !(description.Usage & D3DUSAGE_RENDERTARGET) && description.Width >= 512 && description.Height >= 256;
}

}  // namespace

void MovieSurround::reset() noexcept {
    if (vertex) {
        vertex->Release();
        vertex = nullptr;
    }
    if (pixel) {
        pixel->Release();
        pixel = nullptr;
    }
}

VideoLayout fit_video_viewport(UINT width, UINT height, UINT source_width, UINT source_height,
                               float min_depth, float max_depth) noexcept {
    VideoLayout layout{{0, 0, width, height, min_depth, max_depth}, {}, {}, false};
    if (!width || !height || !source_width || !source_height) {
        return layout;
    }
    const std::uint64_t image_width = static_cast<std::uint64_t>(source_width) * height;
    const std::uint64_t screen_width = static_cast<std::uint64_t>(width) * source_height;
    if (screen_width > image_width) {
        const UINT fitted = std::max(1u, static_cast<UINT>((image_width + source_height / 2) / source_height));
        const UINT margin = (width - fitted) / 2;
        layout.picture = {margin, 0, fitted, height, min_depth, max_depth};
        layout.first_band = {0, 0, margin, height, min_depth, max_depth};
        layout.second_band = {margin + fitted, 0, width - margin - fitted, height, min_depth, max_depth};
        layout.vertical_bands = true;
    } else if (image_width > screen_width) {
        const UINT fitted = std::max(1u, static_cast<UINT>((screen_width + source_width / 2) / source_width));
        const UINT margin = (height - fitted) / 2;
        layout.picture = {0, margin, width, fitted, min_depth, max_depth};
        layout.first_band = {0, 0, width, margin, min_depth, max_depth};
        layout.second_band = {0, margin + fitted, width, height - margin - fitted, min_depth, max_depth};
    }
    return layout;
}

bool draw_fitted_movie(IDirect3DDevice9* device, UINT width, UINT height,
                       const void* vertices, MovieSurround& surround, HRESULT& result,
                       UINT& bands_drawn) noexcept {
    D3DSURFACE_DESC texture{};
    D3DVIEWPORT9 previous{};
    if (!vertices || FAILED(device->GetViewport(&previous)) ||
        !movie_texture(device, texture) || !movie_declaration(device)) {
        return false;
    }

    std::array<MovieVertex, 4> picture;
    std::memcpy(picture.data(), vertices, sizeof(picture));
    float min_u = 1, min_v = 1, max_u = 0, max_v = 0;
    unsigned corners = 0;
    for (const auto& point : picture) {
        if (!(point.u >= 0 && point.u <= 1 && point.v >= 0 && point.v <= 1) ||
            std::abs(point.x) != 1 || std::abs(point.y) != 1 || !std::isfinite(point.z)) {
            return false;
        }
        corners |= 1u << ((point.x > 0 ? 1 : 0) + (point.y > 0 ? 2 : 0));
        min_u = std::min(min_u, point.u);
        min_v = std::min(min_v, point.v);
        max_u = std::max(max_u, point.u);
        max_v = std::max(max_v, point.v);
    }
    const UINT source_width = static_cast<UINT>(std::lround(texture.Width * (max_u - min_u)));
    const UINT source_height = static_cast<UINT>(std::lround(texture.Height * (max_v - min_v)));
    if (corners != 15 || source_width < 256 || source_height < 128) {
        return false;
    }

    const auto layout = fit_video_viewport(width, height, source_width, source_height, previous.MinZ, previous.MaxZ);
    const auto original_gate = fit_video_viewport(width, height, 16, 9, previous.MinZ, previous.MaxZ);
    const auto same_rect = [&previous](const D3DVIEWPORT9& viewport) {
        return previous.X == viewport.X && previous.Y == viewport.Y &&
               previous.Width == viewport.Width && previous.Height == viewport.Height;
    };
    if (!same_rect({0, 0, width, height, previous.MinZ, previous.MaxZ}) &&
        !same_rect(original_gate.picture) && !same_rect(layout.picture)) {
        return false;
    }
    const bool have_bands = (layout.first_band.Width && layout.first_band.Height) ||
                            (layout.second_band.Width && layout.second_band.Height);
    if (have_bands && !surround.vertex) {
        device->CreateVertexShader(surround_vertex, &surround.vertex);
    }
    if (have_bands && !surround.pixel) {
        device->CreatePixelShader(surround_pixel, &surround.pixel);
    }
    if (have_bands && surround.vertex && surround.pixel) {
        IDirect3DVertexShader9* old_vertex = nullptr;
        IDirect3DPixelShader9* old_pixel = nullptr;
        float previous_constants[8];
        const HRESULT vertex_status = device->GetVertexShader(&old_vertex);
        const HRESULT pixel_status = device->GetPixelShader(&old_pixel);
        const std::unique_ptr<IDirect3DVertexShader9, Release<IDirect3DVertexShader9>> original_vertex(old_vertex);
        const std::unique_ptr<IDirect3DPixelShader9, Release<IDirect3DPixelShader9>> original_pixel(old_pixel);
        if (SUCCEEDED(vertex_status) && SUCCEEDED(pixel_status) &&
            SUCCEEDED(device->GetPixelShaderConstantF(0, previous_constants, 2))) {
            const float constants[8]{min_u, min_v, max_u, max_v, .5f / texture.Width, .5f / texture.Height, 0, 0};
            if (SUCCEEDED(device->SetVertexShader(surround.vertex)) &&
                SUCCEEDED(device->SetPixelShader(surround.pixel)) &&
                SUCCEEDED(device->SetPixelShaderConstantF(0, constants, 2))) {
                const auto draw_band = [&](const D3DVIEWPORT9& band, bool first) {
                    if (!band.Width || !band.Height || FAILED(device->SetViewport(&band))) {
                        return;
                    }
                    auto edge = picture;
                    const bool side = layout.vertical_bands;
                    const float minimum = side ? min_u : min_v;
                    const float maximum = side ? max_u : max_v;
                    const float span = maximum - minimum;
                    const float extent = static_cast<float>(side ? band.Width : band.Height);
                    const float picture_extent = static_cast<float>(side ? layout.picture.Width : layout.picture.Height);
                    const float extra = span * extent / picture_extent;
                    for (auto& point : edge) {
                        float& coordinate = side ? point.u : point.v;
                        const float progress = (coordinate - minimum) / span;
                        coordinate = first ? minimum - extra * (1 - progress) : maximum + extra * progress;
                    }
                    if (SUCCEEDED(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, edge.data(), sizeof(MovieVertex)))) {
                        ++bands_drawn;
                    }
                };
                draw_band(layout.first_band, true);
                draw_band(layout.second_band, false);
            }
            device->SetPixelShaderConstantF(0, previous_constants, 2);
            device->SetPixelShader(original_pixel.get());
            device->SetVertexShader(original_vertex.get());
        }
    }
    if (FAILED(device->SetViewport(&layout.picture))) {
        device->SetViewport(&previous);
        return false;
    }
    result = device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(MovieVertex));
    device->SetViewport(&previous);
    return true;
}
