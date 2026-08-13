#version 460 core

out gl_PerVertex
{
	vec4 gl_Position;
};

layout(std140, binding = 0) uniform FrameConstants
{
	mat4 projection;
	mat4 view;
	mat4 viewProjection;
	mat4 previousViewProjection;
	mat4 inverseViewProjection;
	vec4 cameraPositionAndNear;
	vec4 renderExtentAndFar;
	uvec4 countsAndFrame;
	vec4 backgroundColor;
};

uniform vec3 gizmoPivot;
uniform mat3 gizmoBasis;
uniform float gizmoScale;
uniform uint gizmoOperation;
uniform uint activeHandle;

out vec4 gizmoColor;

const uint SegmentsPerCircle = 48U;
const float Tau = 6.28318530718;

bool ClipLineToNearPlane(inout vec4 startClip, inout vec4 endClip)
{
	const float minimumW = 0.00001;
	float startDistances[2] = float[2](startClip.z, startClip.w - minimumW);
	float endDistances[2] = float[2](endClip.z, endClip.w - minimumW);
	for (uint plane = 0U; plane < 2U; ++plane)
	{
		if (startDistances[plane] < 0.0 && endDistances[plane] < 0.0)
			return false;
		if (startDistances[plane] < 0.0 || endDistances[plane] < 0.0)
		{
			float interpolation = startDistances[plane] / (startDistances[plane] - endDistances[plane]);
			vec4 clipped = mix(startClip, endClip, interpolation);
			if (startDistances[plane] < 0.0)
				startClip = clipped;
			else
				endClip = clipped;
			startDistances[0] = startClip.z;
			startDistances[1] = startClip.w - minimumW;
			endDistances[0] = endClip.z;
			endDistances[1] = endClip.w - minimumW;
		}
	}
	return true;
}

vec3 AxisColor(const uint axis)
{
	return axis == 0U ? vec3(0.95, 0.16, 0.12) : (axis == 1U ? vec3(0.22, 0.86, 0.24) : vec3(0.18, 0.46, 1.0));
}

void AxisSegment(const uint axis, out vec3 start, out vec3 end, out vec4 color, out uint handle)
{
	start = gizmoPivot;
	end = gizmoPivot + normalize(gizmoBasis[axis]) * gizmoScale;
	color = vec4(AxisColor(axis), 0.95);
	handle = axis + 1U;
}

bool BuildTranslateSegment(const uint segment, out vec3 start, out vec3 end, out vec4 color, out uint handle)
{
	if (segment < 3U)
	{
		AxisSegment(segment, start, end, color, handle);
		return true;
	}
	if (segment < 9U)
	{
		const uint arrow = segment - 3U;
		const uint axis = arrow / 2U;
		const float sideSign = (arrow & 1U) == 0U ? -1.0 : 1.0;
		const vec3 direction = normalize(gizmoBasis[axis]);
		const vec3 side = normalize(gizmoBasis[(axis + 1U) % 3U]);
		start = gizmoPivot + direction * gizmoScale;
		end = gizmoPivot + direction * (gizmoScale * 0.78) + side * (gizmoScale * 0.10 * sideSign);
		color = vec4(AxisColor(axis), 0.95);
		handle = axis + 1U;
		return true;
	}
	if (segment < 21U)
	{
		const uint planeSegment = segment - 9U;
		const uint plane = planeSegment / 4U;
		const uint edge = planeSegment % 4U;
		const uint firstAxis = plane == 0U ? 0U : (plane == 1U ? 1U : 2U);
		const uint secondAxis = plane == 0U ? 1U : (plane == 1U ? 2U : 0U);
		const vec3 first = normalize(gizmoBasis[firstAxis]) * gizmoScale;
		const vec3 second = normalize(gizmoBasis[secondAxis]) * gizmoScale;
		const vec3 corners[4] = vec3[4](gizmoPivot + first * 0.20 + second * 0.20, gizmoPivot + first * 0.42 + second * 0.20,
										gizmoPivot + first * 0.42 + second * 0.42, gizmoPivot + first * 0.20 + second * 0.42);
		start = corners[edge];
		end = corners[(edge + 1U) % 4U];
		color = vec4(mix(AxisColor(firstAxis), AxisColor(secondAxis), 0.5), 0.78);
		handle = plane + 4U;
		return true;
	}
	return false;
}

