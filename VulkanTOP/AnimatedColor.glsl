// Shared by Fill.frag and Multiply.frag. The same image as MetalTOP's
// animatedColor(): scrolling diagonal bands over a horizontal hue gradient.

// Must match FillParams in VulkanTOP.cpp.
layout(push_constant) uniform FillParams
{
	float step;
	float brightness;
	float outputWidth;
	float outputHeight;
} p;

// The pixel's position in [0,1], at its center, like the Metal kernels'
// (gid + 0.5) / size. gl_FragCoord is the pixel center, with y = 0 the first row.
vec2 pixelUV()
{
	return gl_FragCoord.xy / vec2(p.outputWidth, p.outputHeight);
}

vec3 animatedColor(vec2 uv)
{
	float bands = 0.5 + 0.5 * sin((uv.x + uv.y) * 20.0 - p.step * 4.0);
	vec3  hue   = 0.5 + 0.5 * cos(6.28318 * (uv.x + p.step * 0.1 + vec3(0.0, 0.33, 0.67)));
	return hue * mix(0.4, 1.0, bands) * p.brightness;
}
