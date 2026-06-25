//
//  Shader.fsh (FF7 NextOS — sem GL_OES_EGL_image_external p/ Mali-450 fbdev)
//  O caminho OES (texture externa de video) foi removido: o Mali-450 fbdev EGL
//  pode nao expor a extensao, e o ':require' faria o shader PRINCIPAL falhar.
//  enableOES/s_texture_OES viram no-op (uniform inexistente -> location -1).
//
varying mediump vec4 colorVarying;
varying mediump vec2 v_texCoord;

uniform sampler2D s_texture;
uniform bool enableOES;
uniform bool enableTex;
uniform bool enableAlphaTest;

void main()
{
	if(enableTex) {
		mediump vec4 val = texture2D(s_texture, v_texCoord);
		if(enableAlphaTest) {
			if(val.a <= 0.0) {
				discard;
			}
		}
		gl_FragColor = val * colorVarying;
	} else {
		gl_FragColor = colorVarying;
	}
}
