#version 460

layout(location = 0) out vec2 fragCoord;

vec2 positions[3] = vec2[](
  vec2(-1.0, -1.0),  // Top left
  vec2( 3.0, -1.0),  // Top right
  vec2(-1.0,  3.0)   // Bottom left
);

vec2 texCoords[3] = vec2[](
  vec2(0.0, 0.0),  // Top left
  vec2(2.0, 0.0),  // Top right
  vec2(0.0, 2.0)   // Bottom left
);

void main()
{
  gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
  fragCoord   = texCoords[gl_VertexIndex];
}
