#ifndef CUTSCENE_SKIP_RING_H
#define CUTSCENE_SKIP_RING_H

#include <math.h>

// Screen-space annular quads for the Frame cutscene hold indicator. Increasing
// screen-space angle starts at 12 o'clock and proceeds clockwise because Y
// grows downwards in RenderWare's 2D coordinates.
struct CutsceneSkipRingQuad
{
	float x[4];
	float y[4];
};

class CutsceneSkipRing
{
public:
	static const int SEGMENTS = 64;

	static bool GetSegment(float progress, int index, float centerX,
	                       float centerY, float outerRadius, float thickness,
	                       CutsceneSkipRingQuad *quad)
	{
		if(!quad || index < 0 || index >= SEGMENTS ||
		   !(progress > 0.0f) || !(outerRadius > 0.0f) ||
		   !(thickness > 0.0f))
			return false;
		if(progress > 1.0f)
			progress = 1.0f;

		const float tau = 6.2831853071795864769f;
		const float step = tau / (float)SEGMENTS;
		const float start = (float)index * step;
		const float finish = progress * tau;
		if(start >= finish)
			return false;
		const float end = start + step < finish ? start + step : finish;
		const float innerRadius = outerRadius > thickness ?
			outerRadius - thickness : 0.0f;
		const float startAngle = -1.5707963267948966192f + start;
		const float endAngle = -1.5707963267948966192f + end;
		const float cosStart = cosf(startAngle);
		const float sinStart = sinf(startAngle);
		const float cosEnd = cosf(endAngle);
		const float sinEnd = sinf(endAngle);

		quad->x[0] = centerX + cosStart * outerRadius;
		quad->y[0] = centerY + sinStart * outerRadius;
		quad->x[1] = centerX + cosEnd * outerRadius;
		quad->y[1] = centerY + sinEnd * outerRadius;
		quad->x[2] = centerX + cosEnd * innerRadius;
		quad->y[2] = centerY + sinEnd * innerRadius;
		quad->x[3] = centerX + cosStart * innerRadius;
		quad->y[3] = centerY + sinStart * innerRadius;
		return true;
	}

	// CSprite2d::SetVertices stores Draw2DPolygon's arguments in the
	// RenderWare triangle-fan order [arg3, arg4, arg2, arg1]. Reorder this
	// perimeter quad so that the native fan receives [outer-start,
	// outer-end, inner-end, inner-start] without crossing the annulus.
	static bool GetDraw2DPolygonArguments(const CutsceneSkipRingQuad &perimeter,
	                                      CutsceneSkipRingQuad *arguments)
	{
		if(!arguments)
			return false;
		arguments->x[0] = perimeter.x[3];
		arguments->y[0] = perimeter.y[3];
		arguments->x[1] = perimeter.x[2];
		arguments->y[1] = perimeter.y[2];
		arguments->x[2] = perimeter.x[0];
		arguments->y[2] = perimeter.y[0];
		arguments->x[3] = perimeter.x[1];
		arguments->y[3] = perimeter.y[1];
		return true;
	}
};

#endif
