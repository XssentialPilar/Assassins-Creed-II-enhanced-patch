#pragma once

#include <windows.h>
#include <d3d9.h>

struct VideoLayout {
    D3DVIEWPORT9 picture;
    D3DVIEWPORT9 first_band, second_band;
    bool vertical_bands;
};

struct MovieSurround {
    IDirect3DVertexShader9* vertex{};
    IDirect3DPixelShader9* pixel{};

    void reset() noexcept;
    ~MovieSurround() { reset(); }
};

VideoLayout fit_video_viewport(UINT width, UINT height, UINT source_width, UINT source_height,
                               float min_depth, float max_depth) noexcept;

// The game's movie draw uses four interleaved float3 positions and float2 UVs.
// Returns true after taking ownership of that draw, including its result.
bool draw_fitted_movie(IDirect3DDevice9* device, UINT width, UINT height,
                       const void* vertices, MovieSurround& surround, HRESULT& result,
                       UINT& bands_drawn) noexcept;
