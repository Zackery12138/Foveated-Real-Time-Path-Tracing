/*
 * Main class to render the scene, holds sub-classes for various work
 */


#define VMA_IMPLEMENTATION

#include "shaders/host_device.h"
#include "rtx_pipeline.hpp"
#include "raytracer.hpp"
#include "gui.hpp"
#include "tools.hpp"

#include "nvml_monitor.hpp"
#include "fileformats/tiny_gltf_freeimage.h"



//--------------------------------------------------------------------------------------------------
// Keep the handle on the device
// Initialize the tool to do all our allocations: buffers, images
//
void Raytracer::setup(const VkInstance&               instance,
                          const VkDevice&                 device,
                          const VkPhysicalDevice&         physicalDevice,
                          const std::vector<nvvk::Queue>& queues)
{
  AppBaseVk::setup(instance, device, physicalDevice, queues[eGCT0].familyIndex);

  m_gui = std::make_shared<GUI>(this);  // GUI of this class

  // Memory allocator for buffers and images
  m_alloc.init(instance, device, physicalDevice);

  m_debug.setup(m_device);

  // Compute queues can be use for acceleration structures
  m_picker.setup(m_device, physicalDevice, queues[eCompute].familyIndex, &m_alloc);
  m_accelStruct.setup(m_device, physicalDevice, queues[eCompute].familyIndex, &m_alloc);

  // Note: the GTC family queue is used because the nvvk::cmdGenerateMipmaps uses vkCmdBlitImage and this
  // command requires graphic queue and not only transfer.
  m_scene.setup(m_device, physicalDevice, queues[eGCT1], &m_alloc);

  // Transfer queues can be use for the creation of the following assets
  m_offscreen.setup(m_device, physicalDevice, queues[eTransfer].familyIndex, &m_alloc);

  m_skydome.setup(device, physicalDevice, queues[eTransfer].familyIndex, &m_alloc);

  // Create and setup renderer
  m_pRender[eRtxPipeline] = new RtxPipeline;
  for(auto r : m_pRender)
  {
    r->setup(m_device, physicalDevice, queues[eTransfer].familyIndex, &m_alloc);
  }
}


//--------------------------------------------------------------------------------------------------
// Loading the scene file, setting up all scene buffers, create the acceleration structures
// for the loaded models.
//
void Raytracer::loadScene(const std::string& filename)
{
  m_scene.load(filename);
  m_accelStruct.create(m_scene.getScene(), m_scene.getBuffers(Scene::eVertex), m_scene.getBuffers(Scene::eIndex));

  // The picker is the helper to return information from a ray hit under the mouse cursor
  m_picker.setTlas(m_accelStruct.getTlas());
  resetFrame();
}

//--------------------------------------------------------------------------------------------------
// Loading an HDR image and creating the importance sampling acceleration structure
//
void Raytracer::loadEnvironmentHdr(const std::string& hdrFilename)
{
  MilliTimer timer;
  LOGI("Loading HDR and converting %s\n", hdrFilename.c_str());
  m_skydome.loadEnvironment(hdrFilename);
  timer.print();

  m_rtxState.fireflyClampThreshold = m_skydome.getIntegral() * 4.f;  // magic
}


//--------------------------------------------------------------------------------------------------
// Loading asset in a separate thread
// - Used by file drop and loadscene operation
// Marking the session as busy, to avoid calling rendering while loading assets
//
void Raytracer::loadAssets(const char* filename)
{
  std::string sfile = filename;

  // Need to stop current rendering
  m_busy = true;
  vkDeviceWaitIdle(m_device);

  std::thread([&, sfile]() {
    LOGI("Loading: %s\n", sfile.c_str());

    // Supporting only GLTF and HDR files
    namespace fs          = std::filesystem;
    std::string extension = fs::path(sfile).extension().string();
    if(extension == ".gltf" || extension == ".glb")
    {
      m_busyReasonText = "Loading scene ";

      // Loading scene and creating acceleration structure
      loadScene(sfile);

      // Loading the scene might have loaded new textures, which is changing the number of elements
      // in the DescriptorSetLayout. Therefore, the PipelineLayout will be out-of-date and need
      // to be re-created. If they are re-created, the pipeline also need to be re-created.
      for(auto& r : m_pRender)
        r->destroy();

      m_pRender[m_rndMethod]->create(
          m_size, {m_accelStruct.getDescLayout(), m_offscreen.getDescLayout(), m_scene.getDescLayout(), m_descSetLayout}, &m_scene);
    }

    if(extension == ".hdr")  //|| extension == ".exr")
    {
      m_busyReasonText = "Loading HDR ";
      loadEnvironmentHdr(sfile);
      updateHdrDescriptors();
    }


    // Re-starting the frame count to 0
    Raytracer::resetFrame();
    m_busy = false;
  }).detach();
}


