/*
 * Copyright (c) 2014-2021, NVIDIA CORPORATION.  All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-FileCopyrightText: Copyright (c) 2014-2021 NVIDIA CORPORATION
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "shaders/host_device.h"

#include "nvvk/appbase_vk.hpp"
#include "nvvk/debug_util_vk.hpp"
#include "nvvk/descriptorsets_vk.hpp"
#include "nvvk/memallocator_dma_vk.hpp"
#include "nvvk/resourceallocator_vk.hpp"

// #VKRay
#include "nvh/gltfscene.hpp"
#include "nvvk/raytraceKHR_vk.hpp"
#include "nvvk/sbtwrapper_vk.hpp"


#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))


class HelloVulkan : public nvvk::AppBaseVk
{
public:
  void setup(const VkInstance& instance, const VkDevice& device, const VkPhysicalDevice& physicalDevice, uint32_t queueFamily) override;
  void setDefaults();
  void createDescriptorSetLayout();
  void createGraphicsPipeline();
  void createBeamBoundingBox();
  void loadScene(const std::string& filename);
  void updateDescriptorSet();
  void createUniformBuffer();
  void createTextureImages(const VkCommandBuffer& cmdBuf, tinygltf::Model& gltfModel);
  void updateUniformBuffer(const VkCommandBuffer& cmdBuf);
  void resetBeamBuffer(const VkCommandBuffer& cmdBuf);
  void onResize(int /*w*/, int /*h*/) override;
  void destroyResources();
  void rasterize(const VkCommandBuffer& cmdBuff);
  void setBeamPushConstants(const nvmath::vec4f& clearColor);

  nvh::GltfScene m_gltfScene;
  nvvk::Buffer   m_vertexBuffer;
  nvvk::Buffer   m_normalBuffer;
  nvvk::Buffer   m_uvBuffer;
  nvvk::Buffer   m_indexBuffer;
  nvvk::Buffer   m_materialBuffer;
  nvvk::Buffer   m_primInfo;
  nvvk::Buffer   m_sceneDesc;

  nvvk::Buffer m_beamBoxBuffer;
  nvvk::Buffer m_beamBuffer;
  nvvk::Buffer m_beamAsInfoBuffer;
  nvvk::Buffer m_beamAsCountReadBuffer;

  nvvk::Buffer m_beamTlasScratchBuffer;
  nvvk::AccelKHR m_pbTlas;

  float    m_airAlbedo{0.1f};
  float m_beamRadius{0.5f};
  float    m_photonRadius{0.5f};
  uint32_t m_numBeamSamples{1024};
  uint32_t m_numPhotonSamples{4 * 4 * 1024};

  //uint32_t m_numBeamSamples{64};
  //uint32_t m_numPhotonSamples{64};
  
  // max(number of photon samples, number of number of beam samples) * (expected number of scatter  + surface intersection )
  const uint32_t maxNumBeamSamples{2048};
  const uint32_t maxNumPhotonSamples{4 * 4 * 4096};

  const uint32_t m_maxNumBeams{MAX(maxNumBeamSamples, maxNumPhotonSamples) * 32};
  // number of beam samples * (expected number of scatter  + surface intersection ) * (expected length of the beam / (radius * 2)) 
  const uint32_t m_maxNumSubBeams{maxNumBeamSamples * 48 + maxNumPhotonSamples};


  nvmath::vec4f m_beamNearColor;
  nvmath::vec4f m_beamUnitDistantColor;
  float         m_beamIntensity;
  bool          m_usePhotonMapping;
  bool          m_usePhotonBeam;
  float         m_hgAssymFactor;
  bool          m_showDirectColor;

  bool m_isLightMotionOn;
  bool m_isLightVariationOn;
  float m_lightVariationInterval;
  float m_seedTime;
  float    m_totalTime;
  uint32_t m_randomSeed;

  // Information pushed at each draw call
  PushConstantRaster m_pcRaster{
      {1},               // Identity matrix
      {0.f, 0.0f, 0.f},  // light position
      0,                 // instance Id
      10.f,              // light intensity
      0,                 // light type
      0                  // material id
  };

  // Graphic pipeline
  VkPipelineLayout            m_pipelineLayout;
  VkPipeline                  m_graphicsPipeline;
  nvvk::DescriptorSetBindings m_descSetLayoutBind;
  VkDescriptorPool            m_descPool;
  VkDescriptorSetLayout       m_descSetLayout;
  VkDescriptorSet             m_descSet;

  nvvk::Buffer               m_bGlobals;  // Device-Host of the camera matrices
  std::vector<nvvk::Texture> m_textures;  // vector of all textures of the scene

  nvvk::ResourceAllocatorDma m_alloc;  // Allocator for buffer, images, acceleration structures
  nvvk::DebugUtil            m_debug;  // Utility to name objects


  // #Post - Draw the rendered image on a quad using a tonemapper
  void addTime(float timeDetla);
  void createOffscreenRender();
  void createPostPipeline();
  void createPostDescriptor();
  void updatePostDescriptorSet();
  void drawPost(VkCommandBuffer cmdBuf);

  nvvk::DescriptorSetBindings m_postDescSetLayoutBind;
  VkDescriptorPool            m_postDescPool{VK_NULL_HANDLE};
  VkDescriptorSetLayout       m_postDescSetLayout{VK_NULL_HANDLE};
  VkDescriptorSet             m_postDescSet{VK_NULL_HANDLE};
  VkPipeline                  m_postPipeline{VK_NULL_HANDLE};
  VkPipelineLayout            m_postPipelineLayout{VK_NULL_HANDLE};
  VkRenderPass                m_offscreenRenderPass{VK_NULL_HANDLE};
  VkFramebuffer               m_offscreenFramebuffer{VK_NULL_HANDLE};
  nvvk::Texture               m_offscreenColor;
  nvvk::Texture               m_offscreenDepth;
  VkFormat                    m_offscreenColorFormat{VK_FORMAT_R32G32B32A32_SFLOAT};
  VkFormat                    m_offscreenDepthFormat{VK_FORMAT_X8_D24_UNORM_PACK32};

  // #VKRay
  void initRayTracing();
  auto primitiveToVkGeometry(const nvh::GltfPrimMesh& prim);
  void createBottomLevelAS();
  void createTopLevelAS();
  void createBeamASResources();
  void createRtDescriptorSet();
  void updateRtDescriptorSet();
  void updateRtDescriptorSetBeamTlas();
  void createRtPipeline();

  void createPbDescriptorSet();
  void createPbPipeline();
  void buildPbTlas(const nvmath::vec4f& clearColor, const VkCommandBuffer& cmdBuf);

  void raytrace(const VkCommandBuffer& cmdBuf);
  void updateFrame();

  VkPhysicalDeviceRayTracingPipelinePropertiesKHR m_rtProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
  nvvk::RaytracingBuilderKHR                      m_rtBuilder;
  nvvk::DescriptorSetBindings                     m_rtDescSetLayoutBind;
  VkDescriptorPool                                m_rtDescPool;
  VkDescriptorSetLayout                           m_rtDescSetLayout;
  VkDescriptorSet                                 m_rtDescSet;
  std::vector<VkRayTracingShaderGroupCreateInfoKHR> m_rtShaderGroups;
  VkPipelineLayout                                  m_rtPipelineLayout;
  VkPipeline                                        m_rtPipeline;
  nvvk::SBTWrapper                                  m_sbtWrapper;

  PushConstantRay m_pcRay{};

  nvvk::RaytracingBuilderKHR                        m_pbBuilder; // only used for creating Blas
  nvvk::DescriptorSetBindings m_pbDescSetLayoutBind;
  VkDescriptorPool            m_pbDescPool;
  VkDescriptorSetLayout       m_pbDescSetLayout;
  VkDescriptorSet             m_pbDescSet;
  std::vector<VkRayTracingShaderGroupCreateInfoKHR> m_pbShaderGroups;
  VkPipelineLayout                                  m_pbPipelineLayout;
  VkPipeline                                        m_pbPipeline;
  nvvk::SBTWrapper                                  m_pbSbtWrapper;
};
