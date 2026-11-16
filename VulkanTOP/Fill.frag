// No input connected: writes the animated colors.
// Compiled to SPIR-V at build time (see CMakeLists.txt) and embedded in the plugin.
#version 450
#extension GL_GOOGLE_include_directive : require

#include "AnimatedColor.glsl"

layout(location = 0) out vec4 outColor;

void main()
{
	outColor = vec4(animatedColor(pixelUV()), 1.0);
}
