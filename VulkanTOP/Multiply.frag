// Input connected: the input times the animated colors.
// Compiled to SPIR-V at build time (see CMakeLists.txt) and embedded in the plugin.
#version 450
#extension GL_GOOGLE_include_directive : require

#include "AnimatedColor.glsl"

// Normalized coordinates and linear filtering, so the input can differ in size
// from the output. The image view maps TouchDesigner's mono, mono+alpha and
// alpha-only formats to their RGBA meaning (see VulkanTOP.cpp), so the texel
// can be used as-is.
layout(set = 0, binding = 0) uniform sampler2D inTex;

layout(location = 0) out vec4 outColor;

void main()
{
	const vec2 uv  = pixelUV();
	// Mip level 0, like Metal's sample() in a compute kernel.
	const vec4 src = textureLod(inTex, uv, 0.0);
	// TD images are premultiplied; scaling rgb and keeping alpha stays premultiplied.
	outColor = vec4(src.rgb * animatedColor(uv), src.a);
}
