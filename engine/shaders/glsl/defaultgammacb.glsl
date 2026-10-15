!!ver 100-450
!!samps 1
!!cvarf shadows=0
//this shader is applies gamma/contrast/brightness to the source image, and dumps it out.

varying vec2 tc;
varying vec4 vc;	//gamma, contrast, brightness, contrastboost

#ifdef VERTEX_SHADER
attribute vec2 v_texcoord;
attribute vec4 v_colour;
void main ()
{
	tc = vec2(v_texcoord.s, 1.0-v_texcoord.t);
	vc = v_colour;
	gl_Position = ftetransform();
}
#endif
#ifdef FRAGMENT_SHADER
uniform float cvar_shadows;
void main ()
{
	vec3 t = texture2D(s_t0, tc).rgb;
	float y = max(dot(t, vec3(0.2126, 0.7152, 0.0722)), 0.0);
	float lift = 0.14 * clamp(cvar_shadows, 0.0, 1.0) *
		smoothstep(2.0/255.0, 0.04, y) * (1.0 - smoothstep(0.04, 0.65, y));
	// Scale luminance rather than RGB independently; leave the black toe alone.
	float peak = max(max(t.r, t.g), t.b);
	lift = min(lift, y * max(0.0, 1.0/max(peak, 0.00001) - 1.0));
	t *= 1.0 + lift/max(y, 0.00001);
	t = vc.a * t/((vc.a-1.0)*t + 1.0);
	t = pow(max(t, vec3(0.0)), vec3(vc.r))*vc.g + vc.b;
	// Stable, sub-LSB interleaved noise: no temporal shimmer or lifted black floor.
	float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715)))) - 0.5;
	float changed = (lift > 0.0 || vc.r != 1.0 || vc.g != 1.0 || vc.b != 0.0 || vc.a != 1.0) ? 1.0 : 0.0;
	float toe = smoothstep(2.0/255.0, 0.04, y);
	gl_FragColor = vec4(t + vec3(noise * changed * toe / 255.0), 1.0);
}
#endif
