#define ENVMAP 1
#define RR 1        // Using russian roulette
#define RR_DEPTH 0  // Minimum depth


#include "pbr_disney.glsl"
#include "pbr_gltf.glsl"
#include "gltf_material.glsl"
#include "punctual.glsl"
#include "env_sampling.glsl"
#include "shade_state.glsl"



//-----------------------------------------------------------------------
// Shoot a ray and return the information of the closest hit, in the
// PtPayload structure (PRD)
//
void ClosestHit(Ray r)
{
    uint rayFlags = gl_RayFlagsCullBackFacingTrianglesEXT;
    prd.hitT = INFINITY;
    traceRayEXT(topLevelAS,   // acceleration structure
        rayFlags,     // rayFlags
        0xFF,         // cullMask
        0,            // sbtRecordOffset
        0,            // sbtRecordStride
        0,            // missIndex
        r.origin,     // ray origin
        0.0,          // ray min range
        r.direction,  // ray direction
        INFINITY,     // ray max range
        0             // payload (location = 0)
    );
}


//-----------------------------------------------------------------------
// Shadow ray - return true if a ray hits anything
//
bool AnyHit(Ray r, float maxDist)
{
    shadow_payload.isHit = true;      // Asume hit, will be set to false if hit nothing (miss shader)
    shadow_payload.seed = prd.seed;  // don't care for the update - but won't affect the rahit shader
    uint rayFlags = gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT | gl_RayFlagsCullBackFacingTrianglesEXT;

    traceRayEXT(topLevelAS,   // acceleration structure
        rayFlags,     // rayFlags
        0xFF,         // cullMask
        0,            // sbtRecordOffset
        0,            // sbtRecordStride
        1,            // missIndex
        r.origin,     // ray origin
        0.0,          // ray min range
        r.direction,  // ray direction
        maxDist,      // ray max range
        1             // payload layout(location = 1)
    );

    // add to ray contribution from next event estimation
    return shadow_payload.isHit;
}


vec3 Eval(in State state, in vec3 V, in vec3 N, in vec3 L, inout float pdf)
{
  if(rtxState.pbrMode == 0)
    return DisneyEval(state, V, N, L, pdf);
  else
    return PbrEval(state, V, N, L, pdf);
}


vec3 Sample(in State state, in vec3 V, in vec3 N, inout vec3 L, inout float pdf, inout RngStateType seed)
{
  if(rtxState.pbrMode == 0)
    return DisneySample(state, V, N, L, pdf, seed);
  else
    return PbrSample(state, V, N, L, pdf, seed);
}



// Use for light/env contribution
struct VisibilityContribution
{
  vec3  radiance;   // Radiance at the point if light is visible
  vec3  lightDir;   // Direction to the light, to shoot shadow ray
  float lightDist;  // Distance to the light (1e32 for infinite or sky)
  bool  visible;    // true if in front of the face and should shoot shadow ray
};


