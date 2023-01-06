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


#include <sstream>


#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION


#include "hello_vulkan.h"
#include "nvh/cameramanipulator.hpp"
#include "nvh/fileoperations.hpp"
#include "nvh/gltfscene.hpp"
#include "nvh/nvprint.hpp"
#include "nvvk/commands_vk.hpp"
#include "nvvk/descriptorsets_vk.hpp"
#include "nvvk/images_vk.hpp"
#include "nvvk/pipeline_vk.hpp"
#include "nvvk/renderpasses_vk.hpp"
#include "nvvk/shaders_vk.hpp"

#include "nvh/alignment.hpp"
#include "nvvk/buffers_vk.hpp"

extern std::vector<std::string> defaultSearchPaths;



void HelloVulkan::setDefaults()
{
  const nvmath::vec4f defaultBeamNearColor{1.0f, 1.0f, 1.0f, 1.0f};
  const nvmath::vec4f defaultBeamUnitDistantColor{0.816, 0.906, 0.906, 1.0f};


  m_beamNearColor        = defaultBeamNearColor;
  m_beamUnitDistantColor = defaultBeamUnitDistantColor;
  m_beamRadius   = 0.6;
  m_photonRadius = 1.0;
  m_beamIntensity        = 15.0f;
  m_usePhotonMapping = true;
  m_usePhotonBeam    = true;
  m_hgAssymFactor    = 0.0;
  m_showDirectColor = false;
  m_airAlbedo            = 0.06;

  m_numBeamSamples = 1024;
  m_numPhotonSamples = 4 * 4 * 2048;

  m_lightMotion = true;
  m_lightVariation = true;
  m_lightVariationInterval = 30.0f;
}

void HelloVulkan::setup(const VkInstance& instance, const VkDevice& device, const VkPhysicalDevice& physicalDevice, uint32_t queueFamily)
{
  setDefaults();
  AppBaseVk::setup(instance, device, physicalDevice, queueFamily);
  m_alloc.init(instance, device, physicalDevice);
  m_debug.setup(m_device);
  m_offscreenDepthFormat = nvvk::findDepthFormat(physicalDevice);
}

//--------------------------------------------------------------------------------------------------
// Called at each frame to update the camera matrix
//
void HelloVulkan::updateUniformBuffer(const VkCommandBuffer& cmdBuf)
{
  // Prepare new UBO contents on host.
  const float    aspectRatio = m_size.width / static_cast<float>(m_size.height);
  GlobalUniforms hostUBO     = {};
  const auto&    view        = CameraManip.getMatrix();
  const auto&    proj        = nvmath::perspectiveVK(CameraManip.getFov(), aspectRatio, 0.1f, 1000.0f);
  // proj[1][1] *= -1;  // Inverting Y for Vulkan (not needed with perspectiveVK).

  hostUBO.viewProj    = proj * view;
  hostUBO.viewInverse = nvmath::invert(view);
  hostUBO.projInverse = nvmath::invert(proj);

  // UBO on the device, and what stages access it.
  VkBuffer deviceUBO      = m_bGlobals.buffer;
  auto     uboUsageStages = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;

  // Ensure that the modified UBO is not visible to previous frames.
  VkBufferMemoryBarrier beforeBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  beforeBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
  beforeBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  beforeBarrier.buffer        = deviceUBO;
  beforeBarrier.offset        = 0;
  beforeBarrier.size          = sizeof(hostUBO);
  vkCmdPipelineBarrier(cmdBuf, uboUsageStages, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_DEPENDENCY_DEVICE_GROUP_BIT, 0,
                       nullptr, 1, &beforeBarrier, 0, nullptr);


  // Schedule the host-to-device upload. (hostUBO is copied into the cmd
  // buffer so it is okay to deallocate when the function returns).
  vkCmdUpdateBuffer(cmdBuf, m_bGlobals.buffer, 0, sizeof(GlobalUniforms), &hostUBO);

  // Making sure the updated UBO will be visible.
  VkBufferMemoryBarrier afterBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  afterBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  afterBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  afterBarrier.buffer        = deviceUBO;
  afterBarrier.offset        = 0;
  afterBarrier.size          = sizeof(hostUBO);
  vkCmdPipelineBarrier(cmdBuf, VK_PIPELINE_STAGE_TRANSFER_BIT, uboUsageStages, VK_DEPENDENCY_DEVICE_GROUP_BIT, 0,
                       nullptr, 1, &afterBarrier, 0, nullptr);
}

// set the counter of the beams in the beam buffer to zero
void HelloVulkan::resetBeamBuffer(const VkCommandBuffer& cmdBuf)
{

  VkBuffer beamBuffer     = m_beamBuffer.buffer;
  auto     beamUsageStages = VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;

  VkBufferMemoryBarrier afterBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  afterBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  afterBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  afterBarrier.buffer        = beamBuffer;
  afterBarrier.offset        = 0;
  afterBarrier.size          = sizeof(uint) * 2; // for sub beamphoton counter and beam counter

  uint beamCounts[2] = {0, 0};

  vkCmdUpdateBuffer(cmdBuf, beamBuffer, 0, sizeof(uint), &beamCounts);
  vkCmdPipelineBarrier(
      cmdBuf, 
      VK_PIPELINE_STAGE_TRANSFER_BIT, 
      beamUsageStages, 
      VK_DEPENDENCY_DEVICE_GROUP_BIT, 
      0, nullptr, 1, &afterBarrier, 0, nullptr
  );
}

//--------------------------------------------------------------------------------------------------
// Describing the layout pushed when rendering
//
void HelloVulkan::createDescriptorSetLayout()
{
  auto& bind = m_descSetLayoutBind;
  // Camera matrices
  bind.addBinding(SceneBindings::eGlobals, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                  VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR);
  // Array of textures
  auto nbTextures = static_cast<uint32_t>(m_textures.size());
  bind.addBinding(SceneBindings::eTextures, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nbTextures,
                  VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR
                      | VK_SHADER_STAGE_ANY_HIT_BIT_KHR);
  // Scene buffers
  bind.addBinding(eSceneDesc, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                  VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR
                      | VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR);

  m_descSetLayout = m_descSetLayoutBind.createLayout(m_device);
  m_descPool      = m_descSetLayoutBind.createPool(m_device, 1);
  m_descSet       = nvvk::allocateDescriptorSet(m_device, m_descPool, m_descSetLayout);
}

//--------------------------------------------------------------------------------------------------
// Setting up the buffers in the descriptor set
//
void HelloVulkan::updateDescriptorSet()
{
  std::vector<VkWriteDescriptorSet> writes;

  // Camera matrices and scene description
  VkDescriptorBufferInfo dbiUnif{m_bGlobals.buffer, 0, VK_WHOLE_SIZE};
  VkDescriptorBufferInfo sceneDesc{m_sceneDesc.buffer, 0, VK_WHOLE_SIZE};

  writes.emplace_back(m_descSetLayoutBind.makeWrite(m_descSet, SceneBindings::eGlobals, &dbiUnif));
  writes.emplace_back(m_descSetLayoutBind.makeWrite(m_descSet, eSceneDesc, &sceneDesc));

  // All texture samplers
  std::vector<VkDescriptorImageInfo> diit;
  for(auto& texture : m_textures)
    diit.emplace_back(texture.descriptor);
  writes.emplace_back(m_descSetLayoutBind.makeWriteArray(m_descSet, SceneBindings::eTextures, diit.data()));

  // Writing the information
  vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}


//--------------------------------------------------------------------------------------------------
// Creating the pipeline layout
//
void HelloVulkan::createGraphicsPipeline()
{
  VkPushConstantRange pushConstantRanges = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstantRaster)};

  // Creating the Pipeline Layout
  VkPipelineLayoutCreateInfo createInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  createInfo.setLayoutCount         = 1;
  createInfo.pSetLayouts            = &m_descSetLayout;
  createInfo.pushConstantRangeCount = 1;
  createInfo.pPushConstantRanges    = &pushConstantRanges;
  vkCreatePipelineLayout(m_device, &createInfo, nullptr, &m_pipelineLayout);


  // Creating the Pipeline
  std::vector<std::string>                paths = defaultSearchPaths;
  nvvk::GraphicsPipelineGeneratorCombined gpb(m_device, m_pipelineLayout, m_offscreenRenderPass);
  gpb.depthStencilState.depthTestEnable = true;
  gpb.addShader(nvh::loadFile("spv/vert_shader.vert.spv", true, paths, true), VK_SHADER_STAGE_VERTEX_BIT);
  gpb.addShader(nvh::loadFile("spv/frag_shader.frag.spv", true, paths, true), VK_SHADER_STAGE_FRAGMENT_BIT);
  gpb.addBindingDescriptions({{0, sizeof(nvmath::vec3f)}, {1, sizeof(nvmath::vec3f)}, {2, sizeof(nvmath::vec2f)}});
  gpb.addAttributeDescriptions({
      {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},  // Position
      {1, 1, VK_FORMAT_R32G32B32_SFLOAT, 0},  // Normal
      {2, 2, VK_FORMAT_R32G32_SFLOAT, 0},     // Texcoord0
  });
  m_graphicsPipeline = gpb.createPipeline();
  m_debug.setObjectName(m_graphicsPipeline, "Graphics");
}