//--------------------------------------------------------------------------------------------------
// Called at each frame to update the UBO: scene, camera, environment (sun&sky)
//
void Raytracer::updateUniformBuffer(const VkCommandBuffer& cmdBuf)
{
  if(m_busy)
    return;

  LABEL_SCOPE_VK(cmdBuf);
  const float aspectRatio = m_renderRegion.extent.width / static_cast<float>(m_renderRegion.extent.height);

  m_scene.updateCamera(cmdBuf, aspectRatio);
  vkCmdUpdateBuffer(cmdBuf, m_sunAndSkyBuffer.buffer, 0, sizeof(SunAndSky), &m_sunAndSky);
}

//--------------------------------------------------------------------------------------------------
// If the camera matrix has changed, resets the frame otherwise, increments frame.
//
void Raytracer::updateFrame()
{
  static nvmath::mat4f refCamMatrix;
  static float         fov = 0;

  auto& m = CameraManip.getMatrix();
  auto  f = CameraManip.getFov();
  if(memcmp(&refCamMatrix.a00, &m.a00, sizeof(nvmath::mat4f)) != 0 || f != fov)
  {
    resetFrame();
    refCamMatrix = m;
    fov          = f;
  }

  if(m_rtxState.frame < m_maxFrames)
    m_rtxState.frame++;
}

//--------------------------------------------------------------------------------------------------
// Reset frame is re-starting the raytracing
//
void Raytracer::resetFrame()
{
    //Since resetFrame will be called before updateFrame increments the frame counter, 
    //resetFrame will set the frame counter to -1:
  m_rtxState.frame = -1;
}