VisibilityContribution DirectLight(in Ray r, in State state)
{
  vec3  Li = vec3(0);
  float lightPdf;
  vec3  lightContrib;
  vec3  lightDir;
  float lightDist = 1e32;
  bool  isLight   = false;

  VisibilityContribution contrib;
  contrib.radiance = vec3(0);
  contrib.visible  = false;

  // keep it simple and use either point light or environment light, each with the same
  // probability. If the environment factor is zero, we always use the point light
  // Note: see also miss shader
  float p_select_light = rtxState.hdrMultiplier > 0.0f ? 0.5f : 1.0f;

  // in general, you would select the light depending on the importance of it
  // e.g. by incorporating their luminance

  // Point lights
  if(sceneCamera.nbLights != 0 && rand(prd.seed) <= p_select_light)
  {
    isLight = true;

    // randomly select one of the lights
    int   light_index = int(min(rand(prd.seed) * sceneCamera.nbLights, sceneCamera.nbLights));
    Light light       = lights[light_index];

    vec3  pointToLight     = -light.direction;
    float rangeAttenuation = 1.0;
    float spotAttenuation  = 1.0;

    if(light.type != LightType_Directional)
    {
      pointToLight = light.position - state.position;
    }

    lightDist = length(pointToLight);

    // Compute range and spot light attenuation.
    if(light.type != LightType_Directional)
    {
      rangeAttenuation = getRangeAttenuation(light.range, lightDist);
    }
    if(light.type == LightType_Spot)
    {
      spotAttenuation = getSpotAttenuation(pointToLight, light.direction, light.outerConeCos, light.innerConeCos);
    }

    vec3 intensity = rangeAttenuation * spotAttenuation * light.intensity * light.color;

    lightContrib = intensity;
    lightDir     = normalize(pointToLight);
    lightPdf     = 1.0;
  }
  // Environment Light
  else
  {
    vec4 dirPdf = EnvSample(lightContrib);
    lightDir    = dirPdf.xyz;
    lightPdf    = dirPdf.w;
  }

  if(state.isSubsurface || dot(lightDir, state.ffnormal) > 0.0)
  {
    // We should shoot a ray toward the environment and check if it is not
    // occluded by an object before doing the following,
    // but the call to traceRayEXT have to store
    // live states (ex: sstate), which is really costly. So we will do
    // all the computation, but adding the contribution at the end of the
    // shader.
    // See: https://developer.nvidia.com/blog/best-practices-using-nvidia-rtx-ray-tracing/
    {
      BsdfSampleRec bsdfSampleRec;

      bsdfSampleRec.f = Eval(state, -r.direction, state.ffnormal, lightDir, bsdfSampleRec.pdf);

      float misWeight = isLight ? 1.0 : max(0.0, powerHeuristic(lightPdf, bsdfSampleRec.pdf));

      Li += misWeight * bsdfSampleRec.f * abs(dot(lightDir, state.ffnormal)) * lightContrib / lightPdf;
    }

    contrib.visible   = true;
    contrib.lightDir  = lightDir;
    contrib.lightDist = lightDist;
    contrib.radiance  = Li;
  }

  return contrib;
}


