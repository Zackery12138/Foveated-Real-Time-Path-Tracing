#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_GOOGLE_include_directive : enable

#include "globals.glsl"

layout(location = 1) rayPayloadInEXT ShadowHitPayload payload;

void main()
{
  payload.isHit = false;
}
