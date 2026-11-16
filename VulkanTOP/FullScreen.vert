// One triangle that covers the whole output; no vertex buffer needed.
// Compiled to SPIR-V at build time (see CMakeLists.txt) and embedded in the plugin.
#version 450

void main()
{
	// Vertices (-1,-1), (3,-1), (-1,3): the triangle contains the [-1,1] square.
	const vec2 pos = vec2((gl_VertexIndex & 1) * 4.0 - 1.0, (gl_VertexIndex & 2) * 2.0 - 1.0);
	gl_Position = vec4(pos, 0.0, 1.0);
}
