sampler2D movie : register(s0);
float4 uv_bounds : register(c0);
float2 half_texel : register(c1);

struct MovieQuad {
    float4 position : POSITION0;
    float2 uv : TEXCOORD0;
};

MovieQuad vertex(float3 position : POSITION0, float2 uv : TEXCOORD0)
{
    MovieQuad output;
    output.position = float4(position, 1);
    output.uv = uv;
    return output;
}

float3 sample_movie(float2 uv)
{
    return tex2D(movie, clamp(uv, uv_bounds.xy + half_texel, uv_bounds.zw - half_texel)).rgb;
}

float4 pixel(float2 uv : TEXCOORD0) : COLOR0
{
    float2 span = uv_bounds.zw - uv_bounds.xy;
    float2 edge = clamp(uv, uv_bounds.xy, uv_bounds.zw);
    float2 beyond = uv - edge;
    float2 distance_uv = abs(beyond) / span;
    float distance = max(distance_uv.x, distance_uv.y);
    float2 reflected = edge - sign(beyond) * min(abs(beyond), span * 0.12);
    float2 radius = span * min(distance * distance * 1.5, 0.075);

    float3 color = sample_movie(reflected) * 4;
    color += 2 * (sample_movie(reflected + float2(radius.x, 0)) +
                  sample_movie(reflected - float2(radius.x, 0)) +
                  sample_movie(reflected + float2(0, radius.y)) +
                  sample_movie(reflected - float2(0, radius.y)));
    color += sample_movie(reflected + radius) + sample_movie(reflected - radius);
    color += sample_movie(reflected + float2(radius.x, -radius.y)) +
             sample_movie(reflected + float2(-radius.x, radius.y));
    return float4(color / 16, 1);
}
