// post processing - Tonemapping
#version 460
#extension GL_GOOGLE_include_directive : enable
#extension GL_ARB_gpu_shader_int64 : enable  


#include "random.glsl"
#include "host_device.h"


layout(location = 0) in vec2 uvCoords;
layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 0) uniform sampler2D inImage;

layout(push_constant) uniform _Tonemapper
{
  Tonemapper tm;
};



// linear to sRGB converting
// http://chilliant.blogspot.com/2012/08/srgb-approximations-for-hlsl.html
vec3 linearTosRGB(vec3 color)
{
  return pow(color, vec3(1.0 / 2.2));
}
// sRGB to linear converting
// see http://chilliant.blogspot.com/2012/08/srgb-approximations-for-hlsl.html
vec3 sRGBToLinear(vec3 srgb)
{
  return pow(srgb, vec3(2.2));
}
vec4 sRGBToLinear(vec4 srgb)
{
  return vec4(sRGBToLinear(srgb.xyz), srgb.w);
}

// http://user.ceng.metu.edu.tr/~akyuz/files/hdrgpu.pdf
const mat3 RGB2XYZ = mat3(0.4124564, 0.3575761, 0.1804375, 
                          0.2126729, 0.7151522, 0.0721750, 
                          0.0193339, 0.1191920, 0.9503041);

float      luminance(vec3 color)
{
  return dot(color, vec3(0.2126f, 0.7152f, 0.0722f));  
}

vec3 toneExposure(vec3 RGB, float logAvgLum)
{
  vec3  XYZ = RGB2XYZ * RGB;
  float Y   = (0.5f / logAvgLum) * XYZ.y;
  float Yd  = (Y * (1.0 + Y / 0.25f)) / (1.0 + Y);
  return RGB / XYZ.y * Yd;
}


// For Uncharted2 tonemapping
const float A = 0.15;
const float B = 0.50;
const float C = 0.10;
const float D = 0.20;
const float E = 0.02;
const float F = 0.30;

// Uncharted 2 tone map
// see: http://filmicworlds.com/blog/filmic-tonemapping-operators/
vec3 Uncharted2Tonemap(vec3 x)
{
  return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

vec3 toneMap(vec3 color, float u_Exposure)
{
  const float W   = 11.2;
  color *= u_Exposure;
  return linearTosRGB(Uncharted2Tonemap(color * 2.0) * (1.0 / Uncharted2Tonemap(vec3(W)))); 
}


void main()
{
  // ray tracing output image
  vec4 hdr = texture(inImage, uvCoords).rgba;

  if(tm.autoExposure == 1)
  {
    // Get the average value of the image
    vec4  avgImage     = textureLod(inImage, vec2(0.5), 20);  
    float avgLuminance = luminance(avgImage.rgb);                  
    hdr.rgb = toneExposure(hdr.rgb, avgLuminance);  // Adjust exposure
  }

  // Tonemapping
  vec3 color = toneMap(hdr.rgb, tm.avgLum);
  //vec3 color = hdr.rgb;

   //contrast, brightness and saturation all in one.
  vec3 i = vec3(dot(color, vec3(0.299, 0.587, 0.114)));
  color  = mix(i, color, tm.saturation);
  color = clamp(mix(vec3(0.5), color, tm.contrast), 0, 1);
  color = pow(color, vec3(1.0 / tm.brightness));

  
  fragColor.xyz = color;
  fragColor.a   = hdr.a;
}
