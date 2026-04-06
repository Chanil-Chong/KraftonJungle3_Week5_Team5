cbuffer HiZReduceConstants : register(b0)
{
	uint SrcWidth;
	uint SrcHeight;
	uint DstWidth;
	uint DstHeight;
};

Texture2D<float> SourceMip : register(t0);
RWTexture2D<float> OutMip : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
	if (DTid.x >= DstWidth || DTid.y >= DstHeight)
	{
		return;
	}

	uint2 SrcBase = DTid.xy * 2;
	uint2 P0 = min(SrcBase + uint2(0, 0), uint2(SrcWidth - 1, SrcHeight - 1));
	uint2 P1 = min(SrcBase + uint2(1, 0), uint2(SrcWidth - 1, SrcHeight - 1));
	uint2 P2 = min(SrcBase + uint2(0, 1), uint2(SrcWidth - 1, SrcHeight - 1));
	uint2 P3 = min(SrcBase + uint2(1, 1), uint2(SrcWidth - 1, SrcHeight - 1));

	float Z0 = SourceMip.Load(int3(P0, 0));
	float Z1 = SourceMip.Load(int3(P1, 0));
	float Z2 = SourceMip.Load(int3(P2, 0));
	float Z3 = SourceMip.Load(int3(P3, 0));

	// Reversed-Z: keep the minimum depth of the covered region for conservative occlusion.
	OutMip[DTid.xy] = min(min(Z0, Z1), min(Z2, Z3));
}
