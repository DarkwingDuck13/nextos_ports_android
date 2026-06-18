#version 300 es
#define ATTRIBUTE_IN in
#define VARYING_IN in
#define VARYING_OUT out
#define DECLARE_FRAG_COLOR out vec4 fragColor
#define FRAG_COLOR fragColor
#define SAMPLE_TEXTURE_2D texture

precision highp float;
ATTRIBUTE_IN vec4 vertex;
uniform vec4 uvOffsetAndScale;
VARYING_OUT vec2 texCoord;
void main()
{
    gl_Position = vec4(vertex.xy, 0.0, 1.0);
    texCoord = vertex.zw * uvOffsetAndScale.zw + uvOffsetAndScale.xy;
}
