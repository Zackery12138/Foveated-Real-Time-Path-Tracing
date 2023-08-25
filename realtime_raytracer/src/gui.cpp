
#include <bitset>  // std::bitset
#include <iomanip>
#include <sstream>

#include "nvmath/nvmath.h"

#include "imgui_helper.h"
#include "imgui_orient.h"
#include "rtx_pipeline.hpp"
#include "raytracer.hpp"
#include "gui.hpp"
#include "tools.hpp"

#include "nvml_monitor.hpp"


using GuiH = ImGuiH::Control;



// render all gui
void GUI::render()
{
  // Show UI panel window.
  float panelAlpha = 1.0f;
  if(_se->showGui())
  {
    ImGuiH::Control::style.ctrlPerc = 0.55f;
    ImGuiH::Panel::Begin(ImGuiH::Panel::Side::Left, panelAlpha);

    bool changed{false};

    if(ImGui::CollapsingHeader("Camera"), ImGuiTreeNodeFlags_DefaultOpen)
      changed |= ImGuiH::CameraWidget();
    if(ImGui::CollapsingHeader("Ray Tracing" ), ImGuiTreeNodeFlags_DefaultOpen)
      changed |= guiRayTracing();
    if(ImGui::CollapsingHeader("Post-Processing" ), ImGuiTreeNodeFlags_DefaultOpen)
      changed |= guiTonemapper();
    if(ImGui::CollapsingHeader("Environment" ))
      changed |= guiEnvironment();

    if(ImGui::Button("Load Scene"))
    {
        loadSceneWindow();
    }

    ImGui::TextWrapped("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate,
                       ImGui::GetIO().Framerate);

    if(changed)
    {
      _se->resetFrame();
    }

    ImGui::End();  // ImGui::Panel::end()
  }

  // Rendering region is different if the side panel is visible
  if(panelAlpha >= 1.0f && _se->showGui())
  {
    ImVec2 pos, size;
    ImGuiH::Panel::CentralDimension(pos, size);
    _se->setRenderRegion(VkRect2D{VkOffset2D{static_cast<int32_t>(pos.x), static_cast<int32_t>(pos.y)},
                                  VkExtent2D{static_cast<uint32_t>(size.x), static_cast<uint32_t>(size.y)}});
  }
  else
  {
    _se->setRenderRegion(VkRect2D{{}, _se->getSize()});
  }
}


bool GUI::guiRayTracing()
{
  auto  Normal = ImGuiH::Control::Flags::Normal;
  bool  changed = false;
  auto& rtxState(_se->m_rtxState);

  changed |= GuiH::Checkbox("Enable Foveation", "", (bool*)&rtxState.enableFoveation, nullptr);
  changed |= GuiH::Checkbox("Periphery Blur", "", (bool*)&rtxState.enablePeripheryBlur, nullptr);
  changed |= GuiH::Slider("Max Ray Depth", "", &rtxState.maxDepth, nullptr, Normal, 1, 10);
  changed |= GuiH::Slider("Samples Per Frame", "", &rtxState.maxSamples, nullptr, Normal, 1, 10);
  changed |= GuiH::Slider("Max Iteration ", "", &_se->m_maxFrames, nullptr, Normal, 1, 100000);

  changed |= GuiH::Selection("Pbr Mode", "PBR material model", &rtxState.pbrMode, nullptr, Normal, {"Disney", "Gltf"});

  static bool bAnyHit = true;
  if(_se->m_rndMethod == Raytracer::RndMethod::eRtxPipeline)
  {
    if(GuiH::Checkbox("Enable AnyHit", "AnyHit is used for double sided, cutout opacity, but can be slower when all objects are opaque",
                      &bAnyHit, nullptr))
    {
      auto rtx = dynamic_cast<RtxPipeline*>(_se->m_pRender[_se->m_rndMethod]);
      vkDeviceWaitIdle(_se->m_device);  // cannot run while changing this
      rtx->useAnyHit(bAnyHit);
      changed = true;
    }
  }

  GuiH::Info("Frame", "", std::to_string(rtxState.frame), GuiH::Flags::Disabled);
  return changed;
}


