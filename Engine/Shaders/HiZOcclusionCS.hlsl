cbuffer HiZReduceConstants : register(b0)
{
    uint SrcWidth;
    uint SrcHeight;
    uint DstWidth;
    uint DstHeight;
    uint SrcOffsetX;
    uint SrcOffsetY;
};

Texture2D<float> InputTexture : register(t0);
RWTexture2D<float> OutputTexture : register(u0);

[numthreads(8, 8, 1)]
void CopyDepthMain(uint3 DispatchThreadID : SV_DispatchThreadID)
{
    if (DispatchThreadID.x >= DstWidth || DispatchThreadID.y >= DstHeight)
    {
        return;
    }

    uint2 SourceCoord = DispatchThreadID.xy + uint2(SrcOffsetX, SrcOffsetY);
    OutputTexture[DispatchThreadID.xy] = InputTexture.Load(int3(SourceCoord, 0));
}

[numthreads(8, 8, 1)]
void ReduceMinMain(uint3 DispatchThreadID : SV_DispatchThreadID)
{
    if (DispatchThreadID.x >= DstWidth || DispatchThreadID.y >= DstHeight)
    {
        return;
    }

    uint2 BaseCoord = DispatchThreadID.xy * 2u;
    uint2 Coord00 = min(BaseCoord, uint2(SrcWidth - 1u, SrcHeight - 1u));
    uint2 Coord10 = min(BaseCoord + uint2(1u, 0u), uint2(SrcWidth - 1u, SrcHeight - 1u));
    uint2 Coord01 = min(BaseCoord + uint2(0u, 1u), uint2(SrcWidth - 1u, SrcHeight - 1u));
    uint2 Coord11 = min(BaseCoord + uint2(1u, 1u), uint2(SrcWidth - 1u, SrcHeight - 1u));

    float Depth00 = InputTexture.Load(int3(Coord00, 0));
    float Depth10 = InputTexture.Load(int3(Coord10, 0));
    float Depth01 = InputTexture.Load(int3(Coord01, 0));
    float Depth11 = InputTexture.Load(int3(Coord11, 0));

    // Reversed-Z path: keep the nearest (largest) depth in the hierarchy.
    OutputTexture[DispatchThreadID.xy] = max(max(Depth00, Depth10), max(Depth01, Depth11));
}
