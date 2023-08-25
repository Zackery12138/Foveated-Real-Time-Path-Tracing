#pragma once
#include "nvh/gltfscene.hpp"
#include "nvvk/resourceallocator_vk.hpp"
#include "nvvk/descriptorsets_vk.hpp"
#include "nvvk/raytraceKHR_vk.hpp"


/*
 The AccelStructure class uploads a glTF scene to an acceleration structure.
 It initializes, creates by passing the glTF scene and the buffer of vertices and indices, and destroys.
 The Top Level Acceleration Structure (TLAS) and descriptor sets and layout can be retrieved.
*/
class AccelStructure
{
public:
  void setup(const VkDevice& device, const VkPhysicalDevice& physicalDevice, uint32_t familyIndex, nvvk::ResourceAllocator* allocator);
  void destroy();
  void create(nvh::GltfScene& gltfScene, const std::vector<nvvk::Buffer>& vertex, const std::vector<nvvk::Buffer>& index);

  VkAccelerationStructureKHR getTlas() { return m_rtBuilder.getAccelerationStructure(); }
  VkDescriptorSetLayout      getDescLayout() { return m_rtDescSetLayout; }
  VkDescriptorSet            getDescSet() { return m_rtDescSet; }

private:
  nvvk::RaytracingBuilderKHR::BlasInput primitiveToGeometry(const nvh::GltfPrimMesh& prim, VkBuffer vertex, VkBuffer index);
  void                                  createBottomLevelAS(nvh::GltfScene& gltfScene, const std::vector<nvvk::Buffer>& vertex, const std::vector<nvvk::Buffer>& index);
  void                                  createTopLevelAS(nvh::GltfScene& gltfScene);
  void                                  createRtDescriptorSet();


  // Setup
  nvvk::ResourceAllocator* m_pAlloc{nullptr};  // Allocator for buffer, images, acceleration structures
  nvvk::DebugUtil          m_debug;            // Utility to name objects
  VkDevice                 m_device{nullptr};
  uint32_t                 m_queueIndex{0};

  nvvk::RaytracingBuilderKHR m_rtBuilder;

  VkDescriptorPool      m_rtDescPool{VK_NULL_HANDLE};
  VkDescriptorSetLayout m_rtDescSetLayout{VK_NULL_HANDLE};
  VkDescriptorSet       m_rtDescSet{VK_NULL_HANDLE};
};