bool GUI::guiTonemapper()
{
  static Tonemapper default_tm{
      1.0f,          // brightness;
      1.0f,          // contrast;
      1.0f,          // saturation;
      1.0f,          // avgLum;
      0             // autoExposure;
  };

  auto&     tm = _se->m_offscreen.m_tonemapper;
  bool      changed{false};
 
  changed |= GuiH::Checkbox("Auto Exposure", "", (bool*)&tm.autoExposure);
  changed |= GuiH::Slider("Exposure", "", &tm.avgLum, &default_tm.avgLum, GuiH::Flags::Normal, 0.001f, 5.00f);
  changed |= GuiH::Slider("Brightness", "", &tm.brightness, &default_tm.brightness, GuiH::Flags::Normal, 0.0f, 2.0f);
  changed |= GuiH::Slider("Contrast", "", &tm.contrast, &default_tm.contrast, GuiH::Flags::Normal, 0.0f, 2.0f);
  changed |= GuiH::Slider("Saturation", "", &tm.saturation, &default_tm.saturation, GuiH::Flags::Normal, 0.0f, 5.0f);

  return false;  // no need to restart the renderer
}

//--------------------------------------------------------------------------------------------------
//
//
bool GUI::guiEnvironment()
{
  static SunAndSky dss{
      {1, 1, 1},            // rgb_unit_conversion;
      0.0000101320f,        // multiplier;
      0.0f,                 // haze;
      0.0f,                 // redblueshift;
      1.0f,                 // saturation;
      0.0f,                 // horizon_height;
      {0.4f, 0.4f, 0.4f},   // ground_color;
      0.1f,                 // horizon_blur;
      {0.0, 0.0, 0.01f},    // night_color;
      0.8f,                 // sun_disk_intensity;
      {0.00, 0.78, 0.62f},  // sun_direction;
      5.0f,                 // sun_disk_scale;
      1.0f,                 // sun_glow_intensity;
      1,                    // y_is_up;
      1,                    // physically_scaled_sun;
      0,                    // in_use;
  };

  bool  changed{false};
  auto& sunAndSky(_se->m_sunAndSky);

  changed |= ImGui::Checkbox("Use Sun & Sky", (bool*)&sunAndSky.in_use);
  changed |= GuiH::Slider("Exposure", "Intensity of the environment", &_se->m_rtxState.hdrMultiplier, nullptr,
                          GuiH::Flags::Normal, 0.f, 5.f);

  // Adjusting the up with the camera
  nvmath::vec3f eye, center, up;
  CameraManip.getLookat(eye, center, up);
  sunAndSky.y_is_up = (up.y == 1);

  if(sunAndSky.in_use)
  {
    GuiH::Group<bool>("Sun", true, [&] {
      changed |= GuiH::Custom("Direction", "Sun Direction", [&] {
        float indent = ImGui::GetCursorPos().x;
        changed |= ImGui::DirectionGizmo("", &sunAndSky.sun_direction.x, true);
        ImGui::NewLine();
        ImGui::SameLine(indent);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        changed |= ImGui::InputFloat3("##IG", &sunAndSky.sun_direction.x);
        return changed;
      });
      changed |= GuiH::Slider("Disk Scale", "", &sunAndSky.sun_disk_scale, &dss.sun_disk_scale, GuiH::Flags::Normal, 0.f, 100.f);
      changed |= GuiH::Slider("Glow Intensity", "", &sunAndSky.sun_glow_intensity, &dss.sun_glow_intensity,
                              GuiH::Flags::Normal, 0.f, 5.f);
      changed |= GuiH::Slider("Disk Intensity", "", &sunAndSky.sun_disk_intensity, &dss.sun_disk_intensity,
                              GuiH::Flags::Normal, 0.f, 5.f);
      changed |= GuiH::Color("Night Color", "", &sunAndSky.night_color.x, &dss.night_color.x, GuiH::Flags::Normal);
      return changed;
    });

    GuiH::Group<bool>("Ground", true, [&] {
      changed |= GuiH::Slider("Horizon Height", "", &sunAndSky.horizon_height, &dss.horizon_height, GuiH::Flags::Normal, -1.f, 1.f);
      changed |= GuiH::Slider("Horizon Blur", "", &sunAndSky.horizon_blur, &dss.horizon_blur, GuiH::Flags::Normal, 0.f, 1.f);
      changed |= GuiH::Color("Ground Color", "", &sunAndSky.ground_color.x, &dss.ground_color.x, GuiH::Flags::Normal);
      changed |= GuiH::Slider("Haze", "", &sunAndSky.haze, &dss.haze, GuiH::Flags::Normal, 0.f, 15.f);
      return changed;
    });

    GuiH::Group<bool>("Other", false, [&] {
      changed |= GuiH::Drag("Multiplier", "", &sunAndSky.multiplier, &dss.multiplier, GuiH::Flags::Normal, 0.f,
                            std::numeric_limits<float>::max(), 2, "%5.5f");
      changed |= GuiH::Slider("Saturation", "", &sunAndSky.saturation, &dss.saturation, GuiH::Flags::Normal, 0.f, 1.f);
      changed |= GuiH::Slider("Red Blue Shift", "", &sunAndSky.redblueshift, &dss.redblueshift, GuiH::Flags::Normal, -1.f, 1.f);
      changed |= GuiH::Color("RGB Conversion", "", &sunAndSky.rgb_unit_conversion.x, &dss.rgb_unit_conversion.x, GuiH::Flags::Normal);

      nvmath::vec3f eye, center, up;
      CameraManip.getLookat(eye, center, up);
      sunAndSky.y_is_up = up.y == 1;
      changed |= GuiH::Checkbox("Y is Up", "", (bool*)&sunAndSky.y_is_up, nullptr, GuiH::Flags::Disabled);
      return changed;
    });
  }

  return changed;
}

