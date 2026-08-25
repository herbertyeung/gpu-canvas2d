cbuffer ViewportConstants : register(b0)
{
    float2 viewportSize;
};

Texture2D texture0 : register(t0);
SamplerState sampler0 : register(s0);

struct VertexInput
{
    float2 unitPosition : POSITION;
    float2 unitUv : TEXCOORD0;
    float4 localRect : INSTANCE_RECT;
    float4 transformLinear : INSTANCE_MATRIX0;
    float4 transformTranslation : INSTANCE_MATRIX1;
    float4 color : INSTANCE_COLOR;
    float4 uvRect : INSTANCE_UV;
};

struct VertexOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

VertexOutput VSMain(VertexInput input)
{
    const float2 local = input.localRect.xy + input.unitPosition * input.localRect.zw;
    const float2 device = float2(
        local.x * input.transformLinear.x + local.y * input.transformLinear.z + input.transformTranslation.x,
        local.x * input.transformLinear.y + local.y * input.transformLinear.w + input.transformTranslation.y);

    VertexOutput output;
    output.position = float4(
        device.x * (2.0f / viewportSize.x) - 1.0f,
        1.0f - device.y * (2.0f / viewportSize.y),
        0.0f,
        1.0f);
    output.uv = input.uvRect.xy + input.unitUv * input.uvRect.zw;
    output.color = input.color;
    return output;
}

float4 PSSolid(VertexOutput input) : SV_TARGET
{
    return input.color;
}

float4 PSTextured(VertexOutput input) : SV_TARGET
{
    // Uploaded RGBA textures are expected to contain premultiplied alpha.
    return texture0.Sample(sampler0, input.uv) * input.color;
}
