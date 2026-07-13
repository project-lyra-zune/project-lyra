attribute vec2 a_pos;   // clip space
attribute vec2 a_uv;
varying vec2 v_uv;
varying float v_gx;     // separate varying: v_uv reads constant in ALU on this profile

void main()
{
    gl_Position = vec4(a_pos, 0.0, 1.0);
    v_uv = a_uv;
    v_gx = a_uv.x;
}