void GUI::loadSceneWindow()
{
    auto openFilename = [](const char* filter) {
#ifdef _WIN32
        char          filename[MAX_PATH] = { 0 };
        OPENFILENAMEA ofn;
        ZeroMemory(&filename, sizeof(filename));
        ZeroMemory(&ofn, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = nullptr;  
        ofn.lpstrFilter = filter;
        ofn.lpstrFile = filename;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrTitle = "Select a File";
        ofn.Flags = OFN_DONTADDTORECENT | OFN_FILEMUSTEXIST;

        if (GetOpenFileNameA(&ofn))
        {
            return std::string(filename);
        }
#endif

        return std::string("");
    };

    _se->loadAssets(openFilename("GLTF Files\0 * .gltf; *.glb\0\0").c_str());
}


//--------------------------------------------------------------------------------------------------
// Display a static window when loading assets
//
void GUI::showBusyWindow()
{
  static int   nb_dots   = 0;
  static float deltaTime = 0;
  bool         show      = true;
  size_t       width     = 270;
  size_t       height    = 60;

  deltaTime += ImGui::GetIO().DeltaTime;
  if(deltaTime > .25)
  {
    deltaTime = 0;
    nb_dots   = ++nb_dots % 10;
  }

  ImGui::SetNextWindowSize(ImVec2(float(width), float(height)));
  ImGui::SetNextWindowPos(ImVec2(float(_se->m_size.width - width) * 0.5f, float(_se->m_size.height - height) * 0.5f));

  ImGui::SetNextWindowBgAlpha(0.75f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 15.0);
  if(ImGui::Begin("##notitle", &show,
                  ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing
                      | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMouseInputs))
  {
    ImVec2 available = ImGui::GetContentRegionAvail();

    ImVec2 text_size = ImGui::CalcTextSize(_se->m_busyReasonText.c_str(), nullptr, false, available.x);

    ImVec2 pos = ImGui::GetCursorPos();
    pos.x += (available.x - text_size.x) * 0.5f;
    pos.y += (available.y - text_size.y) * 0.5f;

    ImGui::SetCursorPos(pos);
    ImGui::TextWrapped("%s", (_se->m_busyReasonText + std::string(nb_dots, '.')).c_str());
  }
  ImGui::PopStyleVar();
  ImGui::End();
}
