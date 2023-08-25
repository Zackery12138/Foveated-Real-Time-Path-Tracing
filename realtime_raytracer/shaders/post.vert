
 // for post processing- Draw a triangle that just covers the screen and render over it

#version 450
layout(location = 0) out vec2 v2fUV;
void main()
{
   v2fUV = vec2(
        (gl_VertexIndex << 1) & 2,  // either 0 or 2
        gl_VertexIndex & 2          // either 0 or 2
    );

    gl_Position = vec4(v2fUV * 2.0f + -1.0f, 0.0f, 1.0f);
}