//--------------------------------------------------------------------------------------------------
// Descriptors for the Sun&Sky buffer
//
void Raytracer::createDescriptorSetLayout()
{
  VkShaderStageFlags flags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR
                             | VK_SHADER_STAGE_ANY_HIT_BIT_KHR | VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;


  m_bind.addBinding({EnvBindings::eSunSky, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_MISS_BIT_KHR | flags});
  m_bind.addBinding({EnvBindings::eHdr, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, flags});  // HDR image
  m_bind.addBinding({EnvBindings::eImpSamples, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flags});   // importance sampling


  m_descPool = m_bind.createPool(m_device, 1);
  CREATE_NAMED_VK(m_descSetLayout, m_bind.createLayout(m_device));
  CREATE_NAMED_VK(m_descSet, nvvk::allocateDescriptorSet(m_device, m_descPool, m_descSetLayout));

  // Using the environment
  std::vector<VkWriteDescriptorSet> writes;
  VkDescriptorBufferInfo            sunskyDesc{m_sunAndSkyBuffer.buffer, 0, VK_WHOLE_SIZE};
  VkDescriptorBufferInfo            accelImpSmpl{m_skydome.m_accelImpSmpl.buffer, 0, VK_WHOLE_SIZE};
  writes.emplace_back(m_bind.makeWrite(m_descSet, EnvBindings::eSunSky, &sunskyDesc));
  writes.emplace_back(m_bind.makeWrite(m_descSet, EnvBindings::eHdr, &m_skydome.m_texHdr.descriptor));
  writes.emplace_back(m_bind.makeWrite(m_descSet, EnvBindings::eImpSamples, &accelImpSmpl));

  vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

//--------------------------------------------------------------------------------------------------
// Setting the descriptor for the HDR and its acceleration structure
//
void Raytracer::updateHdrDescriptors()
{
  std::vector<VkWriteDescriptorSet> writes;
  VkDescriptorBufferInfo            accelImpSmpl{m_skydome.m_accelImpSmpl.buffer, 0, VK_WHOLE_SIZE};

  writes.emplace_back(m_bind.makeWrite(m_descSet, EnvBindings::eHdr, &m_skydome.m_texHdr.descriptor));
  writes.emplace_back(m_bind.makeWrite(m_descSet, EnvBindings::eImpSamples, &accelImpSmpl));
  vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

//--------------------------------------------------------------------------------------------------
// Creating the uniform buffer holding the Sun&Sky structure
// - Buffer is host visible and will be set each frame
//
void Raytracer::createUniformBuffer()
{
  m_sunAndSkyBuffer = m_alloc.createBuffer(sizeof(SunAndSky), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  NAME_VK(m_sunAndSkyBuffer.buffer);
}

//--------------------------------------------------------------------------------------------------
// Destroying all allocations
//
void Raytracer::destroyResources()
{
  // Resources
  m_alloc.destroy(m_sunAndSkyBuffer);

  // Descriptors
  vkDestroyDescriptorPool(m_device, m_descPool, nullptr);
  vkDestroyDescriptorSetLayout(m_device, m_descSetLayout, nullptr);

  // Other
  m_picker.destroy();
  m_scene.destroy();
  m_accelStruct.destroy();
  m_offscreen.destroy();
  m_skydome.destroy();
  m_axis.deinit();

  // All renderers
  for(auto p : m_pRender)
  {
    p->destroy();
    p = nullptr;
  }

  // Memory
  m_alloc.deinit();
}

//--------------------------------------------------------------------------------------------------
// Handling resize of the window
//
void Raytracer::onResize(int /*w*/, int /*h*/)
{
  m_offscreen.update(m_size);
  resetFrame();
}

void Raytracer::initImGui()
{
 // UI
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // Avoiding the INI file
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;      // Enable Docking
    // ImGui Fonts setup
    io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/consola.ttf", 12.0f); 
    
    //set Gui to dark style
    setGuiDarkStyle();

    initGuiDescPool();

    ImGui_ImplVulkan_InitInfo initInfo = createImGuiInitInfo();

    ImGui_ImplVulkan_Init(&initInfo, m_renderPass);
    
    // Upload Fonts
    VkCommandBuffer cmdbuf = createTempCmdBuffer();
    ImGui_ImplVulkan_CreateFontsTexture(cmdbuf);
    submitTempCmdBuffer(cmdbuf);
    
}

void Raytracer::setGuiDarkStyle()
{
    // Setup Dear ImGui style
    ImGui::StyleColorsDark();
    //ImGui::StyleColorsClassic();

    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.WindowBorderSize = 0.0f;
    style.ColorButtonPosition = ImGuiDir_Right;
    style.FrameRounding = 2.0f;
    style.FrameBorderSize = 1.0f;
    style.GrabRounding = 4.0f;
    style.IndentSpacing = 12.0f;

    auto& colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4{ 0.1f, 0.105f, 0.11f, 1.0f };
    colors[ImGuiCol_Header] = ImVec4{ 0.2f, 0.205f, 0.21f, 1.0f };
    colors[ImGuiCol_HeaderHovered] = ImVec4{ 0.3f, 0.305f, 0.31f, 1.0f };
    colors[ImGuiCol_HeaderActive] = ImVec4{ 0.15f, 0.1505f, 0.151f, 1.0f };

    colors[ImGuiCol_Button] = ImVec4{ 0.2f, 0.205f, 0.21f, 1.0f };
    colors[ImGuiCol_ButtonHovered] = ImVec4{ 0.3f, 0.305f, 0.31f, 1.0f }; 
    colors[ImGuiCol_ButtonActive] = ImVec4{ 0.15f, 0.1505f, 0.151f, 1.0f }; 

    colors[ImGuiCol_FrameBg] = ImVec4{ 0.2f, 0.205f, 0.21f, 1.0f }; 
    colors[ImGuiCol_FrameBgHovered] = ImVec4{ 0.3f, 0.305f, 0.31f, 1.0f }; 
    colors[ImGuiCol_FrameBgActive] = ImVec4{ 0.15f, 0.1505f, 0.151f, 1.0f };

    colors[ImGuiCol_TitleBg] = ImVec4{ 0.15f, 0.1505f, 0.151f, 1.0f };
    colors[ImGuiCol_TitleBgActive] = ImVec4{ 0.15f, 0.1505f, 0.151f, 1.0f };
    colors[ImGuiCol_TitleBgCollapsed] = ImVec4{ 0.15f, 0.1505f, 0.151f, 1.0f };
    colors[ImGuiCol_Tab] = ImVec4{ 0.15f, 0.1505f, 0.151f, 1.0f }; 
    colors[ImGuiCol_TabHovered] = ImVec4{ 0.38f, 0.3805f, 0.381f, 1.0f }; 
    colors[ImGuiCol_TabActive] = ImVec4{ 0.28f, 0.2805f, 0.281f, 1.0f }; 

    colors[ImGuiCol_ResizeGrip] = ImVec4(0.91f, 0.91f, 0.91f, 0.25f);
    colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.81f, 0.81f, 0.81f, 0.67f);
    colors[ImGuiCol_ResizeGripActive] = ImVec4(0.46f, 0.46f, 0.46f, 0.95f);

    colors[ImGuiCol_SliderGrab] = ImVec4(0.51f, 0.51f, 0.51f, 0.7f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.66f, 0.66f, 0.66f, 1.0f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.94f, 0.94f, 0.94f, 1.0f);
}

 
void Raytracer::initGuiDescPool()
{
    std::vector<VkDescriptorPoolSize> poolSize{ {VK_DESCRIPTOR_TYPE_SAMPLER, 1}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1} }; 
    VkDescriptorPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO }; 
    poolInfo.maxSets = 2; 
    poolInfo.poolSizeCount = 2; 
    poolInfo.pPoolSizes = poolSize.data(); 
    vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_imguiDescPool); 
}