//-----------------------------------------------------------------------
//-----------------------------------------------------------------------
vec3 PathTrace(Ray r)
{
  vec3 radiance   = vec3(0.0);
  vec3 throughput = vec3(1.0);
  vec3 absorption = vec3(0.0);

  for(int depth = 0; depth < rtxState.maxDepth; depth++)
  {
    ClosestHit(r);

    // Hitting the environment
    if(prd.hitT == INFINITY)
    {
      vec3 env;
      if(_sunAndSky.in_use == 1)
        env = sun_and_sky(_sunAndSky, r.direction);
      else
      {
        vec2 uv = GetSphericalUv(r.direction);  // See common.glsl
        env     = texture(environmentTexture, uv).rgb;
      }
      // Done sampling return
      return radiance + (env * rtxState.hdrMultiplier * throughput);
    }


    BsdfSampleRec bsdfSampleRec;

    // Get Position, Normal, Tangents, Texture Coordinates, Color
    ShadeState sstate = GetShadeState(prd);

    State state;
    state.position       = sstate.position;
    state.normal         = sstate.normal;
    state.tangent        = sstate.tangent_u[0];
    state.bitangent      = sstate.tangent_v[0];
    state.texCoord       = sstate.text_coords[0];
    state.matID          = sstate.matIndex;
    state.isEmitter      = false;
    state.specularBounce = false;
    state.isSubsurface   = false;
    state.ffnormal       = dot(state.normal, r.direction) <= 0.0 ? state.normal : -state.normal;

    // Filling material structures
    GetMaterialsAndTextures(state, r);

    // Color at vertices
    state.mat.albedo *= sstate.color;

    // KHR_materials_unlit
    if(state.mat.unlit)
    {
      return radiance + state.mat.albedo * throughput;
    }

    // Reset absorption when ray is going out of surface
    if(dot(state.normal, state.ffnormal) > 0.0)
    {
      absorption = vec3(0.0);
    }

    // Emissive material
    radiance += state.mat.emission * throughput;

    // Add absoption (transmission / volume)
    throughput *= exp(-absorption * prd.hitT);

    // Light and environment contribution
    VisibilityContribution vcontrib = DirectLight(r, state);
    vcontrib.radiance *= throughput;

    // Sampling for the next ray
    bsdfSampleRec.f = Sample(state, -r.direction, state.ffnormal, bsdfSampleRec.L, bsdfSampleRec.pdf, prd.seed);

    // Set absorption only if the ray is currently inside the object.
    if(dot(state.ffnormal, bsdfSampleRec.L) < 0.0)
    {
      absorption = -log(state.mat.attenuationColor) / vec3(state.mat.attenuationDistance);
    }

    if(bsdfSampleRec.pdf > 0.0)
    {
      throughput *= bsdfSampleRec.f * abs(dot(state.ffnormal, bsdfSampleRec.L)) / bsdfSampleRec.pdf;
    }
    else
    {
      break;
    }


#ifdef RR
    // For Russian-Roulette (minimizing live state)
    float rrPcont = (depth >= RR_DEPTH) ?
                        min(max(throughput.x, max(throughput.y, throughput.z)) * state.eta * state.eta + 0.001, 0.95) :
                        1.0;
#endif

    // Next ray
    r.direction = bsdfSampleRec.L;
    r.origin = OffsetRay(sstate.position, dot(bsdfSampleRec.L, state.ffnormal) > 0 ? state.ffnormal : -state.ffnormal);

    // We are adding the contribution to the radiance only if the ray is not occluded by an object.
    // This is done here to minimize live state across ray-trace calls.
    if(vcontrib.visible == true)
    {
      // Shoot shadow ray up to the light (1e32 == environement)
      Ray  shadowRay = Ray(r.origin, vcontrib.lightDir);
      bool inShadow  = AnyHit(shadowRay, vcontrib.lightDist);
      if(!inShadow)
      {
        radiance += vcontrib.radiance;
      }
    }


#ifdef RR
    if(rand(prd.seed) >= rrPcont)
      break;                // paths with low throughput that won't contribute
    throughput /= rrPcont;  // boost the energy of the non-terminated paths
#endif
  }


  return radiance;
}


vec3 samplePixel(ivec2 imageCoords, ivec2 sizeImage)
{
    // Compute ray origin using the camera's inverse view matrix.
    vec4 origin = sceneCamera.viewInverse * vec4(0, 0, 0, 1);

    // Antialiasing: Send the ray through a random position inside the pixel each frame.
    vec2 jitterOffset = rtxState.frame == 0 ? vec2(0.5f, 0.5f) : vec2(rand(prd.seed), rand(prd.seed));
    //vec2 jitterOffset = vec2(0);
    vec2 uv = (vec2(imageCoords) + jitterOffset) / vec2(sizeImage.xy) * 2.0 - 1.0;

    // Convert the UV coordinates to a point in the camera's frustum.
    vec4 targetInFrustum = sceneCamera.projInverse * vec4(uv.x, uv.y, 1, 1);

    // Compute ray direction using the camera's inverse view matrix.
    vec3 direction = normalize((sceneCamera.viewInverse * vec4(targetInFrustum.xyz, 0)).xyz);

    // Create a ray with the calculated origin and direction.
    Ray ray = Ray(origin.xyz, direction);

    // Calculate the color contribution from the ray.
    vec3 radiance = PathTrace(ray);

    // Firefly removal: Clamp extremely bright pixels to reduce noise.
    float luminance = dot(radiance, vec3(0.212671f, 0.715160f, 0.072169f));
    if (luminance > rtxState.fireflyClampThreshold)
    {
        radiance *= rtxState.fireflyClampThreshold / luminance;
    }

    return radiance;
}