void HelloVulkan::createBeamBoundingBox()
{
  std::vector<Aabb> aabbs;
  aabbs.reserve(2);

  Aabb beamBox;
  beamBox.minimum = nvmath::vec3f(-1.0f, -1.0f, 0.0f);
  beamBox.maximum = nvmath::vec3f(1.0f, 1.0f, 2.0f);

  Aabb photonBox;
  // Not making photon aabb with 0 height, 
  // because a photon could hit non-flat surface, such as surface of sphere
  photonBox.minimum = nvmath::vec3f(-1.0f, -0.1f, -1.0);
  photonBox.maximum = nvmath::vec3f(1.0f, 0.1f, 1.0f);

  aabbs.emplace_back(beamBox);
  aabbs.emplace_back(photonBox);
 

  nvvk::CommandPool cmdBufGet(m_device, m_graphicsQueueIndex);
  VkCommandBuffer   cmdBuf = cmdBufGet.createCommandBuffer();

  m_beamBoxBuffer = m_alloc.createBuffer(
      cmdBuf, 
      aabbs,
      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR
  );

  cmdBufGet.submitAndWait(cmdBuf);
  m_alloc.finalizeAndReleaseStaging();

  NAME_VK(m_beamBoxBuffer.buffer);
}

//--------------------------------------------------------------------------------------------------
// Loading the OBJ file and setting up all buffers
//
void HelloVulkan::loadScene(const std::string& filename)
{
  using vkBU = VkBufferUsageFlagBits;
  tinygltf::Model    tmodel;
  tinygltf::TinyGLTF tcontext;
  std::string        warn, error;

  LOGI("Loading file: %s", filename.c_str());
  if(!tcontext.LoadASCIIFromFile(&tmodel, &error, &warn, filename))
  {
    assert(!"Error while loading scene");
  }
  LOGW(warn.c_str());
  LOGE(error.c_str());


  m_gltfScene.importMaterials(tmodel);
  m_gltfScene.importDrawableNodes(tmodel, nvh::GltfAttributes::Normal | nvh::GltfAttributes::Texcoord_0);

  // Create the buffers on Device and copy vertices, indices and materials
  nvvk::CommandPool cmdBufGet(m_device, m_graphicsQueueIndex);
  VkCommandBuffer   cmdBuf = cmdBufGet.createCommandBuffer();

  m_vertexBuffer = m_alloc.createBuffer(cmdBuf, m_gltfScene.m_positions,
                                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
                                            | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR);
  m_indexBuffer  = m_alloc.createBuffer(cmdBuf, m_gltfScene.m_indices,
                                       VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
                                           | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR);
  m_normalBuffer = m_alloc.createBuffer(cmdBuf, m_gltfScene.m_normals,
                                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                                            | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
  m_uvBuffer     = m_alloc.createBuffer(cmdBuf, m_gltfScene.m_texcoords0,
                                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                                        | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);

  // Copying all materials, only the elements we need
  std::vector<GltfShadeMaterial> shadeMaterials;
  for(const auto& m : m_gltfScene.m_materials)
  {
    shadeMaterials.emplace_back(
        GltfShadeMaterial{
            m.baseColorFactor, 
            m.emissiveFactor, 
            m.baseColorTexture, 
            m.metallicFactor, 
            m.roughnessFactor
        }
    );
  }
  m_materialBuffer = m_alloc.createBuffer(
      cmdBuf, 
      shadeMaterials,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
  );

  // The following is used to find the primitive mesh information in the CHIT
  std::vector<PrimMeshInfo> primLookup;
  for(auto& primMesh : m_gltfScene.m_primMeshes)
  {
    primLookup.push_back({primMesh.firstIndex, primMesh.vertexOffset, primMesh.materialIndex});
  }
  m_primInfo = m_alloc.createBuffer(cmdBuf, primLookup, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);

  m_beamBuffer = m_alloc.createBuffer(
      cmdBuf, 
      m_maxNumBeams * sizeof(PhotonBeam) + 4 * sizeof(uint), 
      nullptr, 
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
  );

  m_beamAsInfoBuffer = m_alloc.createBuffer(
      cmdBuf, 
      m_maxNumSubBeams * sizeof(ShaderVkAccelerationStructureInstanceKHR), 
      nullptr,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR
  );

  m_beamAsCountReadBuffer = m_alloc.createBuffer(
      cmdBuf, 
      1 * sizeof(uint32_t), 
      nullptr,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
  );

  SceneDesc sceneDesc;
  sceneDesc.vertexAddress   = nvvk::getBufferDeviceAddress(m_device, m_vertexBuffer.buffer);
  sceneDesc.indexAddress    = nvvk::getBufferDeviceAddress(m_device, m_indexBuffer.buffer);
  sceneDesc.normalAddress   = nvvk::getBufferDeviceAddress(m_device, m_normalBuffer.buffer);
  sceneDesc.uvAddress       = nvvk::getBufferDeviceAddress(m_device, m_uvBuffer.buffer);
  sceneDesc.materialAddress = nvvk::getBufferDeviceAddress(m_device, m_materialBuffer.buffer);
  sceneDesc.primInfoAddress = nvvk::getBufferDeviceAddress(m_device, m_primInfo.buffer);
  m_sceneDesc               = m_alloc.createBuffer(cmdBuf, sizeof(SceneDesc), &sceneDesc,
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);

  // Creates all textures found
  createTextureImages(cmdBuf, tmodel);
  cmdBufGet.submitAndWait(cmdBuf);
  m_alloc.finalizeAndReleaseStaging();


  NAME_VK(m_vertexBuffer.buffer);
  NAME_VK(m_indexBuffer.buffer);
  NAME_VK(m_normalBuffer.buffer);
  NAME_VK(m_uvBuffer.buffer);
  NAME_VK(m_materialBuffer.buffer);
  NAME_VK(m_primInfo.buffer);
  NAME_VK(m_sceneDesc.buffer);

  NAME_VK(m_beamBuffer.buffer);
  NAME_VK(m_beamAsInfoBuffer.buffer);
  NAME_VK(m_beamAsCountReadBuffer.buffer);
}


//--------------------------------------------------------------------------------------------------
// Creating the uniform buffer holding the camera matrices
// - Buffer is host visible
//
void HelloVulkan::createUniformBuffer()
{
  m_bGlobals = m_alloc.createBuffer(sizeof(GlobalUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  m_debug.setObjectName(m_bGlobals.buffer, "Globals");
}

//--------------------------------------------------------------------------------------------------
// Creating all textures and samplers
//
void HelloVulkan::createTextureImages(const VkCommandBuffer& cmdBuf, tinygltf::Model& gltfModel)
{
  VkSamplerCreateInfo samplerCreateInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  samplerCreateInfo.minFilter  = VK_FILTER_LINEAR;
  samplerCreateInfo.magFilter  = VK_FILTER_LINEAR;
  samplerCreateInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  samplerCreateInfo.maxLod     = FLT_MAX;

  VkFormat format = VK_FORMAT_R8G8B8A8_SRGB;

  auto addDefaultTexture = [this]() {
    // Make dummy image(1,1), needed as we cannot have an empty array
    nvvk::ScopeCommandBuffer cmdBuf(m_device, m_graphicsQueueIndex);
    std::array<uint8_t, 4>   white = {255, 255, 255, 255};

    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    m_textures.emplace_back(m_alloc.createTexture(cmdBuf, 4, white.data(), nvvk::makeImage2DCreateInfo(VkExtent2D{1, 1}), sampler));
    m_debug.setObjectName(m_textures.back().image, "dummy");
  };

  if(gltfModel.images.empty())
  {
    addDefaultTexture();
    return;
  }

  m_textures.reserve(gltfModel.images.size());
  for(size_t i = 0; i < gltfModel.images.size(); i++)
  {
    auto&        gltfimage  = gltfModel.images[i];
    void*        buffer     = &gltfimage.image[0];
    VkDeviceSize bufferSize = gltfimage.image.size();
    auto         imgSize    = VkExtent2D{(uint32_t)gltfimage.width, (uint32_t)gltfimage.height};

    if(bufferSize == 0 || gltfimage.width == -1 || gltfimage.height == -1)
    {
      addDefaultTexture();
      continue;
    }

    VkImageCreateInfo imageCreateInfo = nvvk::makeImage2DCreateInfo(imgSize, format, VK_IMAGE_USAGE_SAMPLED_BIT, true);

    nvvk::Image image = m_alloc.createImage(cmdBuf, bufferSize, buffer, imageCreateInfo);
    nvvk::cmdGenerateMipmaps(cmdBuf, image.image, format, imgSize, imageCreateInfo.mipLevels);
    VkImageViewCreateInfo ivInfo = nvvk::makeImageViewCreateInfo(image.image, imageCreateInfo);
    m_textures.emplace_back(m_alloc.createTexture(image, ivInfo, samplerCreateInfo));

    m_debug.setObjectName(m_textures[i].image, std::string("Txt" + std::to_string(i)));
  }
}

//--------------------------------------------------------------------------------------------------
// Destroying all allocations
//
void HelloVulkan::destroyResources()
{
  m_alloc.destroy(m_pbTlas);
  m_alloc.destroy(m_beamTlasScratchBuffer);
  m_alloc.destroy(m_beamAsInfoBuffer);
  m_alloc.destroy(m_beamAsCountReadBuffer);

  vkDestroyPipeline(m_device, m_graphicsPipeline, nullptr);
  vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
  vkDestroyDescriptorPool(m_device, m_descPool, nullptr);
  vkDestroyDescriptorSetLayout(m_device, m_descSetLayout, nullptr);

  m_alloc.destroy(m_bGlobals);

  m_alloc.destroy(m_vertexBuffer);
  m_alloc.destroy(m_normalBuffer);
  m_alloc.destroy(m_uvBuffer);
  m_alloc.destroy(m_indexBuffer);
  m_alloc.destroy(m_materialBuffer);
  m_alloc.destroy(m_primInfo);
  m_alloc.destroy(m_sceneDesc);

  m_alloc.destroy(m_beamBuffer);
  // m_alloc.destroy(m_beamAsInfoBuffer);
  m_alloc.destroy(m_beamBoxBuffer);

  for(auto& t : m_textures)
  {
    m_alloc.destroy(t);
  }

  //#Post
  m_alloc.destroy(m_offscreenColor);
  m_alloc.destroy(m_offscreenDepth);
  vkDestroyPipeline(m_device, m_postPipeline, nullptr);
  vkDestroyPipelineLayout(m_device, m_postPipelineLayout, nullptr);
  vkDestroyDescriptorPool(m_device, m_postDescPool, nullptr);
  vkDestroyDescriptorSetLayout(m_device, m_postDescSetLayout, nullptr);
  vkDestroyRenderPass(m_device, m_offscreenRenderPass, nullptr);
  vkDestroyFramebuffer(m_device, m_offscreenFramebuffer, nullptr);

  // #VKRay
  m_pbBuilder.destroy();
  m_pbSbtWrapper.destroy();

  m_rtBuilder.destroy();
  m_sbtWrapper.destroy();

  vkDestroyPipeline(m_device, m_pbPipeline, nullptr);
  vkDestroyPipelineLayout(m_device, m_pbPipelineLayout, nullptr);
  vkDestroyDescriptorPool(m_device, m_pbDescPool, nullptr);
  vkDestroyDescriptorSetLayout(m_device, m_pbDescSetLayout, nullptr);

  vkDestroyPipeline(m_device, m_rtPipeline, nullptr);
  vkDestroyPipelineLayout(m_device, m_rtPipelineLayout, nullptr);
  vkDestroyDescriptorPool(m_device, m_rtDescPool, nullptr);
  vkDestroyDescriptorSetLayout(m_device, m_rtDescSetLayout, nullptr);

  m_alloc.deinit();
}

//--------------------------------------------------------------------------------------------------
// Drawing the scene in raster mode
//
void HelloVulkan::rasterize(const VkCommandBuffer& cmdBuf)
{
  using vkPBP = VkPipelineBindPoint;

  std::vector<VkDeviceSize> offsets = {0, 0, 0};

  m_debug.beginLabel(cmdBuf, "Rasterize");

  // Dynamic Viewport
  setViewport(cmdBuf);

  // Drawing all triangles
  vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_graphicsPipeline);
  vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &m_descSet, 0, nullptr);

  std::vector<VkBuffer> vertexBuffers = {m_vertexBuffer.buffer, m_normalBuffer.buffer, m_uvBuffer.buffer};
  vkCmdBindVertexBuffers(cmdBuf, 0, static_cast<uint32_t>(vertexBuffers.size()), vertexBuffers.data(), offsets.data());
  vkCmdBindIndexBuffer(cmdBuf, m_indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);

  uint32_t idxNode = 0;
  for(auto& node : m_gltfScene.m_nodes)
  {
    auto& primitive = m_gltfScene.m_primMeshes[node.primMesh];

    m_pcRaster.modelMatrix = node.worldMatrix;
    m_pcRaster.objIndex    = node.primMesh;
    m_pcRaster.materialId  = primitive.materialIndex;
    vkCmdPushConstants(cmdBuf, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(PushConstantRaster), &m_pcRaster);
    vkCmdDrawIndexed(cmdBuf, primitive.indexCount, 1, primitive.firstIndex, primitive.vertexOffset, 0);
  }

  m_debug.endLabel(cmdBuf);
}

//--------------------------------------------------------------------------------------------------
// Handling resize of the window
//
void HelloVulkan::onResize(int /*w*/, int /*h*/)
{
  createOffscreenRender();
  updatePostDescriptorSet();
  updateRtDescriptorSet();
}


//////////////////////////////////////////////////////////////////////////
// Post-processing
//////////////////////////////////////////////////////////////////////////


//--------------------------------------------------------------------------------------------------
// Creating an offscreen frame buffer and the associated render pass
//
void HelloVulkan::createOffscreenRender()
{
  m_alloc.destroy(m_offscreenColor);
  m_alloc.destroy(m_offscreenDepth);

  // Creating the color image
  {
    auto colorCreateInfo = nvvk::makeImage2DCreateInfo(m_size, m_offscreenColorFormat,
                                                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                                                           | VK_IMAGE_USAGE_STORAGE_BIT);


    nvvk::Image           image  = m_alloc.createImage(colorCreateInfo);
    VkImageViewCreateInfo ivInfo = nvvk::makeImageViewCreateInfo(image.image, colorCreateInfo);
    VkSamplerCreateInfo   sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    m_offscreenColor                        = m_alloc.createTexture(image, ivInfo, sampler);
    m_offscreenColor.descriptor.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  }

  // Creating the depth buffer
  auto depthCreateInfo = nvvk::makeImage2DCreateInfo(m_size, m_offscreenDepthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
  {
    nvvk::Image image = m_alloc.createImage(depthCreateInfo);


    VkImageViewCreateInfo depthStencilView{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    depthStencilView.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    depthStencilView.format           = m_offscreenDepthFormat;
    depthStencilView.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    depthStencilView.image            = image.image;

    m_offscreenDepth = m_alloc.createTexture(image, depthStencilView);
  }

  // Setting the image layout for both color and depth
  {
    nvvk::CommandPool genCmdBuf(m_device, m_graphicsQueueIndex);
    auto              cmdBuf = genCmdBuf.createCommandBuffer();
    nvvk::cmdBarrierImageLayout(cmdBuf, m_offscreenColor.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    nvvk::cmdBarrierImageLayout(cmdBuf, m_offscreenDepth.image, VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);

    genCmdBuf.submitAndWait(cmdBuf);
  }

  // Creating a renderpass for the offscreen
  if(!m_offscreenRenderPass)
  {
    m_offscreenRenderPass = nvvk::createRenderPass(m_device, {m_offscreenColorFormat}, m_offscreenDepthFormat, 1, true,
                                                   true, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
  }


  // Creating the frame buffer for offscreen
  std::vector<VkImageView> attachments = {m_offscreenColor.descriptor.imageView, m_offscreenDepth.descriptor.imageView};

  vkDestroyFramebuffer(m_device, m_offscreenFramebuffer, nullptr);
  VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  info.renderPass      = m_offscreenRenderPass;
  info.attachmentCount = 2;
  info.pAttachments    = attachments.data();
  info.width           = m_size.width;
  info.height          = m_size.height;
  info.layers          = 1;
  vkCreateFramebuffer(m_device, &info, nullptr, &m_offscreenFramebuffer);
}

//--------------------------------------------------------------------------------------------------
// The pipeline is how things are rendered, which shaders, type of primitives, depth test and more
//
void HelloVulkan::createPostPipeline()
{
  // Push constants in the fragment shader
  VkPushConstantRange pushConstantRanges = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float)};

  // Creating the pipeline layout
  VkPipelineLayoutCreateInfo createInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  createInfo.setLayoutCount         = 1;
  createInfo.pSetLayouts            = &m_postDescSetLayout;
  createInfo.pushConstantRangeCount = 1;
  createInfo.pPushConstantRanges    = &pushConstantRanges;
  vkCreatePipelineLayout(m_device, &createInfo, nullptr, &m_postPipelineLayout);


  // Pipeline: completely generic, no vertices
  nvvk::GraphicsPipelineGeneratorCombined pipelineGenerator(m_device, m_postPipelineLayout, m_renderPass);
  pipelineGenerator.addShader(nvh::loadFile("spv/passthrough.vert.spv", true, defaultSearchPaths, true), VK_SHADER_STAGE_VERTEX_BIT);
  pipelineGenerator.addShader(nvh::loadFile("spv/post.frag.spv", true, defaultSearchPaths, true), VK_SHADER_STAGE_FRAGMENT_BIT);
  pipelineGenerator.rasterizationState.cullMode = VK_CULL_MODE_NONE;
  m_postPipeline                                = pipelineGenerator.createPipeline();
  m_debug.setObjectName(m_postPipeline, "post");
}

//--------------------------------------------------------------------------------------------------
// The descriptor layout is the description of the data that is passed to the vertex or the
// fragment program.
//
void HelloVulkan::createPostDescriptor()
{
  m_postDescSetLayoutBind.addBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
  m_postDescSetLayout = m_postDescSetLayoutBind.createLayout(m_device);
  m_postDescPool      = m_postDescSetLayoutBind.createPool(m_device);
  m_postDescSet       = nvvk::allocateDescriptorSet(m_device, m_postDescPool, m_postDescSetLayout);
}


//--------------------------------------------------------------------------------------------------
// Update the output
//
void HelloVulkan::updatePostDescriptorSet()
{
  VkWriteDescriptorSet writeDescriptorSets = m_postDescSetLayoutBind.makeWrite(m_postDescSet, 0, &m_offscreenColor.descriptor);
  vkUpdateDescriptorSets(m_device, 1, &writeDescriptorSets, 0, nullptr);
}

//--------------------------------------------------------------------------------------------------
// Draw a full screen quad with the attached image
//
void HelloVulkan::drawPost(VkCommandBuffer cmdBuf)
{
  m_debug.beginLabel(cmdBuf, "Post");

  setViewport(cmdBuf);

  auto aspectRatio = static_cast<float>(m_size.width) / static_cast<float>(m_size.height);
  vkCmdPushConstants(cmdBuf, m_postPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float), &aspectRatio);
  vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_postPipeline);
  vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_postPipelineLayout, 0, 1, &m_postDescSet, 0, nullptr);
  vkCmdDraw(cmdBuf, 3, 1, 0, 0);

  m_debug.endLabel(cmdBuf);
}


