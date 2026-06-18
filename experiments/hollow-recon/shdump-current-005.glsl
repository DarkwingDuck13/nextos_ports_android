#version 300 es
#define ATTRIBUTE_IN in
#define VARYING_IN in
#define VARYING_OUT out
#define DECLARE_FRAG_COLOR out vec4 fragColor
#define FRAG_COLOR fragColor
#define SAMPLE_TEXTURE_2D texture

precision mediump float;
VARYING_IN vec2 texCoord;
#ifdef DECLARE_FRAG_COLOR
    DECLARE_FRAG_COLOR;
#endif
uniform sampler2D tex;
void main()
{
    vec4 c = SAMPLE_TEXTURE_2D(tex, texCoord);
    FRAG_COLOR = c;
}
