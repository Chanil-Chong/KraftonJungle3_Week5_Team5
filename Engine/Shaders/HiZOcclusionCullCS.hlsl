struct FHiZCullCommand
{
	float3 Center;
	float Padding0;
	float3 Extent;
	uint Flags;
};

cbuffer HiZCullConstants : register(b0)
{
	float4x4 ViewProjection;
	uint ViewWidth;
	uint ViewHeight;
	uint MipCount;
	uint CommandCount;
};

StructuredBuffer<FHiZCullCommand> Commands : register(t0);
Texture2D<float> HiZPyramid : register(t1);
RWByteAddressBuffer DrawArgs : register(u0);

float2 ToScreenUV(float2 Ndc)
{
	return float2(Ndc.x * 0.5f + 0.5f, 1.0f - (Ndc.y * 0.5f + 0.5f));
}

float QueryMinDepthInRect(uint2 PixelMin, uint2 PixelMax, uint Mip)
{
	uint2 TexMin = PixelMin >> Mip;
	uint2 TexMax = (PixelMax - 1u) >> Mip;

	float MinDepth = 1.0f;
    [loop]
	for (uint Y = TexMin.y; Y <= TexMax.y; ++Y)
	{
        [loop]
		for (uint X = TexMin.x; X <= TexMax.x; ++X)
		{
			MinDepth = min(MinDepth, HiZPyramid.Load(int3(uint2(X, Y), Mip)));
		}
	}
	return MinDepth;
}

uint ChooseStartMip(uint PixelWidth, uint PixelHeight, uint MipCount)
{
	uint Mip = 0u;
	uint MaxDim = max(PixelWidth, PixelHeight);

	while ((Mip + 1u) < MipCount && MaxDim > 8u)
	{
		MaxDim = max(1u, MaxDim >> 1u);
		++Mip;
	}
	return Mip;
}

[numthreads(64, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
	uint CommandIndex = DTid.x;
	if (CommandIndex >= CommandCount)
	{
		return;
	}

	FHiZCullCommand Command = Commands[CommandIndex];
	uint Visible = 1u;

	if ((Command.Flags & 1u) != 0u)
	{
		float3 Extent = Command.Extent;
		float3 MinCorner = Command.Center - Extent;
		float3 MaxCorner = Command.Center + Extent;

		float3 Corners[8] =
		{
			float3(MinCorner.x, MinCorner.y, MinCorner.z),
            float3(MaxCorner.x, MinCorner.y, MinCorner.z),
            float3(MinCorner.x, MaxCorner.y, MinCorner.z),
            float3(MaxCorner.x, MaxCorner.y, MinCorner.z),
            float3(MinCorner.x, MinCorner.y, MaxCorner.z),
            float3(MaxCorner.x, MinCorner.y, MaxCorner.z),
            float3(MinCorner.x, MaxCorner.y, MaxCorner.z),
            float3(MaxCorner.x, MaxCorner.y, MaxCorner.z)
		};

		float2 MinNdc = float2(1.0f, 1.0f);
		float2 MaxNdc = float2(-1.0f, -1.0f);
		float ObjectNearestDepth = 0.0f;
		bool bConservativeVisible = false;

        [unroll]
		for (uint CornerIndex = 0; CornerIndex < 8; ++CornerIndex)
		{
			float4 Clip = mul(float4(Corners[CornerIndex], 1.0f), ViewProjection);
			if (Clip.w <= 1e-5f)
			{
				bConservativeVisible = true;
				break;
			}

			float3 Ndc = Clip.xyz / Clip.w;
			MinNdc = min(MinNdc, Ndc.xy);
			MaxNdc = max(MaxNdc, Ndc.xy);
			ObjectNearestDepth = max(ObjectNearestDepth, saturate(Ndc.z)); // reversed-Z
		}

		if (!bConservativeVisible)
		{
			if (MaxNdc.x < -1.0f || MinNdc.x > 1.0f || MaxNdc.y < -1.0f || MinNdc.y > 1.0f)
			{
				Visible = 0u;
			}
			else
			{
				float2 ScreenMinUV = saturate(ToScreenUV(float2(MinNdc.x, MaxNdc.y)));
				float2 ScreenMaxUV = saturate(ToScreenUV(float2(MaxNdc.x, MinNdc.y)));

				int2 PixelMinI = int2(floor(ScreenMinUV * float2(ViewWidth, ViewHeight)));
				int2 PixelMaxI = int2(ceil(ScreenMaxUV * float2(ViewWidth, ViewHeight)));

				PixelMinI = clamp(PixelMinI, int2(0, 0), int2((int) ViewWidth - 1, (int) ViewHeight - 1));
				PixelMaxI = clamp(PixelMaxI, PixelMinI + int2(1, 1), int2((int) ViewWidth, (int) ViewHeight));

				uint2 PixelMin = uint2(PixelMinI);
				uint2 PixelMax = uint2(PixelMaxI);

				uint PixelWidth = max(1u, PixelMax.x - PixelMin.x);
				uint PixelHeight = max(1u, PixelMax.y - PixelMin.y);

				uint StartMip = ChooseStartMip(PixelWidth, PixelHeight, MipCount);

				bool bOccluded = false;
				for (int Mip = (int) StartMip; Mip >= 0; --Mip)
				{
					float MinDepth = QueryMinDepthInRect(PixelMin, PixelMax, (uint) Mip);

                    // reversed-Z:
                    // object가 occluder들보다 더 뒤에 있으면 ObjectNearestDepth < MinDepth
					if (ObjectNearestDepth < MinDepth)
					{
						bOccluded = true;
						break;
					}
				}

				Visible = bOccluded ? 0u : 1u;
			}
		}
	}

	const uint ByteOffset = CommandIndex * 20u + 4u;
	DrawArgs.Store(ByteOffset, Visible);
}