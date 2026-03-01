// cube.frag.slang — Fragment shader.
// Receives interpolated color from the vertex shader and outputs it directly.
// Normal-based lighting can be added once camera setup is confirmed working.

struct FSIn
{
    float4 color : COLOR0;
};

[shader("fragment")]
float4 main(FSIn i) : SV_Target
{
    return float4(i.color.rgb, 1.0f);
}
