!!permu FRAMEBLEND
!!permu SKELETAL
//opaque surfaces are drawn to the render target to mask out skies
//FTESurf Patch 1047: skinned, as depthonly.glsl -- Source props are GPU-skinned now
#include "sys/defs.h"
#ifdef VERTEX_SHADER
#include "sys/skeletal.h"
void main ()
{
	gl_Position = skeletaltransform();
}
#endif
#ifdef FRAGMENT_SHADER
void main()
{
	gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
}
#endif
