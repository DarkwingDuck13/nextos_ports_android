//
//  Shader.vsh
//  testOpenGL2
//

attribute vec4 position;
attribute vec4 color;
attribute vec2 texcoord0;

varying mediump vec4 colorVarying;
varying mediump vec2 v_texCoord;

uniform mat4 modelViewProjectionMatrix;


void main()
{
	colorVarying = color/255.0;

	gl_Position = modelViewProjectionMatrix * position;
	v_texCoord = texcoord0;
}