bool BuildScaleSegment(const uint segment, out vec3 start, out vec3 end, out vec4 color, out uint handle)
{
	if (segment < 3U)
	{
		AxisSegment(segment, start, end, color, handle);
		return true;
	}
	if (segment < 15U)
	{
		const uint tick = segment - 3U;
		const uint axis = tick / 4U;
		const uint edge = tick % 4U;
		const vec3 direction = normalize(gizmoBasis[axis]);
		const vec3 firstSide = normalize(gizmoBasis[(axis + 1U) % 3U]) * gizmoScale * 0.07;
		const vec3 secondSide = normalize(gizmoBasis[(axis + 2U) % 3U]) * gizmoScale * 0.07;
		const vec3 center = gizmoPivot + direction * gizmoScale;
		const vec3 corners[4] = vec3[4](center - firstSide - secondSide, center + firstSide - secondSide, center + firstSide + secondSide,
										center - firstSide + secondSide);
		start = corners[edge];
		end = corners[(edge + 1U) % 4U];
		color = vec4(AxisColor(axis), 0.95);
		handle = axis + 1U;
		return true;
	}
	if (segment < 27U)
		return BuildTranslateSegment(segment - 6U, start, end, color, handle);
	if (segment < 31U)
	{
		const uint edge = segment - 27U;
		const mat3 inverseViewRotation = transpose(mat3(view));
		const vec3 right = normalize(inverseViewRotation[0]) * gizmoScale * 0.065;
		const vec3 up = normalize(inverseViewRotation[1]) * gizmoScale * 0.065;
		const vec3 corners[4] = vec3[4](gizmoPivot - right - up, gizmoPivot + right - up, gizmoPivot + right + up, gizmoPivot - right + up);
		start = corners[edge];
		end = corners[(edge + 1U) % 4U];
		color = vec4(0.95, 0.95, 0.95, 0.95);
		handle = 8U;
		return true;
	}
	return false;
}

bool BuildRotateSegmentWithOptions(const uint segment, const float radiusScale, const bool explicitHandle, out vec3 start, out vec3 end,
								   out vec4 color, out uint handle)
{
	if (segment >= SegmentsPerCircle * 4U)
		return false;
	const uint axis = segment / SegmentsPerCircle;
	const uint arc = segment % SegmentsPerCircle;
	vec3 firstDirection;
	vec3 secondDirection;
	if (axis < 3U)
	{
		firstDirection = normalize(gizmoBasis[(axis + 1U) % 3U]);
		secondDirection = normalize(gizmoBasis[(axis + 2U) % 3U]);
	}
	else
	{
		const mat3 inverseViewRotation = transpose(mat3(view));
		firstDirection = normalize(inverseViewRotation[0]);
		secondDirection = normalize(inverseViewRotation[1]);
	}
	const float firstAngle = Tau * float(arc) / float(SegmentsPerCircle);
	const float secondAngle = Tau * float(arc + 1U) / float(SegmentsPerCircle);
	start = gizmoPivot + (firstDirection * cos(firstAngle) + secondDirection * sin(firstAngle)) * gizmoScale * radiusScale;
	end = gizmoPivot + (firstDirection * cos(secondAngle) + secondDirection * sin(secondAngle)) * gizmoScale * radiusScale;
	color = axis < 3U ? vec4(AxisColor(axis), 0.92) : vec4(0.92, 0.92, 0.92, 0.72);
	handle = explicitHandle ? (axis < 3U ? axis + 9U : 12U) : (axis < 3U ? axis + 1U : 7U);
	return true;
}

bool BuildRotateSegment(const uint segment, out vec3 start, out vec3 end, out vec4 color, out uint handle)
{
	return BuildRotateSegmentWithOptions(segment, 1.0, false, start, end, color, handle);
}

