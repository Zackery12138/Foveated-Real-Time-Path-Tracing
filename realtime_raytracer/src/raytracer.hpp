
#pragma once
#include "hdr_sampling.hpp"
#include "nvvk/gizmos_vk.hpp"
#include "renderer.h"


#include "nvvk/resourceallocator_vk.hpp"
#include <nvvk/memallocator_dma_vk.hpp>
typedef nvvk::ResourceAllocatorDma Allocator;


#define CPP  // For sun_and_sky

#include "nvh/gltfscene.hpp"
#include "nvvkhl/appbase_vk.hpp"
#include "nvvk/debug_util_vk.hpp"
#include "nvvk/raytraceKHR_vk.hpp"
#include "nvvk/raypicker_vk.hpp"

#include "accelstruct.hpp"
#include "render_output.hpp"
#include "scene.hpp"
#include "shaders/host_device.h"

#include "imgui_internal.h"
#include "queue.hpp"

class GUI;

//--------------------------------------------------------------------------------------------------
// Simple rasterizer of OBJ objects
// - Each OBJ loaded are stored in an `ObjModel` and referenced by a `ObjInstance`
// - It is possible to have many `ObjInstance` referencing the same `ObjModel`
// - Rendering is done in an offscreen framebuffer
// - The image of the framebuffer is displayed in post-process in a full-screen quad
//
class Raytracer : public nvvkhl::AppBaseVk
{
  friend GUI;

public:
  enum RndMethod
  {
    eRtxPipeline,
    eNone,
  };

  enum Queues
  {
    eGCT0,
    eGCT1,
    eCompute,
    eTransfer
  };


  void setup(const VkInstance& instance, const VkDevice& device, const VkPhysicalDevice& physicalDevice, const std::vector<nvvk::Queue>& queues);

  bool isBusy() { return m_busy; }
  void createDescriptorSetLayout();
  void createUniformBuffer();
  void destroyResources();
  void loadAssets(const char* filename);
  void loadEnvironmentHdr(const std::string& hdrFilename);
  void loadScene(const std::string& filename);
  void onFileDrop(const char* filename) override;
  void onKeyboard(int key, int scancode, int action, int mods) override;
  void onMouseButton(int button, int action, int mods) override;
  void onMouseMotion(int x, int y) override;
  void onResize(int /*w*/, int /*h*/) override;
  
  //#Gui functions for setting and render ImGui
  void initImGui();
  void renderGui();

  void createRender(RndMethod method);
  void resetFrame();
  void updateFrame();
  void updateHdrDescriptors();
  void updateUniformBuffer(const VkCommandBuffer& cmdBuf);

  Scene              m_scene;
  AccelStructure     m_accelStruct;
  RenderOutput       m_offscreen;
  HdrSampling        m_skydome;
  nvvk::AxisVK       m_axis;
  nvvk::RayPickerKHR m_picker;

  // All renderers
  std::array<Renderer*, eNone> m_pRender{nullptr};
  RndMethod                    m_rndMethod{eNone};

  nvvk::Buffer m_sunAndSkyBuffer;

  // Graphic pipeline
  VkDescriptorPool            m_descPool{VK_NULL_HANDLE};
  VkDescriptorSetLayout       m_descSetLayout{VK_NULL_HANDLE};
  VkDescriptorSet             m_descSet{VK_NULL_HANDLE};
  nvvk::DescriptorSetBindings m_bind;

  Allocator       m_alloc;  // Allocator for buffer, images, acceleration structures
  nvvk::DebugUtil m_debug;  // Utility to name objects


  VkRect2D m_renderRegion{};
  void     setRenderRegion(const VkRect2D& size);

  // #Post
  void createOffscreenRender();
  void drawPost(VkCommandBuffer cmdBuf);

  // #VKRay
  void renderScene(const VkCommandBuffer& cmdBuf);


  RtxState m_rtxState{
      0,       // frame;
      10,      // maxDepth;
      1,       // maxSamples;
      1,       // fireflyClampThreshold;
      1,       // hdrMultiplier;
      0,       // pbrMode;
      {0, 0},  // size;
      0,       // enable Foveation
      0,       // Periphery bluring
  };

  SunAndSky m_sunAndSky{
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

  int         m_maxFrames{10000};
  bool        m_busy{false};
  std::string m_busyReasonText;


  std::shared_ptr<GUI> m_gui;
private:
    //#helper functions for Gui
    void setGuiDarkStyle();
    void initGuiDescPool();
    ImGui_ImplVulkan_InitInfo createImGuiInitInfo();


};
