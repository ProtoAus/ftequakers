!!ver 110
!!permu FOG

#include "sys/fog.h"

#ifndef FOGTINT
#define FOGTINT 0.2,0.3,0.35
#endif
#ifndef ALPHA
#define ALPHA 0.7
#endif

#ifdef VERTEX_SHADER
void main()
{
	gl_Position = ftetransform();
}
#endif

#ifdef FRAGMENT_SHADER
void main()
{
	gl_FragColor = fog4blend(vec4(vec3(FOGTINT), float(ALPHA)));
}
#endif