bool BuildUniversalSegment(const uint segment, out vec3 start, out vec3 end, out vec4 color, out uint handle)
{
	if (segment < 21U)
		return BuildTranslateSegment(segment, start, end, color, handle);
	if (segment < 21U + SegmentsPerCircle * 4U)
		return BuildRotateSegmentWithOptions(segment - 21U, 0.82, true, start, end, color, handle);

	const uint scaleSegment = segment - (21U + SegmentsPerCircle * 4U);
	if (scaleSegment < 12U)
	{
		const uint axis = scaleSegment / 4U;
		const uint edge = scaleSegment % 4U;
		const vec3 direction = normalize(gizmoBasis[axis]);
		const vec3 firstSide = normalize(gizmoBasis[(axis + 1U) % 3U]) * gizmoScale * 0.055;
		const vec3 secondSide = normalize(gizmoBasis[(axis + 2U) % 3U]) * gizmoScale * 0.055;
		const vec3 center = gizmoPivot + direction * gizmoScale * 0.72;
		const vec3 corners[4] = vec3[4](center - firstSide - secondSide, center + firstSide - secondSide, center + firstSide + secondSide,
										center - firstSide + secondSide);
		start = corners[edge];
		end = corners[(edge + 1U) % 4U];
		color = vec4(AxisColor(axis), 1.0);
		handle = 13U + axis;
		return true;
	}
	if (scaleSegment < 24U)
	{
		const uint planeSegment = scaleSegment - 12U;
		const uint plane = planeSegment / 4U;
		const uint edge = planeSegment % 4U;
		const uint firstAxis = plane == 0U ? 0U : (plane == 1U ? 1U : 2U);
		const uint secondAxis = plane == 0U ? 1U : (plane == 1U ? 2U : 0U);
		const vec3 first = normalize(gizmoBasis[firstAxis]) * gizmoScale;
		const vec3 second = normalize(gizmoBasis[secondAxis]) * gizmoScale;
		const vec3 corners[4] = vec3[4](gizmoPivot + first * 0.48 + second * 0.48, gizmoPivot + first * 0.61 + second * 0.48,
										gizmoPivot + first * 0.61 + second * 0.61, gizmoPivot + first * 0.48 + second * 0.61);
		start = corners[edge];
		end = corners[(edge + 1U) % 4U];
		color = vec4(mix(AxisColor(firstAxis), AxisColor(secondAxis), 0.5), 0.95);
		handle = 16U + plane;
		return true;
	}
	if (scaleSegment < 28U)
	{
		const uint edge = scaleSegment - 24U;
		const mat3 inverseViewRotation = transpose(mat3(view));
		const vec3 right = normalize(inverseViewRotation[0]) * gizmoScale * 0.065;
		const vec3 up = normalize(inverseViewRotation[1]) * gizmoScale * 0.065;
		const vec3 corners[4] = vec3[4](gizmoPivot - right - up, gizmoPivot + right - up, gizmoPivot + right + up, gizmoPivot - right + up);
		start = corners[edge];
		end = corners[(edge + 1U) % 4U];
		color = vec4(0.98, 0.98, 0.98, 1.0);
		handle = 19U;
		return true;
	}
	return false;
}

void main()
{
	const uint segment = uint(gl_VertexID) / 6U;
	const uint corner = uint(gl_VertexID) % 6U;
	vec3 start;
	vec3 end;
	vec4 color;
	uint handle = 0U;
	const bool valid = gizmoOperation == 0U
						   ? BuildTranslateSegment(segment, start, end, color, handle)
						   : (gizmoOperation == 1U ? BuildRotateSegment(segment, start, end, color, handle)
												   : (gizmoOperation == 2U ? BuildScaleSegment(segment, start, end, color, handle)
																		   : BuildUniversalSegment(segment, start, end, color, handle)));
	if (!valid)
	{
		gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
		gizmoColor = vec4(0.0);
		return;
	}

	vec4 clipStart = viewProjection * vec4(start, 1.0);
	vec4 clipEnd = viewProjection * vec4(end, 1.0);
	if (!ClipLineToNearPlane(clipStart, clipEnd))
	{
		gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
		gizmoColor = vec4(0.0);
		return;
	}
	const vec2 startNDC = clipStart.xy / clipStart.w;
	const vec2 endNDC = clipEnd.xy / clipEnd.w;
	const vec2 pixelDelta = (endNDC - startNDC) * renderExtentAndFar.xy;
	if (dot(pixelDelta, pixelDelta) < 0.001)
	{
		gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
		gizmoColor = vec4(0.0);
		return;
	}
	const vec2 pixelDirection = normalize(pixelDelta);
	const vec2 pixelNormal = vec2(-pixelDirection.y, pixelDirection.x);
	const vec2 ndcOffset = pixelNormal * (2.0 * 1.75) / renderExtentAndFar.xy;
	const bool useEnd = corner == 1U || corner == 2U || corner == 4U;
	const float side = corner == 0U || corner == 1U || corner == 3U ? -1.0 : 1.0;
	const vec4 clip = useEnd ? clipEnd : clipStart;
	gl_Position = clip + vec4(ndcOffset * side * clip.w, 0.0, 0.0);
	gizmoColor = handle == activeHandle ? vec4(1.0, 0.76, 0.12, 1.0) : color;
}