ImGui_ImplVulkan_InitInfo Raytracer::createImGuiInitInfo()
{
    ImGui_ImplVulkan_InitInfo init_info = {}; 
    init_info.Instance = m_instance; 
    init_info.PhysicalDevice = m_physicalDevice; 
    init_info.Device = m_device; 
    init_info.QueueFamily = m_graphicsQueueIndex; 
    init_info.Queue = m_queue;
    init_info.PipelineCache = VK_NULL_HANDLE;
    init_info.DescriptorPool = m_imguiDescPool;
    init_info.Subpass = 0;
    init_info.MinImageCount = 2;
    init_info.ImageCount = static_cast<int>(m_swapChain.getImageCount());
    init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init_info.CheckVkResultFn = nullptr;
    init_info.Allocator = nullptr;

#ifdef VK_KHR_dynamic_rendering
    VkPipelineRenderingCreateInfoKHR rfInfo{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR };
    if (m_useDynamicRendering)
    {
        rfInfo.colorAttachmentCount = 1;
        rfInfo.pColorAttachmentFormats = &m_colorFormat;
        rfInfo.depthAttachmentFormat = m_depthFormat;
        rfInfo.stencilAttachmentFormat = m_depthFormat;
        init_info.rinfo = &rfInfo;
    }
#endif
    return init_info;
}

//--------------------------------------------------------------------------------------------------
// Call the rendering of all graphical user interface
//
void Raytracer::renderGui()
{
  m_gui->render();

}


//--------------------------------------------------------------------------------------------------
// Creating the render
// - Destroy the previous one.
void Raytracer::createRender(RndMethod method)
{
  if(method == m_rndMethod)
    return;

  LOGI("Switching renderer, from %d to %d \n", m_rndMethod, method);
  if(m_rndMethod != eNone)
  {
    vkDeviceWaitIdle(m_device);  // cannot destroy while in use
    m_pRender[m_rndMethod]->destroy();
  }
  m_rndMethod = method;

  m_pRender[m_rndMethod]->create(
      m_size, {m_accelStruct.getDescLayout(), m_offscreen.getDescLayout(), m_scene.getDescLayout(), m_descSetLayout}, &m_scene);
}

//--------------------------------------------------------------------------------------------------
// The GUI is taking space and size of the rendering area is smaller than the viewport
// This is the space left in the center view.
void Raytracer::setRenderRegion(const VkRect2D& size)
{
  if(memcmp(&m_renderRegion, &size, sizeof(VkRect2D)) != 0)
    resetFrame();
  m_renderRegion = size;
}