//--------------------------------------------------------------------------------------------------
// Initialize Vulkan ray tracing
// #VKRay
void HelloVulkan::initRayTracing()
{
  // Requesting ray tracing properties
  VkPhysicalDeviceProperties2 prop2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
  prop2.pNext = &m_rtProperties;
  vkGetPhysicalDeviceProperties2(m_physicalDevice, &prop2);

  m_rtBuilder.setup(m_device, &m_alloc, m_graphicsQueueIndex);
  m_pbBuilder.setup(m_device, &m_alloc, m_graphicsQueueIndex);
  m_sbtWrapper.setup(m_device, m_graphicsQueueIndex, &m_alloc, m_rtProperties);
  m_pbSbtWrapper.setup(m_device, m_graphicsQueueIndex, &m_alloc, m_rtProperties);
}

void HelloVulkan::createBeamASResources()
{
  VkDeviceAddress beamBoxDataAddress = nvvk::getBufferDeviceAddress(m_device, m_beamBoxBuffer.buffer);

  VkAccelerationStructureGeometryAabbsDataKHR aabbs{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR};
  aabbs.data.deviceAddress = beamBoxDataAddress;
  aabbs.stride             = sizeof(Aabb);

  VkAccelerationStructureGeometryKHR asGeom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
  asGeom.geometryType   = VK_GEOMETRY_TYPE_AABBS_KHR;
  asGeom.flags          = VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR;
  asGeom.geometry.aabbs = aabbs;

  VkAccelerationStructureBuildRangeInfoKHR offset{};
  offset.firstVertex     = 0;
  offset.primitiveCount  = 1;
  offset.primitiveOffset = 0;
  offset.transformOffset = 0;

  nvvk::RaytracingBuilderKHR::BlasInput beamBoxInput;
  beamBoxInput.asGeometry.emplace_back(asGeom);
  beamBoxInput.asBuildOffsetInfo.emplace_back(offset);


  VkAccelerationStructureBuildRangeInfoKHR photonBoxOffset{};
  photonBoxOffset.firstVertex = 0;
  photonBoxOffset.primitiveCount = 1;
  photonBoxOffset.primitiveOffset = sizeof(Aabb);
  photonBoxOffset.transformOffset = 0;

  nvvk::RaytracingBuilderKHR::BlasInput photonBoxInput;
  photonBoxInput.asGeometry.emplace_back(asGeom);
  photonBoxInput.asBuildOffsetInfo.emplace_back(photonBoxOffset);


  // Add Blas for the beam box
  std::vector<nvvk::RaytracingBuilderKHR::BlasInput> allBlas;
  allBlas.reserve(2);
  allBlas.push_back({beamBoxInput});
  allBlas.push_back({photonBoxInput});

  m_pbBuilder.buildBlas(allBlas, VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR);
  m_pcRay.beamBlasAddress = m_pbBuilder.getBlasDeviceAddress(0);
  m_pcRay.photonBlasAddress = m_pbBuilder.getBlasDeviceAddress(1);


  VkBuildAccelerationStructureFlagsKHR flags  = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
  bool                                 update = m_pbTlas.accel != nullptr;
  bool                                 motion = false;

  VkBufferDeviceAddressInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, nullptr, m_beamAsInfoBuffer.buffer};
  VkDeviceAddress instBufferAddr = vkGetBufferDeviceAddress(m_device, &bufferInfo);

  // Wraps a device pointer to the above uploaded instances.
  VkAccelerationStructureGeometryInstancesDataKHR instancesVk{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
  instancesVk.data.deviceAddress = instBufferAddr;

  // Put the above into a VkAccelerationStructureGeometryKHR. We need to put the instances struct in a union and label it as instance data.
  VkAccelerationStructureGeometryKHR topASGeometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
  topASGeometry.geometryType       = VK_GEOMETRY_TYPE_INSTANCES_KHR;
  topASGeometry.geometry.instances = instancesVk;

  // Find sizes
  VkAccelerationStructureBuildGeometryInfoKHR buildInfo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
  buildInfo.flags                    = flags;
  buildInfo.geometryCount            = 1;
  buildInfo.pGeometries              = &topASGeometry;
  buildInfo.mode                     = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  buildInfo.type                     = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
  buildInfo.srcAccelerationStructure = VK_NULL_HANDLE;


  VkAccelerationStructureBuildSizesInfoKHR sizeInfo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
  vkGetAccelerationStructureBuildSizesKHR(m_device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo,
                                          &m_maxNumSubBeams, &sizeInfo);

    VkAccelerationStructureCreateInfoKHR createInfo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    createInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    createInfo.size = sizeInfo.accelerationStructureSize;

    m_pbTlas = m_alloc.createAcceleration(createInfo);
  
    m_beamTlasScratchBuffer = m_alloc.createBuffer(sizeInfo.buildScratchSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                                                                                  | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
  
}

//--------------------------------------------------------------------------------------------------
// Converting a GLTF primitive in the Raytracing Geometry used for the BLAS
//
auto HelloVulkan::primitiveToVkGeometry(const nvh::GltfPrimMesh& prim)
{
  // BLAS builder requires raw device addresses.
  VkDeviceAddress vertexAddress = nvvk::getBufferDeviceAddress(m_device, m_vertexBuffer.buffer);
  VkDeviceAddress indexAddress  = nvvk::getBufferDeviceAddress(m_device, m_indexBuffer.buffer);

  uint32_t maxPrimitiveCount = prim.indexCount / 3;

  // Describe buffer as array of VertexObj.
  VkAccelerationStructureGeometryTrianglesDataKHR triangles{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
  triangles.vertexFormat             = VK_FORMAT_R32G32B32_SFLOAT;  // vec3 vertex position data.
  triangles.vertexData.deviceAddress = vertexAddress;
  triangles.vertexStride             = sizeof(nvmath::vec3f);
  // Describe index data (32-bit unsigned int)
  triangles.indexType               = VK_INDEX_TYPE_UINT32;
  triangles.indexData.deviceAddress = indexAddress;
  // Indicate identity transform by setting transformData to null device pointer.
  //triangles.transformData = {};
  triangles.maxVertex = prim.vertexCount;

  // Identify the above data as containing opaque triangles.
  VkAccelerationStructureGeometryKHR asGeom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
  asGeom.geometryType       = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
  asGeom.flags              = VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR;  // For AnyHit
  asGeom.geometry.triangles = triangles;

  VkAccelerationStructureBuildRangeInfoKHR offset;
  offset.firstVertex     = prim.vertexOffset;
  offset.primitiveCount  = maxPrimitiveCount;
  offset.primitiveOffset = prim.firstIndex * sizeof(uint32_t);
  offset.transformOffset = 0;

  // Our blas is made from only one geometry, but could be made of many geometries
  nvvk::RaytracingBuilderKHR::BlasInput input;
  input.asGeometry.emplace_back(asGeom);
  input.asBuildOffsetInfo.emplace_back(offset);

  return input;
}

//--------------------------------------------------------------------------------------------------
//
//
void HelloVulkan::createBottomLevelAS()
{
  // BLAS - Storing each primitive in a geometry
  std::vector<nvvk::RaytracingBuilderKHR::BlasInput> allBlas;
  allBlas.reserve(m_gltfScene.m_primMeshes.size());
  for(auto& primMesh : m_gltfScene.m_primMeshes)
  {
    auto geo = primitiveToVkGeometry(primMesh);
    allBlas.push_back({geo});
  }

  m_rtBuilder.buildBlas(allBlas, VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR);
}

//--------------------------------------------------------------------------------------------------
//
//
void HelloVulkan::createTopLevelAS()
{
  std::vector<VkAccelerationStructureInstanceKHR> tlas;
  tlas.reserve(m_gltfScene.m_nodes.size());
  for(auto& node : m_gltfScene.m_nodes)
  {
    VkAccelerationStructureInstanceKHR rayInst{};
    rayInst.transform                      = nvvk::toTransformMatrixKHR(node.worldMatrix);
    rayInst.instanceCustomIndex            = node.primMesh;  // gl_InstanceCustomIndexEXT: to find which primitive
    rayInst.accelerationStructureReference = m_rtBuilder.getBlasDeviceAddress(node.primMesh);
    rayInst.flags                          = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    rayInst.mask                           = 0xFF;
    rayInst.instanceShaderBindingTableRecordOffset = 0;  // We will use the same hit group for all objects
    tlas.emplace_back(rayInst);
  }
  m_rtBuilder.buildTlas(tlas, VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR);
}


void HelloVulkan::createPbDescriptorSet()
{
  // Top-level acceleration structure, usable by both the ray generation and the closest hit (to shoot shadow rays)
  m_pbDescSetLayoutBind.addBinding(PbBindings::ePbTlas, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1,
                                   VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR);  // TLAS
  m_pbDescSetLayoutBind.addBinding(PbBindings::ePbPrimLookup, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                   VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR);  // Primitive info

  m_pbDescSetLayoutBind.addBinding(PbBindings::ePbPhotonBeam, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                VK_SHADER_STAGE_RAYGEN_BIT_KHR);  // photon beam data
  m_pbDescSetLayoutBind.addBinding(PbBindings::ePbPhotonBeamAs, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                   VK_SHADER_STAGE_RAYGEN_BIT_KHR);  // photon beam data

  m_pbDescPool      = m_pbDescSetLayoutBind.createPool(m_device);
  m_pbDescSetLayout = m_pbDescSetLayoutBind.createLayout(m_device);

  VkDescriptorSetAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  allocateInfo.descriptorPool     = m_pbDescPool;
  allocateInfo.descriptorSetCount = 1;
  allocateInfo.pSetLayouts        = &m_pbDescSetLayout;
  vkAllocateDescriptorSets(m_device, &allocateInfo, &m_pbDescSet);


  VkAccelerationStructureKHR                   tlas = m_rtBuilder.getAccelerationStructure();
  VkWriteDescriptorSetAccelerationStructureKHR descASInfo{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
  descASInfo.accelerationStructureCount = 1;
  descASInfo.pAccelerationStructures    = &tlas;
  VkDescriptorBufferInfo primitiveInfoDesc{m_primInfo.buffer, 0, VK_WHOLE_SIZE};
  VkDescriptorBufferInfo beamInfo{m_beamBuffer.buffer, 0, VK_WHOLE_SIZE};
  VkDescriptorBufferInfo beamAsInfo{m_beamAsInfoBuffer.buffer, 0, VK_WHOLE_SIZE};

  std::vector<VkWriteDescriptorSet> writes;
  writes.emplace_back(m_pbDescSetLayoutBind.makeWrite(m_pbDescSet, PbBindings::ePbTlas, &descASInfo));
  writes.emplace_back(m_pbDescSetLayoutBind.makeWrite(m_pbDescSet, PbBindings::ePbPrimLookup, &primitiveInfoDesc));
  writes.emplace_back(m_pbDescSetLayoutBind.makeWrite(m_pbDescSet, PbBindings::ePbPhotonBeam, &beamInfo));
  writes.emplace_back(m_pbDescSetLayoutBind.makeWrite(m_pbDescSet, PbBindings::ePbPhotonBeamAs, &beamAsInfo));
  vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}


void HelloVulkan::createPbPipeline()
{
  enum StageIndices
  {
    eRaygen,
    eMiss,
    eClosestHit,
    eShaderGroupCount
  };

  // All stages
  std::array<VkPipelineShaderStageCreateInfo, eShaderGroupCount> stages{};
  VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  stage.pName = "main";  // All the same entry point
  // Raygen
  stage.module = nvvk::createShaderModule(m_device, nvh::loadFile("spv/photonbeam.rgen.spv", true, defaultSearchPaths, true));
  stage.stage     = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
  stages[eRaygen] = stage;
  // Miss
  stage.module = nvvk::createShaderModule(m_device, nvh::loadFile("spv/photonbeam.rmiss.spv", true, defaultSearchPaths, true));
  stage.stage   = VK_SHADER_STAGE_MISS_BIT_KHR;
  stages[eMiss] = stage;
  // Hit Group - Closest Hit
  stage.module = nvvk::createShaderModule(m_device, nvh::loadFile("spv/photonbeam.rchit.spv", true, defaultSearchPaths, true));
  stage.stage         = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
  stages[eClosestHit] = stage;


  // Shader groups
  VkRayTracingShaderGroupCreateInfoKHR group{VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR};
  group.anyHitShader       = VK_SHADER_UNUSED_KHR;
  group.closestHitShader   = VK_SHADER_UNUSED_KHR;
  group.generalShader      = VK_SHADER_UNUSED_KHR;
  group.intersectionShader = VK_SHADER_UNUSED_KHR;

  // Raygen
  group.type          = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
  group.generalShader = eRaygen;
  m_pbShaderGroups.push_back(group);

  // Miss
  group.type          = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
  group.generalShader = eMiss;
  m_pbShaderGroups.push_back(group);

  // closest hit shader
  group.type             = VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR;
  group.generalShader    = VK_SHADER_UNUSED_KHR;
  group.closestHitShader = eClosestHit;
  m_pbShaderGroups.push_back(group);


  // Push constant: we want to be able to update constants used by the shaders
  VkPushConstantRange pushConstant{VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR,
                                   0, sizeof(PushConstantRay)};


  VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pipelineLayoutCreateInfo.pushConstantRangeCount = 1;
  pipelineLayoutCreateInfo.pPushConstantRanges    = &pushConstant;

  // Descriptor sets: one specific to ray tracing, and one shared with the rasterization pipeline
  std::vector<VkDescriptorSetLayout> pbDescSetLayouts = {m_pbDescSetLayout, m_descSetLayout};
  pipelineLayoutCreateInfo.setLayoutCount             = static_cast<uint32_t>(pbDescSetLayouts.size());
  pipelineLayoutCreateInfo.pSetLayouts                = pbDescSetLayouts.data();

  vkCreatePipelineLayout(m_device, &pipelineLayoutCreateInfo, nullptr, &m_pbPipelineLayout);


  // Assemble the shader stages and recursion depth info into the ray tracing pipeline
  VkRayTracingPipelineCreateInfoKHR rayPipelineInfo{VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
  rayPipelineInfo.stageCount = static_cast<uint32_t>(stages.size());  // Stages are shaders
  rayPipelineInfo.pStages    = stages.data();

  // In this case, m_rtShaderGroups.size() == 4: we have one raygen group,
  // two miss shader groups, and one hit group.
  rayPipelineInfo.groupCount = static_cast<uint32_t>(m_pbShaderGroups.size());
  rayPipelineInfo.pGroups    = m_pbShaderGroups.data();

  // The ray tracing process can shoot rays from the camera, and a shadow ray can be shot from the
  // hit points of the camera rays, hence a recursion level of 2. This number should be kept as low
  // as possible for performance reasons. Even recursive ray tracing should be flattened into a loop
  // in the ray generation to avoid deep recursion.
  rayPipelineInfo.maxPipelineRayRecursionDepth = 2;  // Ray depth
  rayPipelineInfo.layout                       = m_pbPipelineLayout;

  vkCreateRayTracingPipelinesKHR(m_device, {}, {}, 1, &rayPipelineInfo, nullptr, &m_pbPipeline);


  // Creating the SBT
  m_pbSbtWrapper.create(m_pbPipeline, rayPipelineInfo);


  for(auto& s : stages)
    vkDestroyShaderModule(m_device, s.module, nullptr);
}

void HelloVulkan::setBeamPushConstants(const nvmath::vec4f& clearColor)
{
  // Initializing push constant values
  m_pcRay.clearColor       = clearColor;
  m_pcRay.lightPosition    = m_pcRaster.lightPosition;
  m_pcRay.lightType        = m_pcRaster.lightType;
  m_pcRay.beamRadius       = m_beamRadius;
  m_pcRay.photonRadius     = m_photonRadius;
  m_pcRay.maxNumBeams      = m_maxNumBeams;
  m_pcRay.maxNumSubBeams   = m_maxNumSubBeams;
  m_pcRay.airHGAssymFactor = m_hgAssymFactor;
  m_pcRay.numBeamSources   = m_numBeamSamples;
  m_pcRay.numPhotonSources = m_numPhotonSamples;
  m_pcRay.showDirectColor  = m_showDirectColor ? 1 : 0;

  // Bellow sets scatter and extinct cofficients and source light power, 
  // given the distance from the light source, 
  // the color near the light source
  // the color a unit distance away from the light soruce,
  // and the value of scatter/extinct cofficent
  // the method is based on the following article 
  // A Programmable System for Artistic Volumetric Lighting(2011) Derek Nowrouzezahrai
  const float minimumUnitDistantAlbedo = 0.1f;
  vec3 beamNearColor         = vec3(m_beamNearColor) * m_beamNearColor.w;
  vec3 beamUnitDistantColor = vec3(m_beamUnitDistantColor) * m_beamUnitDistantColor.w;

  vec3 unitDistantMinColor = beamNearColor * minimumUnitDistantAlbedo;

  beamUnitDistantColor.x = MIN(beamNearColor.x, MAX(beamUnitDistantColor.x, unitDistantMinColor.x));
  beamUnitDistantColor.y = MIN(beamNearColor.y, MAX(beamUnitDistantColor.y, unitDistantMinColor.y));
  beamUnitDistantColor.z = MIN(beamNearColor.z, MAX(beamUnitDistantColor.z, unitDistantMinColor.z));

  m_beamUnitDistantColor = vec4(beamUnitDistantColor,1.0);

  vec3 unitDistantAlbedoInverse(0.0);

  // do not allow division by zero
  unitDistantAlbedoInverse.x = beamNearColor.x == 0.0f ? 1.0f : beamNearColor.x / beamUnitDistantColor.x;
  unitDistantAlbedoInverse.y = beamNearColor.y == 0.0f ? 1.0f : beamNearColor.y / beamUnitDistantColor.y;
  unitDistantAlbedoInverse.z = beamNearColor.z == 0.0f ? 1.0f : beamNearColor.z / beamUnitDistantColor.z;

  //const auto& view = CameraManip.getMatrix();
  //vec3 eyePos = view * vec3(0, 0, 1);
  float beamSourceDist = 15.0f;  //use fixed distance between eye and camera

  vec3 extinctCoff = vec3(std::log(unitDistantAlbedoInverse.x), std::log(unitDistantAlbedoInverse.y), std::log(unitDistantAlbedoInverse.z));
  vec3 scatterCoff    = m_airAlbedo * extinctCoff;
  m_pcRay.sourceLight = beamNearColor  * nvmath::pow(unitDistantAlbedoInverse, beamSourceDist);

  m_pcRay.sourceLight.x = (extinctCoff.x <= 0.00001) ? beamNearColor.x : m_pcRay.sourceLight.x / scatterCoff.x;
  m_pcRay.sourceLight.y = (extinctCoff.y <= 0.00001) ? beamNearColor.y : m_pcRay.sourceLight.y / scatterCoff.y;
  m_pcRay.sourceLight.z = (extinctCoff.z <= 0.00001) ? beamNearColor.z : m_pcRay.sourceLight.z / scatterCoff.z;

  m_pcRay.sourceLight *= m_beamIntensity;

  m_pcRay.airExtinctCoff = extinctCoff;
  m_pcRay.airScatterCoff = scatterCoff;
}


//--------------------------------------------------------------------------------------------------
// This descriptor set holds the Acceleration structure and the output image
//
void HelloVulkan::createRtDescriptorSet()
{
  // Top-level acceleration structure, usable by both the ray generation and the closest hit (to shoot shadow rays)
  m_rtDescSetLayoutBind.addBinding(RtxBindings::eBeamAS, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1,
                                   VK_SHADER_STAGE_RAYGEN_BIT_KHR);  // TLAS
  m_rtDescSetLayoutBind.addBinding(RtxBindings::eOutImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                                   VK_SHADER_STAGE_RAYGEN_BIT_KHR);  // Output image
  m_rtDescSetLayoutBind.addBinding(RtxBindings::eBeamLookup, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                   VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR | VK_SHADER_STAGE_INTERSECTION_BIT_KHR);  // Beam info

  m_rtDescSetLayoutBind.addBinding(RtxBindings::eSurfaceAS, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR); 

  m_rtDescSetLayoutBind.addBinding(RtxBindings::ePrimLookup, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                   VK_SHADER_STAGE_RAYGEN_BIT_KHR);  // Primitive info

  m_rtDescPool      = m_rtDescSetLayoutBind.createPool(m_device);
  m_rtDescSetLayout = m_rtDescSetLayoutBind.createLayout(m_device);

  VkDescriptorSetAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  allocateInfo.descriptorPool     = m_rtDescPool;
  allocateInfo.descriptorSetCount = 1;
  allocateInfo.pSetLayouts        = &m_rtDescSetLayout;
  vkAllocateDescriptorSets(m_device, &allocateInfo, &m_rtDescSet);

  VkAccelerationStructureKHR                   beamAS = m_pbTlas.accel;
  VkWriteDescriptorSetAccelerationStructureKHR descBeamASInfo{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
  descBeamASInfo.accelerationStructureCount = 1;
  descBeamASInfo.pAccelerationStructures    = &beamAS;
  
  VkAccelerationStructureKHR                   surfaceAS = m_rtBuilder.getAccelerationStructure();
  VkWriteDescriptorSetAccelerationStructureKHR descSurfaceASInfo{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
  descSurfaceASInfo.accelerationStructureCount = 1;
  descSurfaceASInfo.pAccelerationStructures    = &surfaceAS;

  VkDescriptorImageInfo  imageInfo{{}, m_offscreenColor.descriptor.imageView, VK_IMAGE_LAYOUT_GENERAL};
  VkDescriptorBufferInfo beamInfoDesc{m_beamBuffer.buffer, 0, VK_WHOLE_SIZE};
  VkDescriptorBufferInfo primitiveInfoDesc{m_primInfo.buffer, 0, VK_WHOLE_SIZE};

  std::vector<VkWriteDescriptorSet> writes;
  writes.emplace_back(m_rtDescSetLayoutBind.makeWrite(m_rtDescSet, RtxBindings::eBeamAS, &descBeamASInfo));
  writes.emplace_back(m_rtDescSetLayoutBind.makeWrite(m_rtDescSet, RtxBindings::eOutImage, &imageInfo));
  writes.emplace_back(m_rtDescSetLayoutBind.makeWrite(m_rtDescSet, RtxBindings::eBeamLookup, &beamInfoDesc));
  writes.emplace_back(m_rtDescSetLayoutBind.makeWrite(m_rtDescSet, RtxBindings::eSurfaceAS, &descSurfaceASInfo));
  writes.emplace_back(m_rtDescSetLayoutBind.makeWrite(m_rtDescSet, RtxBindings::ePrimLookup, &primitiveInfoDesc));
  vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}


//--------------------------------------------------------------------------------------------------
// Writes the output image to the descriptor set
// - Required when changing resolution
//
void HelloVulkan::updateRtDescriptorSet()
{
  // (1) Output buffer
  VkDescriptorImageInfo imageInfo{{}, m_offscreenColor.descriptor.imageView, VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet  wds = m_rtDescSetLayoutBind.makeWrite(m_rtDescSet, RtxBindings::eOutImage, &imageInfo);
  vkUpdateDescriptorSets(m_device, 1, &wds, 0, nullptr);
}

void HelloVulkan::updateRtDescriptorSetBeamTlas()
{
  VkAccelerationStructureKHR                   beamAS = m_pbTlas.accel;
  VkWriteDescriptorSetAccelerationStructureKHR descBeamASInfo{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
  descBeamASInfo.accelerationStructureCount = 1;
  descBeamASInfo.pAccelerationStructures    = &beamAS;
  VkWriteDescriptorSet wds = m_rtDescSetLayoutBind.makeWrite(m_rtDescSet, RtxBindings::eBeamAS, &descBeamASInfo);
  vkUpdateDescriptorSets(m_device, 1, &wds, 0, nullptr);
}


void HelloVulkan::buildPbTlas(const nvmath::vec4f& clearColor, const VkCommandBuffer& cmdBuf)
{
    setBeamPushConstants(clearColor);


    m_debug.beginLabel(cmdBuf, "Beam trace");

    std::vector<VkDescriptorSet> descSets{m_pbDescSet, m_descSet};

    vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, m_pbPipeline);
    vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, m_pbPipelineLayout, 0,
                            (uint32_t)descSets.size(), descSets.data(), 0, nullptr);

    m_pcRay.numBeamSources   = m_usePhotonBeam ? m_numBeamSamples : 0;
    m_pcRay.numPhotonSources = m_usePhotonMapping ? m_numPhotonSamples : 0;
    vkCmdPushConstants(cmdBuf, m_pbPipelineLayout,
                       VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR,
                       0, sizeof(PushConstantRay), &m_pcRay);


    vkCmdFillBuffer(cmdBuf, m_beamBuffer.buffer, 0, sizeof(uint) * 2, 0);
    vkCmdFillBuffer(
        cmdBuf, 
        m_beamAsInfoBuffer.buffer, 
        0,
        m_maxNumSubBeams * sizeof(ShaderVkAccelerationStructureInstanceKHR), 
        0
    );

    // barrier for making ray traycing to proceed after the counters are reset to 0

    VkBufferMemoryBarrier beamDataBarriers[2] = {
      {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER},
      {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER}
    };
    
    beamDataBarriers[0].srcAccessMask           = VK_ACCESS_TRANSFER_WRITE_BIT;
    beamDataBarriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    beamDataBarriers[0].buffer                  = m_beamBuffer.buffer;
    beamDataBarriers[0].offset                 = 0;
    beamDataBarriers[0].size                   = sizeof(uint) * 2;  // for sub beamphoton counter and beam counter

    beamDataBarriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    beamDataBarriers[1].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    beamDataBarriers[1].buffer        = m_beamAsInfoBuffer.buffer;
    beamDataBarriers[1].offset        = 0;
    beamDataBarriers[1].size = m_maxNumSubBeams * sizeof(ShaderVkAccelerationStructureInstanceKHR);  // for sub beamphoton counter and beam counter

    vkCmdPipelineBarrier(
        cmdBuf, 
        VK_PIPELINE_STAGE_TRANSFER_BIT, 
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        0,
        0, nullptr, 
        2, beamDataBarriers, 
        0, nullptr
    );

    auto& regions = m_pbSbtWrapper.getRegions();
    vkCmdTraceRaysKHR(
        cmdBuf, 
        &regions[0], &regions[1], &regions[2], &regions[3],
         // It seems 4096 is the maximum allowed value for the next 3 parameters, larger value does not lauhcn ray tracing
         4, 4, MAX(m_numPhotonSamples, m_numBeamSamples) / 16
    );


    VkBufferMemoryBarrier subBeamDataBarrier = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};

    subBeamDataBarrier.srcAccessMask  = VK_ACCESS_SHADER_WRITE_BIT;
    subBeamDataBarrier.dstAccessMask  = VK_ACCESS_MEMORY_READ_BIT;
    subBeamDataBarrier.buffer         = m_beamAsInfoBuffer.buffer;
    subBeamDataBarrier.offset         = 0;
    subBeamDataBarrier.size = m_maxNumSubBeams * sizeof(ShaderVkAccelerationStructureInstanceKHR);  // for sub beamphoton counter and beam counter
  
    vkCmdPipelineBarrier(
        cmdBuf, 
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,         
        VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 
        0, 
        0, 
        nullptr, 
        1, 
        &subBeamDataBarrier, 
        0, 
        nullptr
    );


    VkBuildAccelerationStructureFlagsKHR flags  = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    bool                                 update = m_pbTlas.accel != nullptr;
    bool                                 motion = false;

    VkBufferDeviceAddressInfo bufferInfo{
        VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, 
        nullptr, 
        m_beamAsInfoBuffer.buffer
    };
    VkDeviceAddress instBufferAddr = vkGetBufferDeviceAddress(m_device, &bufferInfo);

    // Wraps a device pointer to the above uploaded instances.
    VkAccelerationStructureGeometryInstancesDataKHR instancesVk{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
    instancesVk.data.deviceAddress = instBufferAddr;

    // Put the above into a VkAccelerationStructureGeometryKHR. We need to put the instances struct in a union and label it as instance data.
    VkAccelerationStructureGeometryKHR topASGeometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    topASGeometry.geometryType       = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    topASGeometry.geometry.instances = instancesVk;

    // Find sizes
    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    buildInfo.flags         = flags;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries   = &topASGeometry;
    buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.type                     = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    buildInfo.srcAccelerationStructure = VK_NULL_HANDLE;


    VkAccelerationStructureBuildSizesInfoKHR sizeInfo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkGetAccelerationStructureBuildSizesKHR(
        m_device, 
        VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, 
        &buildInfo, 
        &m_maxNumSubBeams, 
        &sizeInfo
    );

    // Create TLAS
    if(update == false)
    {
        VkAccelerationStructureCreateInfoKHR createInfo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        createInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        createInfo.size = sizeInfo.accelerationStructureSize;

        m_pbTlas = m_alloc.createAcceleration(createInfo);

    }


    // Allocate the scratch memory
    if(m_beamTlasScratchBuffer.buffer == VK_NULL_HANDLE)
    {

        m_beamTlasScratchBuffer = m_alloc.createBuffer(
            sizeInfo.buildScratchSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
        );
    }

    VkBufferDeviceAddressInfo scratchBufferInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, nullptr, m_beamTlasScratchBuffer.buffer};
    VkDeviceAddress           scratchAddress = vkGetBufferDeviceAddress(m_device, &scratchBufferInfo);

    // Update build information
    buildInfo.srcAccelerationStructure  = update ? m_pbTlas.accel : VK_NULL_HANDLE;
    buildInfo.dstAccelerationStructure  = m_pbTlas.accel;
    buildInfo.scratchData.deviceAddress = scratchAddress;

    // Build Offsets info: n instances
    VkAccelerationStructureBuildRangeInfoKHR        buildOffsetInfo{m_maxNumSubBeams, 0, 0, 0};
    const VkAccelerationStructureBuildRangeInfoKHR* pBuildOffsetInfo = &buildOffsetInfo;

    // Build the TLAS
    vkCmdBuildAccelerationStructuresKHR(cmdBuf, 1, &buildInfo, &pBuildOffsetInfo);
    
    m_debug.endLabel(cmdBuf);

    VkMemoryBarrier tlasBarrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};

    tlasBarrier.srcAccessMask        = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    tlasBarrier.dstAccessMask        = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;

    vkCmdPipelineBarrier(
        cmdBuf, 
        VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        0, 
        1, 
        &tlasBarrier, 
        0, 
        nullptr, 
        0, 
        nullptr
    );

}

//--------------------------------------------------------------------------------------------------
// Pipeline for the ray tracer: all shaders, raygen, chit, miss
//
void HelloVulkan::createRtPipeline()
{
  enum StageIndices
  {
    eRaygen,
    eMiss,
    eClosestHit,
    eIntersection,
    eIntersectionSurface,
    eAnyHit,
    eAnyHitSurface,
    eShaderGroupCount
  };

  // All stages
  std::array<VkPipelineShaderStageCreateInfo, eShaderGroupCount> stages{};
  VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  stage.pName = "main";  // All the same entry point
  // Raygen
  stage.module = nvvk::createShaderModule(m_device, nvh::loadFile("spv/raytrace.rgen.spv", true, defaultSearchPaths, true));
  stage.stage     = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
  stages[eRaygen] = stage;
  // Miss
  stage.module = nvvk::createShaderModule(m_device, nvh::loadFile("spv/raytrace.rmiss.spv", true, defaultSearchPaths, true));
  stage.stage   = VK_SHADER_STAGE_MISS_BIT_KHR;
  stages[eMiss] = stage;
  // Hit Group - Closest Hit
  stage.module = nvvk::createShaderModule(m_device, nvh::loadFile("spv/raytrace.rchit.spv", true, defaultSearchPaths, true));
  stage.stage         = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
  stages[eClosestHit] = stage;

  stage.module = nvvk::createShaderModule(m_device, nvh::loadFile("spv/raytrace.rint.spv", true, defaultSearchPaths, true));
  stage.stage           = VK_SHADER_STAGE_INTERSECTION_BIT_KHR;
  stages[eIntersection] = stage;

  stage.module = nvvk::createShaderModule(m_device, nvh::loadFile("spv/raytrace_surface.rint.spv", true, defaultSearchPaths, true));
  stage.stage           = VK_SHADER_STAGE_INTERSECTION_BIT_KHR;
  stages[eIntersectionSurface] = stage;

  stage.module = nvvk::createShaderModule(m_device, nvh::loadFile("spv/raytrace.rahit.spv", true, defaultSearchPaths, true));
  stage.stage     = VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
  stages[eAnyHit] = stage;

  stage.module = nvvk::createShaderModule(m_device, nvh::loadFile("spv/raytrace_surface.rahit.spv", true, defaultSearchPaths, true));
  stage.stage     = VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
  stages[eAnyHitSurface] = stage;


  // Shader groups
  VkRayTracingShaderGroupCreateInfoKHR group{VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR};
  group.anyHitShader       = VK_SHADER_UNUSED_KHR;
  group.closestHitShader   = VK_SHADER_UNUSED_KHR;
  group.generalShader      = VK_SHADER_UNUSED_KHR;
  group.intersectionShader = VK_SHADER_UNUSED_KHR;

  // Raygen
  group.type          = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
  group.generalShader = eRaygen;
  m_rtShaderGroups.push_back(group);

  // Miss
  group.type          = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
  group.generalShader = eMiss;
  m_rtShaderGroups.push_back(group);

  // closest hit shader + Intersection
  group.type               = VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR;
  group.generalShader    = VK_SHADER_UNUSED_KHR;
  group.closestHitShader = eClosestHit;
  group.intersectionShader = eIntersection;
  group.anyHitShader       = eAnyHit;
  m_rtShaderGroups.push_back(group);


  // closest hit shader + Intersection
  group.type               = VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR;
  group.generalShader      = VK_SHADER_UNUSED_KHR;
  group.closestHitShader   = eClosestHit;
  group.intersectionShader = eIntersectionSurface;
  group.anyHitShader       = eAnyHitSurface;
  m_rtShaderGroups.push_back(group);


  // Push constant: we want to be able to update constants used by the shaders
  VkPushConstantRange pushConstant{
      VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR 
      | VK_SHADER_STAGE_INTERSECTION_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
      0, sizeof(PushConstantRay)
  };


  VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pipelineLayoutCreateInfo.pushConstantRangeCount = 1;
  pipelineLayoutCreateInfo.pPushConstantRanges    = &pushConstant;

  // Descriptor sets: one specific to ray tracing, and one shared with the rasterization pipeline
  std::vector<VkDescriptorSetLayout> rtDescSetLayouts = {m_rtDescSetLayout, m_descSetLayout};
  pipelineLayoutCreateInfo.setLayoutCount             = static_cast<uint32_t>(rtDescSetLayouts.size());
  pipelineLayoutCreateInfo.pSetLayouts                = rtDescSetLayouts.data();

  vkCreatePipelineLayout(m_device, &pipelineLayoutCreateInfo, nullptr, &m_rtPipelineLayout);


  // Assemble the shader stages and recursion depth info into the ray tracing pipeline
  VkRayTracingPipelineCreateInfoKHR rayPipelineInfo{VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
  rayPipelineInfo.stageCount = static_cast<uint32_t>(stages.size());  // Stages are shaders
  rayPipelineInfo.pStages    = stages.data();

  // In this case, m_rtShaderGroups.size() == 4: we have one raygen group,
  // two miss shader groups, and one hit group.
  rayPipelineInfo.groupCount = static_cast<uint32_t>(m_rtShaderGroups.size());
  rayPipelineInfo.pGroups    = m_rtShaderGroups.data();

  // The ray tracing process can shoot rays from the camera, and a shadow ray can be shot from the
  // hit points of the camera rays, hence a recursion level of 2. This number should be kept as low
  // as possible for performance reasons. Even recursive ray tracing should be flattened into a loop
  // in the ray generation to avoid deep recursion.
  rayPipelineInfo.maxPipelineRayRecursionDepth = 2;  // Ray depth
  rayPipelineInfo.layout                       = m_rtPipelineLayout;

  vkCreateRayTracingPipelinesKHR(m_device, {}, {}, 1, &rayPipelineInfo, nullptr, &m_rtPipeline);


  // Creating the SBT
  m_sbtWrapper.create(m_rtPipeline, rayPipelineInfo);


  for(auto& s : stages)
    vkDestroyShaderModule(m_device, s.module, nullptr);
}

//--------------------------------------------------------------------------------------------------
// Ray Tracing the scene
//
void HelloVulkan::raytrace(const VkCommandBuffer& cmdBuf)
{
    updateFrame();

    m_debug.beginLabel(cmdBuf, "Ray trace");

    std::vector<VkDescriptorSet> descSets{m_rtDescSet, m_descSet};
    vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, m_rtPipeline);
    vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, m_rtPipelineLayout, 0,
                            (uint32_t)descSets.size(), descSets.data(), 0, nullptr);

    m_pcRay.numBeamSources   = m_numBeamSamples;
    m_pcRay.numPhotonSources = m_numPhotonSamples;
    vkCmdPushConstants(
        cmdBuf, 
        m_rtPipelineLayout,
        VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR 
        | VK_SHADER_STAGE_INTERSECTION_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
        0, sizeof(PushConstantRay), &m_pcRay
    );


    auto& regions = m_sbtWrapper.getRegions();
    vkCmdTraceRaysKHR(
        cmdBuf, 
        &regions[0], &regions[1], &regions[2], &regions[3], 
        m_size.width, m_size.height, 1
    );


    m_debug.endLabel(cmdBuf);
}

//--------------------------------------------------------------------------------------------------
// If the camera matrix has changed, resets the frame.
// otherwise, increments frame.
//
void HelloVulkan::updateFrame()
{
  static nvmath::mat4f refCamMatrix;
  static float         refFov{CameraManip.getFov()};

  const auto& m   = CameraManip.getMatrix();
  const auto  fov = CameraManip.getFov();

  if(memcmp(&refCamMatrix.a00, &m.a00, sizeof(nvmath::mat4f)) != 0 || refFov != fov)
  {
    refCamMatrix = m;
    refFov       = fov;
  }
}

