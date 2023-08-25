
#include <thread>

#define IMGUI_DEFINE_MATH_OPERATORS
#include "backends/imgui_impl_glfw.h"
#include "nvh/cameramanipulator.hpp"
#include "nvh/fileoperations.hpp"
#include "nvh/inputparser.h"
#include "nvvk/context_vk.hpp"
#include "raytracer.hpp"

// Default search path for shaders
std::vector<std::string> defaultSearchPaths;


static int const WINDOW_WIDTH  = 1280;
static int const WINDOW_HEIGHT = 720;

//--------------------------------------------------------------------------------------------------
// Entry point
//
int main(int argc, char** argv)
{
  InputParser parser(argc, argv);
  std::string sceneFile = parser.getString("-f", "casino_grand/scene.gltf");
  //std::string sceneFile = parser.getString("-f", "bathroom_interior/scene.gltf");
  std::string hdrFilename = parser.getString("-e", "std_env.hdr");

  // Setup GLFW window
  if(glfwInit() == GLFW_FALSE)
  {
    char const* errMsg = nullptr;
    glfwGetError(&errMsg);
    fprintf(stderr, "GLFW Error %s\n", errMsg);
    return 1;
  }
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  GLFWwindow* window = glfwCreateWindow(WINDOW_WIDTH, WINDOW_HEIGHT, PROJECT_NAME, nullptr, nullptr);

  // Setup camera
  CameraManip.setWindowSize(WINDOW_WIDTH, WINDOW_HEIGHT);
  //CameraManip.setLookat({2.0, 2.0, -5.0}, {-1.0, 2.0, -1.0}, {0.000, 1.000, 0.000});

  // Setup Vulkan
  if(glfwVulkanSupported() == GLFW_FALSE)
  {
    fprintf(stderr, "GLFW: Vulkan Not Supported\n");
    return 1;
  }

  // Search path for shaders and other media
  defaultSearchPaths = {
      NVPSystem::exePath() + PROJECT_NAME, 
      NVPSystem::exePath() + R"(media)",
      NVPSystem::exePath() + PROJECT_RELDIRECTORY,
      NVPSystem::exePath() + PROJECT_DOWNLOAD_RELDIRECTORY,
  };

  // Vulkan required extensions
  assert(glfwVulkanSupported() == GLFW_TRUE); 
  uint32_t count{0};
  auto     reqExtensions = glfwGetRequiredInstanceExtensions(&count);

  // Requesting Vulkan extensions and layers
  nvvk::ContextCreateInfo contextInfo(true);
  contextInfo.setVersion(1, 2); 
  for(uint32_t ext_id = 0; ext_id < count; ext_id++)  
    contextInfo.addInstanceExtension(reqExtensions[ext_id]);
  contextInfo.addInstanceExtension("VK_EXT_debug_utils", true);  
  contextInfo.addDeviceExtension("VK_KHR_swapchain");            

  VkPhysicalDeviceShaderClockFeaturesKHR clockFeature{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CLOCK_FEATURES_KHR};
  contextInfo.addDeviceExtension("VK_KHR_shader_clock", false, &clockFeature);

  // ray tracing extension
  VkPhysicalDeviceAccelerationStructureFeaturesKHR accelFeature{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
  contextInfo.addDeviceExtension("VK_KHR_acceleration_structure", false, &accelFeature);
  VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtPipelineFeature{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
  contextInfo.addDeviceExtension("VK_KHR_ray_tracing_pipeline", false, &rtPipelineFeature);
  VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR };
  contextInfo.addDeviceExtension("VK_KHR_ray_query", true/*Optional extension*/, &rayQueryFeatures);
  contextInfo.addDeviceExtension("VK_KHR_deferred_host_operations");
  contextInfo.addDeviceExtension("VK_KHR_buffer_device_address");

  // Extra queues for parallel load/build
  contextInfo.addRequestedQueue(contextInfo.defaultQueueGCT, 1, 1.0f);  // Loading scene - mipmap generation



  // Creating Vulkan base application
  nvvk::Context vkContext{};
  vkContext.initInstance(contextInfo);
  auto compatibleDevices = vkContext.getCompatibleDevices(contextInfo);  // Find all compatible devices
  assert(!compatibleDevices.empty());
  vkContext.initDevice(compatibleDevices[0], contextInfo);  // Use first compatible device


  Raytracer raytracer;

  // Window need to be opened to get the surface on which to draw
  const VkSurfaceKHR surface = raytracer.getVkSurface(vkContext.m_instance, window);
  vkContext.setGCTQueueWithPresent(surface);
  raytracer.setupGlfwCallbacks(window);

  
  auto                     qGCT1 = vkContext.createQueue(contextInfo.defaultQueueGCT, "GCT1", 1.0f);
  std::vector<nvvk::Queue> queues;
  queues.push_back({vkContext.m_queueGCT.queue, vkContext.m_queueGCT.familyIndex, vkContext.m_queueGCT.queueIndex});
  queues.push_back({qGCT1.queue, qGCT1.familyIndex, qGCT1.queueIndex});
  queues.push_back({vkContext.m_queueC.queue, vkContext.m_queueC.familyIndex, vkContext.m_queueC.queueIndex});
  queues.push_back({vkContext.m_queueT.queue, vkContext.m_queueT.familyIndex, vkContext.m_queueT.queueIndex});

  // Create app
  raytracer.setup(vkContext.m_instance, vkContext.m_device, vkContext.m_physicalDevice, queues);
  raytracer.createSwapchain(surface, WINDOW_WIDTH, WINDOW_HEIGHT);
  raytracer.createDepthBuffer();
  raytracer.createRenderPass();
  raytracer.createFrameBuffers();

  // Setup Imgui
  raytracer.initImGui();
  raytracer.createOffscreenRender();
  ImGui_ImplGlfw_InitForVulkan(window, true);

  // Creation of the example - loading scene in separate thread
  raytracer.loadEnvironmentHdr(nvh::findFile(hdrFilename, defaultSearchPaths, true));
  raytracer.m_busy = true;
  std::thread([&] {
    raytracer.m_busyReasonText = "Loading Scene";
    raytracer.loadScene(nvh::findFile(sceneFile, defaultSearchPaths, true));
    raytracer.createUniformBuffer();
    raytracer.createDescriptorSetLayout();
    raytracer.createRender(Raytracer::eRtxPipeline);
    raytracer.resetFrame();
    raytracer.m_busy = false;
  }).detach();


  // Main loop
  while(glfwWindowShouldClose(window) == GLFW_FALSE)
  {
    glfwPollEvents();
    if(raytracer.isMinimized())
      continue;

    // Start the Dear ImGui frame
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    // Start rendering the scene
    raytracer.prepareFrame();  // Waits for a framebuffer to be available
    raytracer.updateFrame();   // Increment/update rendering frame count

    // Start command buffer of this frame
    auto                   curFrame = raytracer.getCurFrame();
    const VkCommandBuffer& cmdBuf   = raytracer.getCommandBuffers()[curFrame];

    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmdBuf, &beginInfo);

    raytracer.renderGui();          
    raytracer.updateUniformBuffer(cmdBuf);  // Updating UBOs

    // Rendering Scene (ray tracing)
    raytracer.renderScene(cmdBuf);

    // Rendering pass in swapchain framebuffer + tone mapper, UI
    {

      std::array<VkClearValue, 2> clearValues;
      clearValues[0].color        = {{0.0f, 0.0f, 0.0f, 0.0f}};
      clearValues[1].depthStencil = {1.0f, 0};

      VkRenderPassBeginInfo postRenderPassBeginInfo{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
      postRenderPassBeginInfo.clearValueCount = 2;
      postRenderPassBeginInfo.pClearValues    = clearValues.data();
      postRenderPassBeginInfo.renderPass      = raytracer.getRenderPass();
      postRenderPassBeginInfo.framebuffer     = raytracer.getFramebuffers()[curFrame];
      postRenderPassBeginInfo.renderArea      = {{}, raytracer.getSize()};

      vkCmdBeginRenderPass(cmdBuf, &postRenderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

      // Draw the rendering result + tonemapper
      raytracer.drawPost(cmdBuf);

      // Render the UI
      ImGui::Render();
      ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmdBuf);

      vkCmdEndRenderPass(cmdBuf);
    }


    // Submit for display
    vkEndCommandBuffer(cmdBuf);
    raytracer.submitFrame();

    CameraManip.updateAnim();
  }

  // Cleanup
  vkDeviceWaitIdle(raytracer.getDevice());
  raytracer.destroyResources();
  raytracer.destroy();
  vkContext.deinit();

  glfwDestroyWindow(window);
  glfwTerminate();

  return 0;
}