//////////////////////////////////////////////////////////////////////////
// Post ray tracing
//////////////////////////////////////////////////////////////////////////

void Raytracer::createOffscreenRender()
{
  m_offscreen.create(m_size, m_renderPass);
  m_axis.init(m_device, m_renderPass, 0, 50.0f);
}

//--------------------------------------------------------------------------------------------------
// This will draw the result of the rendering and apply the tonemapper.
// If enabled, draw orientation axis in the lower left corner.
void Raytracer::drawPost(VkCommandBuffer cmdBuf)
{
  LABEL_SCOPE_VK(cmdBuf);
  auto size = nvmath::vec2f(m_size.width, m_size.height);
  auto area = nvmath::vec2f(m_renderRegion.extent.width, m_renderRegion.extent.height);

  VkViewport viewport{static_cast<float>(m_renderRegion.offset.x),
                      static_cast<float>(m_renderRegion.offset.y),
                      static_cast<float>(m_size.width),
                      static_cast<float>(m_size.height),
                      0.0f,
                      1.0f};
  VkRect2D   scissor{m_renderRegion.offset, {m_renderRegion.extent.width, m_renderRegion.extent.height}};
  vkCmdSetViewport(cmdBuf, 0, 1, &viewport);
  vkCmdSetScissor(cmdBuf, 0, 1, &scissor);

  m_offscreen.run(cmdBuf);

}

//////////////////////////////////////////////////////////////////////////
// Ray tracing
//////////////////////////////////////////////////////////////////////////

void Raytracer::renderScene(const VkCommandBuffer& cmdBuf)
{


  if(m_busy)
  {
    m_gui->showBusyWindow();  // Busy while loading scene
    return;
  }

  LABEL_SCOPE_VK(cmdBuf);


  // We are done rendering
  if(m_rtxState.frame >= m_maxFrames)
    return;

  // Handling de-scaling by reducing the size to render
  VkExtent2D render_size = m_renderRegion.extent;

  m_rtxState.size = {render_size.width, render_size.height};
  // State is the push constant structure
  m_pRender[m_rndMethod]->setPushContants(m_rtxState);
  // Running the renderer
  m_pRender[m_rndMethod]->run(cmdBuf, render_size, {m_accelStruct.getDescSet(), m_offscreen.getDescSet(), m_scene.getDescSet(), m_descSet});


  // For automatic brightness tonemapping
  if(m_offscreen.m_tonemapper.autoExposure)
  {
    m_offscreen.genMipmap(cmdBuf);
  }
}


//////////////////////////////////////////////////////////////////////////
// Keyboard / Drag and Drop
//////////////////////////////////////////////////////////////////////////

//--------------------------------------------------------------------------------------------------
// Overload keyboard hit
// - Home key: fit all, the camera will move to see the entire scene bounding box
// - Space: Trigger ray picking and set the interest point at the intersection
//          also return all information under the cursor
//
void Raytracer::onKeyboard(int key, int scancode, int action, int mods)
{
  nvvkhl::AppBaseVk::onKeyboard(key, scancode, action, mods);

  if(m_busy || action == GLFW_RELEASE)
    return;

  switch(key)
  {
    case GLFW_KEY_HOME:
    case GLFW_KEY_F:  // Set the camera as to see the model
      fitCamera(m_scene.getScene().m_dimensions.min, m_scene.getScene().m_dimensions.max, false);
      break;
    case GLFW_KEY_R:
      resetFrame();
      break;
    default:
      break;
  }
}


//--------------------------------------------------------------------------------------------------
//
//
void Raytracer::onFileDrop(const char* filename)
{
  if (m_busy)
    return;

  loadAssets(filename);
}

//--------------------------------------------------------------------------------------------------
// Window callback when the mouse move
// - Handling ImGui and a default camera
//
void Raytracer::onMouseMotion(int x, int y)
{
  AppBaseVk::onMouseMotion(x, y);
  if(m_busy)
    return;

  if(ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureKeyboard)
    return;
}

//--------------------------------------------------------------------------------------------------
//
//
void Raytracer::onMouseButton(int button, int action, int mods)
{
  AppBaseVk::onMouseButton(button, action, mods);
}
