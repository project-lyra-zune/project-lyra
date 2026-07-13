precision mediump float;

varying vec2 v_uv;      // texture2D only; a texture-coord varying reads constant in ALU
varying float v_gx;     // gradient coordinate for ALU (see v_uv)

uniform sampler2D u_tex;
uniform float u_phase;  // magenta crest position, looped on the CPU
uniform float u_wash;   // 1 = animated gradient (wordmark), 0 = solid (labels)
uniform vec3 u_warm;
uniform vec3 u_magenta;
uniform vec3 u_solid;

// Tegra declares alpha blending in the shader, not via glBlendFunc; without this
// the alpha channel is ignored and quads render opaque.
#pragma profilepragma blendoperation(gl_FragColor, GL_FUNC_ADD,GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA)
void main()
{
    float a = texture2D(u_tex, v_uv).a;

    // Warm->magenta ramp of period 2 (the +/-2.0 copies) so the face shows mostly
    // one gradient, not the whole cycle; the wrap keeps the loop seamless. abs/min
    // only; mod()/fract() mislower on this profile.
    float d = min(abs(v_gx - u_phase), min(abs(v_gx - u_phase + 2.0), abs(v_gx - u_phase - 2.0)));
    float t = 1.0 - min(1.0, d);
    vec3 grad = u_warm + t * (u_magenta - u_warm);

    vec3 rgb = u_solid + u_wash * (grad - u_solid);
    gl_FragColor = vec4(rgb, a);
}
