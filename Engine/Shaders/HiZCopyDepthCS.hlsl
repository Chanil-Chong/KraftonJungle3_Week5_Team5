cbuffer HiZCopyDepthConstants : register(b0)
{
	uint SrcOffsetX;
	uint SrcOffsetY;
	uint DstWidth;
	uint DstHeight;
};

Texture2D<float> SourceDepth : register(t0);
RWTexture2D<float> OutMip : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
	if (DTid.x >= DstWidth || DTid.y >= DstHeight)
	{
		return;
	}

	const uint2 SrcCoord = uint2(SrcOffsetX + DTid.x, SrcOffsetY + DTid.y);
	OutMip[DTid.xy] = SourceDepth.Load(int3(SrcCoord, 0));
}
