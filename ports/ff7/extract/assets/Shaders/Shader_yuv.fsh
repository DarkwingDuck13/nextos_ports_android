#extension GL_OES_EGL_image_external:require
//
//  Shader.fsh
//  iFF7
//

varying mediump vec4 colorVarying;
varying mediump vec2 v_texCoord;

uniform sampler2D s_texture;
uniform samplerExternalOES s_texture_OES;
uniform bool enableOES;
uniform bool enableTex;
uniform bool enableAlphaTest;


//modified 20151008 for Nexus 6

void main()
{
	if(enableTex) {
		mediump vec4 val;
		if(enableOES) {
			mediump vec3 yuv;
			mediump vec3 rgb;
			const mediump mat3 yuv2rgb = mat3(
				1,  0,         1.2802,
				1, -0.214821, -0.380589,
				1,  2.127982,  0
			);

			yuv = texture2D(s_texture_OES, v_texCoord).rgb;
			yuv.r = 1.1643 * (yuv.r - 0.0625);
			yuv.g = yuv.g - 0.5;
			yuv.b = yuv.b - 0.5;
			rgb = yuv * yuv2rgb;
			val = vec4(rgb, texture2D(s_texture_OES, v_texCoord).a);
		} else {
			val = texture2D(s_texture, v_texCoord);
		}
		if(enableAlphaTest) {
			if(val.a <= 0.0) {
				discard;
				//return;//necessary or not?
			}
		}
		gl_FragColor = val * colorVarying;
	} else {
		gl_FragColor = colorVarying;
	}
}
