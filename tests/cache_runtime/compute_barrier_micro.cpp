// SPDX-License-Identifier: GPL-2.0-or-later
// Existing Vulkan loader/SDK only. Headless, <1 MiB buffers, no game or external collector.
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <utility>
#include <vector>
#define VK_ENABLE_BETA_EXTENSIONS
#include <vulkan/vulkan.h>
#include "video_core/renderer_vulkan/vk_command_stream.h"
#include "video_core/renderer_vulkan/vk_compute_barrier_policy.h"
#include "video_core/renderer_vulkan/vk_readback_copy_policy.h"

static std::atomic<unsigned> errors{};
static VKAPI_ATTR VkBool32 VKAPI_CALL Debug(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                            VkDebugUtilsMessageTypeFlagsEXT,
                                            const VkDebugUtilsMessengerCallbackDataEXT* data,
                                            void*) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ++errors;
        std::fprintf(stderr, "SYNC VALIDATION: %s\n", data->pMessage);
    }
    return VK_FALSE;
}
static void Check(bool ok, const char* text) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", text);
        std::exit(1);
    }
}
static void Vk(VkResult result) {
    Check(result == VK_SUCCESS, "Vulkan call failed");
}
static std::vector<uint32_t> ReadShader(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    Check(bool(file), "shader missing");
    std::vector<uint32_t> code(static_cast<size_t>(file.tellg()) / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(code.data()), code.size() * 4);
    Check(bool(file) && !code.empty(), "shader read failed");
    return code;
}

static void RunIndexedDrawMicro(VkPhysicalDevice physical, VkDevice device, VkQueue queue,
                                uint32_t family, const VkPhysicalDeviceProperties& properties,
                                uint32_t timestamp_bits, const std::vector<uint32_t>& code,
                                bool timing) {
    struct MappedBuffer {
        VkDevice device;
        VkBuffer buffer{};
        VkDeviceMemory memory{};
        uint8_t* data{};
        VkMemoryPropertyFlags flags{};

        MappedBuffer(VkPhysicalDevice physical, VkDevice device_, VkDeviceSize size,
                     VkBufferUsageFlags usage, bool stream) : device{device_} {
            VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            info.size = size;
            info.usage = usage;
            Vk(vkCreateBuffer(device, &info, nullptr, &buffer));
            VkMemoryRequirements req{};
            vkGetBufferMemoryRequirements(device, buffer, &req);
            VkPhysicalDeviceMemoryProperties memories{};
            vkGetPhysicalDeviceMemoryProperties(physical, &memories);
            uint32_t chosen = memories.memoryTypeCount;
            int best = -1;
            constexpr auto Required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            for (uint32_t i = 0; i < memories.memoryTypeCount; ++i) {
                const auto candidate = memories.memoryTypes[i].propertyFlags;
                if (!(req.memoryTypeBits & (1u << i)) || (candidate & Required) != Required) {
                    continue;
                }
                const bool cached = (candidate & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0;
                const bool local = (candidate & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
                const int score = stream ? 2 * local + !cached : 2 * cached + local;
                if (score > best) {
                    chosen = i;
                    flags = candidate;
                    best = score;
                }
            }
            Check(best >= 0, "no compatible mapped buffer memory");
            VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocation.allocationSize = req.size;
            allocation.memoryTypeIndex = chosen;
            Vk(vkAllocateMemory(device, &allocation, nullptr, &memory));
            Vk(vkBindBufferMemory(device, buffer, memory, 0));
            void* ptr{};
            Vk(vkMapMemory(device, memory, 0, size, 0, &ptr));
            data = static_cast<uint8_t*>(ptr);
        }
        ~MappedBuffer() {
            vkUnmapMemory(device, memory);
            vkDestroyBuffer(device, buffer, nullptr);
            vkFreeMemory(device, memory, nullptr);
        }
    };
    using Args = VkDrawIndexedIndirectCommand;
    constexpr uint32_t Draws = 4096;
    constexpr uint32_t Vertices = Draws * 3;
    constexpr uint32_t Words = 3 * Vertices * 5;
    constexpr uint32_t Sentinel = 0xdeadbeef;
    const VkDeviceSize alignment = properties.limits.minUniformBufferOffsetAlignment;
    const VkDeviceSize stride = ((sizeof(Args) + alignment - 1) / alignment) * alignment;
    MappedBuffer args{physical, device, stride * Draws, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, true};
    MappedBuffer indices{physical, device, (Vertices + 3) * sizeof(uint32_t),
                         VK_BUFFER_USAGE_INDEX_BUFFER_BIT, false};
    MappedBuffer output{physical, device, Words * sizeof(uint32_t),
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false};
    std::vector<Args> cpu(Draws);
    std::vector<uint32_t> expected(Words, Sentinel);
    auto* index_data = reinterpret_cast<uint32_t*>(indices.data);
    for (uint32_t i = 0; i < Draws; ++i) {
        auto& draw = cpu[i];
        draw = {timing || i % 11 ? 3u : 0u, timing ? 1u : i % 13 ? 1u + i % 2 : 0u,
                3 * i + 3, static_cast<int32_t>(i % 7) - 3, !timing && i % 3 == 0};
        std::memcpy(args.data + i * stride, &draw, sizeof(draw));
        for (uint32_t j = 0; j < 3; ++j) {
            const uint32_t vertex = 3 * i + j;
            index_data[draw.firstIndex + j] =
                static_cast<uint32_t>(static_cast<int32_t>(vertex) - draw.vertexOffset);
            for (uint32_t inst = 0; draw.indexCount != 0 && inst < draw.instanceCount; ++inst) {
                const uint32_t instance = draw.firstInstance + inst;
                const std::array words{vertex, instance, static_cast<uint32_t>(draw.vertexOffset),
                                       draw.firstInstance, 0u};
                std::copy(words.begin(), words.end(),
                          expected.begin() + (instance * Vertices + vertex) * words.size());
            }
        }
    }

    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                         VK_SHADER_STAGE_VERTEX_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout_info.bindingCount = 1;
    layout_info.pBindings = &binding;
    VkDescriptorSetLayout set_layout{};
    Vk(vkCreateDescriptorSetLayout(device, &layout_info, nullptr, &set_layout));
    VkPipelineLayoutCreateInfo pipeline_layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipeline_layout_info.setLayoutCount = 1;
    pipeline_layout_info.pSetLayouts = &set_layout;
    VkPipelineLayout pipeline_layout{};
    Vk(vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr, &pipeline_layout));
    VkDescriptorPoolSize pool_size{binding.descriptorType, 1};
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    VkDescriptorPool pool{};
    Vk(vkCreateDescriptorPool(device, &pool_info, nullptr, &pool));
    VkDescriptorSetAllocateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    set_info.descriptorPool = pool;
    set_info.descriptorSetCount = 1;
    set_info.pSetLayouts = &set_layout;
    VkDescriptorSet set{};
    Vk(vkAllocateDescriptorSets(device, &set_info, &set));
    VkDescriptorBufferInfo output_info{output.buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.descriptorCount = 1;
    write.descriptorType = binding.descriptorType;
    write.pBufferInfo = &output_info;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    module_info.codeSize = code.size() * sizeof(uint32_t);
    module_info.pCode = code.data();
    VkShaderModule module{};
    Vk(vkCreateShaderModule(device, &module_info, nullptr, &module));
    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    stage.module = module;
    stage.pName = "main";
    VkPipelineVertexInputStateCreateInfo vertices{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineRasterizationStateCreateInfo raster{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.rasterizerDiscardEnable = VK_TRUE;
    raster.lineWidth = 1.0f;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    VkGraphicsPipelineCreateInfo graphics{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    graphics.pNext = &rendering;
    graphics.stageCount = 1;
    graphics.pStages = &stage;
    graphics.pVertexInputState = &vertices;
    graphics.pInputAssemblyState = &assembly;
    graphics.pRasterizationState = &raster;
    graphics.layout = pipeline_layout;
    VkPipeline pipeline{};
    Vk(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &graphics, nullptr, &pipeline));

    VkCommandPoolCreateInfo commands_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    commands_info.queueFamilyIndex = family;
    VkCommandPool commands{};
    Vk(vkCreateCommandPool(device, &commands_info, nullptr, &commands));
    VkCommandBufferAllocateInfo cmd_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cmd_info.commandPool = commands;
    cmd_info.commandBufferCount = 1;
    VkCommandBuffer cmd{};
    Vk(vkAllocateCommandBuffers(device, &cmd_info, &cmd));
    VkQueryPoolCreateInfo query_info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    query_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    query_info.queryCount = 2;
    VkQueryPool queries{};
    Vk(vkCreateQueryPool(device, &query_info, nullptr, &queries));
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence{};
    Vk(vkCreateFence(device, &fence_info, nullptr, &fence));

    enum class Mode { Indirect, DirectCached, DirectMapped };
    struct Times {
        double cpu_ns, gpu_ns;
    };
    const auto run = [&](Mode mode, uint32_t draws) {
        auto* results = reinterpret_cast<uint32_t*>(output.data);
        std::fill_n(results, Words, Sentinel);
        Vk(vkResetCommandPool(device, commands, 0));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        Vk(vkBeginCommandBuffer(cmd, &begin));
        vkCmdResetQueryPool(cmd, queries, 0, 2);
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, queries, 0);
        VkRenderingInfo scope{VK_STRUCTURE_TYPE_RENDERING_INFO};
        scope.renderArea.extent = {1, 1};
        scope.layerCount = 1;
        vkCmdBeginRendering(cmd, &scope);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1, &set,
                                0, nullptr);
        vkCmdBindIndexBuffer(cmd, indices.buffer, 0, VK_INDEX_TYPE_UINT32);
        const auto start = std::chrono::steady_clock::now();
        if (mode == Mode::Indirect) {
            for (uint32_t i = 0; i < draws; ++i) {
                vkCmdDrawIndexedIndirect(cmd, args.buffer, i * stride, 1, sizeof(Args));
            }
        } else {
            for (uint32_t i = 0; i < draws; ++i) {
                Args draw;
                const void* source = mode == Mode::DirectMapped
                                         ? static_cast<const void*>(args.data + i * stride)
                                         : static_cast<const void*>(&cpu[i]);
                std::memcpy(&draw, source, sizeof(draw));
                vkCmdDrawIndexed(cmd, draw.indexCount, draw.instanceCount, draw.firstIndex,
                                 draw.vertexOffset, draw.firstInstance);
            }
        }
        const auto elapsed = std::chrono::steady_clock::now() - start;
        vkCmdEndRendering(cmd);
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, queries, 1);
        VkMemoryBarrier2 host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        host.srcStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
        host.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &host;
        vkCmdPipelineBarrier2(cmd, &dependency);
        Vk(vkEndCommandBuffer(cmd));
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        Vk(vkQueueSubmit(queue, 1, &submit, fence));
        Vk(vkWaitForFences(device, 1, &fence, VK_TRUE, 5'000'000'000));
        Vk(vkResetFences(device, 1, &fence));
        std::array<uint64_t, 2> ticks;
        Vk(vkGetQueryPoolResults(device, queries, 0, 2, sizeof(ticks), ticks.data(),
                                 sizeof(uint64_t), VK_QUERY_RESULT_64_BIT));
        if (!timing) {
            Check(std::equal(expected.begin(), expected.end(), results),
                  "indexed draw parameters or shader-visible output differ");
        }
        const uint64_t mask = ~uint64_t{0} >> (64 - timestamp_bits);
        return Times{std::chrono::duration<double, std::nano>(elapsed).count() / draws,
                     static_cast<double>((ticks[1] - ticks[0]) & mask) *
                         properties.limits.timestampPeriod / draws};
    };

    constexpr std::array modes{Mode::Indirect, Mode::DirectCached, Mode::DirectMapped};
    std::printf("%s; argument memory flags 0x%x, stride %llu B\n", properties.deviceName,
                args.flags, static_cast<unsigned long long>(stride));
    if (timing) {
        constexpr unsigned Repeats = 18;
        for (const uint32_t draws : {256u, Draws}) {
            std::array<std::array<Times, Repeats>, modes.size()> samples;
            for (const auto mode : modes) {
                run(mode, draws);
            }
            for (unsigned repeat = 0; repeat < Repeats; ++repeat) {
                for (unsigned step = 0; step < modes.size(); ++step) {
                    const unsigned index = (step + repeat) % modes.size();
                    samples[index][repeat] = run(modes[index], draws);
                }
            }
            const auto median = [](const auto& rows, auto select) {
                std::array<double, Repeats> sorted;
                std::ranges::transform(rows, sorted.begin(), select);
                std::ranges::sort(sorted);
                return (sorted[Repeats / 2 - 1] + sorted[Repeats / 2]) / 2;
            };
            std::printf("%u draws, median ns/draw (indirect / direct cached / direct mapped)\n",
                        draws);
            for (const auto& rows : samples) {
                std::printf("  CPU %.1f GPU %.1f\n", median(rows, &Times::cpu_ns),
                            median(rows, &Times::gpu_ns));
            }
        }
    } else {
        for (const auto mode : modes) {
            run(mode, Draws);
        }
        std::printf("PASS: %u indexed draws per path, %u exact output words per path\n",
                    Draws, Words);
    }

    vkDestroyFence(device, fence, nullptr);
    vkDestroyQueryPool(device, queries, nullptr);
    vkDestroyCommandPool(device, commands, nullptr);
    vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyShaderModule(device, module, nullptr);
    vkDestroyDescriptorPool(device, pool, nullptr);
    vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
    vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
}

int main(int argc, char** argv) {
    const bool tile = argc == 4 && std::strcmp(argv[3], "--tile") == 0;
    const bool scan_gate = argc == 4 && std::strcmp(argv[3], "--scan-gate") == 0;
    const bool dispatch_pair = argc == 4 && std::strcmp(argv[3], "--dispatch-pair") == 0;
    const bool template_timing =
        argc == 3 && std::strcmp(argv[2], "--descriptor-template-timing") == 0;
    const bool descriptor_template =
        template_timing || (argc == 3 && std::strcmp(argv[2], "--descriptor-template") == 0);
    const bool draw_timing = argc == 3 && std::strcmp(argv[2], "--indexed-draw-timing") == 0;
    const bool draw_micro = draw_timing ||
                            (argc == 3 && std::strcmp(argv[2], "--indexed-draw") == 0);
    const bool timing_only = template_timing || draw_timing;
    Check(argc == 2 || argc == 3 || tile || scan_gate || dispatch_pair,
          "shader path or original and candidate paths required");
    const bool readback_stage = argc == 3 && std::strcmp(argv[2], "--readback-stage") == 0;
    const bool scan = (argc == 3 && !readback_stage && !descriptor_template && !draw_micro) ||
                      scan_gate;
    const auto code = ReadShader(argv[1]);
    uint32_t count{};
    Vk(vkEnumerateInstanceLayerProperties(&count, nullptr));
    std::vector<VkLayerProperties> layers(count);
    Vk(vkEnumerateInstanceLayerProperties(&count, layers.data()));
    Check(std::any_of(layers.begin(), layers.end(),
                      [](const auto& l) {
                          return std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0;
                      }),
          "installed synchronization validation layer unavailable");
    const char* layer = "VK_LAYER_KHRONOS_validation";
    const char* extension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "IFSS compute barrier micro";
    app.apiVersion = VK_API_VERSION_1_3;
    VkValidationFeatureEnableEXT sync = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validation{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    validation.enabledValidationFeatureCount = 1;
    validation.pEnabledValidationFeatures = &sync;
    VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    create.pNext = timing_only ? nullptr : &validation;
    create.pApplicationInfo = &app;
    create.enabledLayerCount = timing_only ? 0u : 1u;
    create.ppEnabledLayerNames = &layer;
    create.enabledExtensionCount = 1;
    create.ppEnabledExtensionNames = &extension;
    VkInstance instance{};
    Vk(vkCreateInstance(&create, nullptr, &instance));
    auto addDebug = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
    auto removeDebug = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
    VkDebugUtilsMessengerCreateInfoEXT debug{
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
    debug.pfnUserCallback = Debug;
    VkDebugUtilsMessengerEXT messenger{};
    Vk(addDebug(instance, &debug, nullptr, &messenger));
    Vk(vkEnumeratePhysicalDevices(instance, &count, nullptr));
    std::vector<VkPhysicalDevice> devices(count);
    Vk(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
    Check(!devices.empty(), "no GPU");
    auto physical = devices.front();
    for (auto p : devices) {
        VkPhysicalDeviceProperties prop{};
        vkGetPhysicalDeviceProperties(p, &prop);
        if (prop.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            physical = p;
            break;
        }
    }
    VkPhysicalDeviceProperties prop{};
    vkGetPhysicalDeviceProperties(physical, &prop);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    uint32_t family = 0;
    const VkQueueFlags needed = VK_QUEUE_COMPUTE_BIT |
                               (readback_stage || draw_micro ? VK_QUEUE_GRAPHICS_BIT : 0);
    while (family < count && (families[family].queueFlags & needed) != needed)
        ++family;
    Check(family < count, "no compute queue");
    if (tile) {
        VkPhysicalDeviceSubgroupProperties subgroup{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties.pNext = &subgroup;
        vkGetPhysicalDeviceProperties2(physical, &properties);
        Check((subgroup.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) &&
                  (subgroup.supportedOperations & VK_SUBGROUP_FEATURE_ARITHMETIC_BIT) &&
                  families[family].timestampValidBits,
              "compute subgroup arithmetic or timestamps unavailable");
        std::printf("Compute subgroup size: %u\n", subgroup.subgroupSize);
    }
    float priority = 1;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    VkPhysicalDeviceVulkan13Features features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    features.synchronization2 = VK_TRUE;
    VkPhysicalDeviceFeatures core{};
    VkPhysicalDeviceVulkan11Features v11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceRawAccessChainsFeaturesNV raw{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAW_ACCESS_CHAINS_FEATURES_NV};
    const std::array compute_extensions{VK_NV_RAW_ACCESS_CHAINS_EXTENSION_NAME,
                                        VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.pNext = &features;
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    if (scan || dispatch_pair || descriptor_template) {
        core.shaderInt64 = VK_TRUE;
        core.shaderInt16 = VK_TRUE;
        v11.storageBuffer16BitAccess = VK_TRUE;
        v12.storageBuffer8BitAccess = VK_TRUE;
        v12.shaderInt8 = VK_TRUE;
        raw.shaderRawAccessChains = VK_TRUE;
        features.pNext = &v12;
        v12.pNext = &v11;
        v11.pNext = &raw;
        di.pEnabledFeatures = &core;
        di.enabledExtensionCount = descriptor_template ? 2u : 1u;
        di.ppEnabledExtensionNames = compute_extensions.data();
    }
    if (draw_micro) {
        Check(families[family].timestampValidBits != 0, "no graphics timestamp support");
        core.drawIndirectFirstInstance = VK_TRUE;
        core.vertexPipelineStoresAndAtomics = VK_TRUE;
        v11.shaderDrawParameters = VK_TRUE;
        features.dynamicRendering = VK_TRUE;
        features.pNext = &v11;
        di.pEnabledFeatures = &core;
    }
    VkDevice device{};
    Vk(vkCreateDevice(physical, &di, nullptr, &device));
    VkQueue queue{};
    vkGetDeviceQueue(device, family, 0, &queue);
    if (draw_micro) {
        RunIndexedDrawMicro(physical, device, queue, family, prop,
                            families[family].timestampValidBits, code, draw_timing);
        vkDestroyDevice(device, nullptr);
        removeDebug(instance, messenger, nullptr);
        vkDestroyInstance(instance, nullptr);
        return errors.load() == 0 ? 0 : 1;
    }
    constexpr uint32_t PairBindings = 18;
    constexpr uint32_t PairRegionWords = 4096;
    const uint32_t N = dispatch_pair ? PairBindings * PairRegionWords :
                       descriptor_template ? 9 * PairRegionWords :
                       scan ? 7 * 32768 : 65536;
    const uint32_t Bytes = N * 4;
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = Bytes;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (readback_stage) bi.usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    Vk(vkCreateBuffer(device, &bi, nullptr, &buffer));
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device, buffer, &req);
    VkPhysicalDeviceMemoryProperties memories{};
    vkGetPhysicalDeviceMemoryProperties(physical, &memories);
    uint32_t type = 0;
    for (; type < memories.memoryTypeCount; ++type)
        if ((req.memoryTypeBits & (1u << type)) &&
            (memories.memoryTypes[type].propertyFlags &
             (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
            break;
    Check(type < memories.memoryTypeCount, "no host coherent memory");
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    Vk(vkAllocateMemory(device, &ai, nullptr, &memory));
    Vk(vkBindBufferMemory(device, buffer, memory, 0));
    uint32_t* mapped{};
    Vk(vkMapMemory(device, memory, 0, Bytes, 0, reinterpret_cast<void**>(&mapped)));
    VkBuffer download{};
    VkDeviceMemory download_memory{};
    uint32_t* downloaded{};
    if (readback_stage) {
        bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        Vk(vkCreateBuffer(device, &bi, nullptr, &download));
        vkGetBufferMemoryRequirements(device, download, &req);
        Check((req.memoryTypeBits & (1u << type)) != 0, "download memory type unavailable");
        ai.allocationSize = req.size;
        Vk(vkAllocateMemory(device, &ai, nullptr, &download_memory));
        Vk(vkBindBufferMemory(device, download, download_memory, 0));
        Vk(vkMapMemory(device, download_memory, 0, Bytes, 0,
                       reinterpret_cast<void**>(&downloaded)));
    }
    const uint32_t bindings_count = dispatch_pair ? PairBindings : descriptor_template ? 9u :
                                    scan ? 7 : 1;
    const uint32_t RegionWords = scan ? 32768 : 4096;
    std::array<VkDescriptorSetLayoutBinding, PairBindings> bindings{};
    for (uint32_t i = 0; i < bindings_count; ++i) {
        bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT};
    }
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    if (descriptor_template) li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    li.bindingCount = bindings_count;
    li.pBindings = bindings.data();
    VkDescriptorSetLayout layout{};
    Vk(vkCreateDescriptorSetLayout(device, &li, nullptr, &layout));
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &layout;
    const VkPushConstantRange push_range{VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                        dispatch_pair ? 124u : scan_gate ? 60u : 56u};
    if (scan || dispatch_pair || descriptor_template) {
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &push_range;
    }
    VkPipelineLayout pipelineLayout{};
    Vk(vkCreatePipelineLayout(device, &pli, nullptr, &pipelineLayout));
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                            bindings_count * (dispatch_pair ? 2u : 1u)};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = dispatch_pair ? 2u : 1u;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    VkDescriptorPool pool{};
    VkDescriptorSetAllocateInfo dsa{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsa.descriptorPool = pool;
    dsa.descriptorSetCount = 1;
    dsa.pSetLayouts = &layout;
    VkDescriptorSet set{};
    if (!descriptor_template) {
        Vk(vkCreateDescriptorPool(device, &dpi, nullptr, &pool));
        dsa.descriptorPool = pool;
        Vk(vkAllocateDescriptorSets(device, &dsa, &set));
    }
    std::array<VkDescriptorBufferInfo, PairBindings> buffers{};
    std::array<VkWriteDescriptorSet, PairBindings> writes{};
    for (uint32_t i = 0; i < bindings_count; ++i) {
        buffers[i] = {buffer, i * RegionWords * 4u,
                      scan || dispatch_pair || descriptor_template ? RegionWords * 4u : Bytes};
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &buffers[i];
    }
    if (!descriptor_template) vkUpdateDescriptorSets(device, bindings_count, writes.data(), 0, nullptr);
    const auto make_pipeline = [&](const std::vector<uint32_t>& words) {
        VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smi.codeSize = words.size() * 4;
        smi.pCode = words.data();
        VkShaderModule shader{};
        Vk(vkCreateShaderModule(device, &smi, nullptr, &shader));
        VkComputePipelineCreateInfo pi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pi.layout = pipelineLayout;
        pi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        pi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pi.stage.module = shader;
        pi.stage.pName = "main";
        VkPipeline result{};
        Vk(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pi, nullptr, &result));
        vkDestroyShaderModule(device, shader, nullptr);
        return result;
    };
    const VkPipeline pipeline = make_pipeline(code);
    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.queueFamilyIndex = family;
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool commandPool{};
    Vk(vkCreateCommandPool(device, &cpi, nullptr, &commandPool));
    VkCommandBufferAllocateInfo cba{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cba.commandPool = commandPool;
    cba.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cba.commandBufferCount = 1;
    VkCommandBuffer cmd{};
    Vk(vkAllocateCommandBuffers(device, &cba, &cmd));
    if (descriptor_template) {
        const auto push_write = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(
            vkGetDeviceProcAddr(device, "vkCmdPushDescriptorSetKHR"));
        const auto push_template = reinterpret_cast<PFN_vkCmdPushDescriptorSetWithTemplateKHR>(
            vkGetDeviceProcAddr(device, "vkCmdPushDescriptorSetWithTemplateKHR"));
        Check(push_write && push_template, "push descriptor template entry points unavailable");
        std::array<VkDescriptorUpdateTemplateEntry, 9> entries{};
        for (uint32_t i = 0; i < entries.size(); ++i) {
            entries[i] = {i, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                          i * sizeof(VkDescriptorBufferInfo), sizeof(VkDescriptorBufferInfo)};
        }
        VkDescriptorUpdateTemplateCreateInfo ti{VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO};
        ti.descriptorUpdateEntryCount = static_cast<uint32_t>(entries.size());
        ti.pDescriptorUpdateEntries = entries.data();
        ti.templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR;
        ti.pipelineBindPoint = VK_PIPELINE_BIND_POINT_COMPUTE;
        ti.pipelineLayout = pipelineLayout;
        VkDescriptorUpdateTemplate update_template{};
        Vk(vkCreateDescriptorUpdateTemplate(device, &ti, nullptr, &update_template));
        const auto push = [&](bool templated) {
            if (templated) {
                push_template(cmd, update_template, pipelineLayout, 0, buffers.data());
            } else {
                push_write(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0,
                           bindings_count, writes.data());
            }
        };
        const auto begin_recording = [&] {
            Vk(vkResetCommandPool(device, commandPool, 0));
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            Vk(vkBeginCommandBuffer(cmd, &begin));
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        };
        const auto dispatch = [&](bool templated) {
            std::fill_n(mapped, N, 0);
            std::fill_n(mapped + 7 * RegionWords, RegionWords, 0xaaaaaaaau);
            mapped[8 * RegionWords + 30] = 107;
            begin_recording();
            const std::array<uint32_t, 14> constants{};
            vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                               sizeof(constants), constants.data());
            push(templated);
            vkCmdDispatch(cmd, 2, 1, 1);
            VkMemoryBarrier2 host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            host.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            host.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
            host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
            host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.memoryBarrierCount = 1;
            dep.pMemoryBarriers = &host;
            vkCmdPipelineBarrier2(cmd, &dep);
            Vk(vkEndCommandBuffer(cmd));
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &cmd;
            Vk(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
            Vk(vkQueueWaitIdle(queue));
        };
        dispatch(false);
        Check(mapped[7 * RegionWords] != 0xaaaaaaaau, "original descriptors produced no output");
        const std::vector<uint32_t> reference(mapped, mapped + N);
        dispatch(true);
        Check(std::equal(reference.begin(), reference.end(), mapped),
              "templated descriptors produced different outputs");
        if (template_timing) {
            constexpr unsigned Updates = 8192;
            const auto measure = [&](bool templated) {
                begin_recording();
                const auto start = std::chrono::steady_clock::now();
                for (unsigned i = 0; i < Updates; ++i) {
                    buffers[7].range = RegionWords * 4 - 4 * (i & 1);
                    push(templated);
                }
                Vk(vkEndCommandBuffer(cmd));
                return std::chrono::duration<double, std::nano>(
                           std::chrono::steady_clock::now() - start).count() / Updates;
            };
            std::array<std::vector<double>, 2> measurements;
            measure(false);
            measure(true);
            for (unsigned repeat = 0; repeat < 16; ++repeat) {
                for (unsigned order = 0; order < 2; ++order) {
                    const unsigned variant = (repeat + order) % 2;
                    measurements[variant].push_back(measure(variant != 0));
                }
            }
            for (auto& values : measurements) std::ranges::sort(values);
            std::printf("9 SSBOs, CPU recording ns/update: writes %.1f, template %.1f on %s\n",
                        (measurements[0][7] + measurements[0][8]) / 2,
                        (measurements[1][7] + measurements[1][8]) / 2, prop.deviceName);
            Vulkan::CommandStream stream;
            const auto measure_stream = [&](bool templated) {
                begin_recording();
                const auto start = std::chrono::steady_clock::now();
                for (unsigned i = 0; i < Updates; ++i) {
                    buffers[7].range = RegionWords * 4 - 4 * (i & 1);
                    if (templated) {
                        const auto* data = stream.Copy(buffers.data(), bindings_count);
                        stream.Record([=](vk::CommandBuffer command) {
                            push_template(static_cast<VkCommandBuffer>(command), update_template,
                                          pipelineLayout, 0, data);
                        });
                    } else {
                        auto* copied_writes = stream.Copy(writes.data(), bindings_count);
                        for (uint32_t binding = 0; binding < bindings_count; ++binding) {
                            auto& write = copied_writes[binding];
                            write.pBufferInfo = stream.Copy(write.pBufferInfo, 1);
                        }
                        stream.Record([=](vk::CommandBuffer command) {
                            push_write(static_cast<VkCommandBuffer>(command),
                                       VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0,
                                       bindings_count, copied_writes);
                        });
                    }
                }
                const auto produced = std::chrono::steady_clock::now();
                stream.Execute(vk::CommandBuffer{cmd});
                Vk(vkEndCommandBuffer(cmd));
                const auto replayed = std::chrono::steady_clock::now();
                return std::array{
                    std::chrono::duration<double, std::nano>(produced - start).count() / Updates,
                    std::chrono::duration<double, std::nano>(replayed - produced).count() / Updates};
            };
            std::array<std::vector<double>, 2> producer, recorder;
            measure_stream(false);
            measure_stream(true);
            for (unsigned repeat = 0; repeat < 16; ++repeat) {
                for (unsigned order = 0; order < 2; ++order) {
                    const unsigned variant = (repeat + order) % 2;
                    const auto [prepare_ns, replay_ns] = measure_stream(variant != 0);
                    producer[variant].push_back(prepare_ns);
                    recorder[variant].push_back(replay_ns);
                }
            }
            const auto median = [](auto& values) {
                std::ranges::sort(values);
                return (values[7] + values[8]) / 2;
            };
            std::printf("CommandStream ns/update: producer %.1f/%.1f, recorder %.1f/%.1f\n",
                        median(producer[0]), median(producer[1]),
                        median(recorder[0]), median(recorder[1]));
        }
        Vk(vkResetCommandPool(device, commandPool, 0));
        vkDestroyDescriptorUpdateTemplate(device, update_template, nullptr);
        std::printf("PASS: %u words equal with templated descriptor updates\n", N);
    } else if (dispatch_pair) {
        const auto paired = make_pipeline(ReadShader(argv[2]));
        const uint32_t valid_bits = families[family].timestampValidBits;
        Check(valid_bits != 0, "no dispatch pair timestamps");
        const uint64_t mask = valid_bits == 64 ? UINT64_MAX : (uint64_t{1} << valid_bits) - 1;
        VkQueryPoolCreateInfo query_info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        query_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        query_info.queryCount = 2;
        VkQueryPool queries{};
        Vk(vkCreateQueryPool(device, &query_info, nullptr, &queries));
        VkDescriptorSet second{};
        Vk(vkAllocateDescriptorSets(device, &dsa, &second));
        for (uint32_t i = 0; i < PairBindings; ++i) {
            writes[i].dstSet = second;
            writes[i].pBufferInfo = &buffers[(i + 9) % PairBindings];
        }
        vkUpdateDescriptorSets(device, PairBindings, writes.data(), 0, nullptr);
        constexpr uint32_t Sentinel = 0xaaaaaaaau;
        const auto seed = [&](uint32_t active_a, uint32_t active_b) {
            std::fill_n(mapped, N, 0);
            std::fill_n(mapped + 7 * PairRegionWords, PairRegionWords, Sentinel);
            std::fill_n(mapped + 16 * PairRegionWords, PairRegionWords, Sentinel);
            mapped[8 * PairRegionWords + 30] = active_a;
            mapped[17 * PairRegionWords + 30] = active_b;
        };
        const auto run = [&](bool fused, uint32_t groups_a, uint32_t groups_b, uint32_t offset_b) {
            Vk(vkResetCommandPool(device, commandPool, 0));
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            Vk(vkBeginCommandBuffer(cmd, &begin));
            vkCmdResetQueryPool(cmd, queries, 0, 2);
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, queries, 0);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, fused ? paired : pipeline);
            const auto launch = [&](VkDescriptorSet descriptors, uint32_t groups,
                                    std::span<const uint32_t> push) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout,
                                        0, 1, &descriptors, 0, nullptr);
                vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                   static_cast<uint32_t>(push.size_bytes()), push.data());
                vkCmdDispatch(cmd, groups, 1, 1);
            };
            std::array<uint32_t, 31> push{};
            if (fused) {
                push[21] = offset_b << 24; // B starts at byte 64, output is guest buffer 7.
                push[30] = groups_a;
                launch(set, groups_a + groups_b, push);
            } else {
                launch(set, groups_a, std::span{push}.first(14));
                push[5] = offset_b << 24;
                launch(second, groups_b, std::span{push}.first(14));
            }
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queries, 1);
            VkMemoryBarrier2 host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            host.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            host.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
            host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
            host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.memoryBarrierCount = 1;
            dep.pMemoryBarriers = &host;
            vkCmdPipelineBarrier2(cmd, &dep);
            Vk(vkEndCommandBuffer(cmd));
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &cmd;
            Vk(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
            Vk(vkQueueWaitIdle(queue));
            std::array<uint64_t, 2> ticks{};
            Vk(vkGetQueryPoolResults(device, queries, 0, 2, sizeof(ticks), ticks.data(),
                                     sizeof(uint64_t), VK_QUERY_RESULT_64_BIT));
            return static_cast<double>((ticks[1] - ticks[0]) & mask) * prop.limits.timestampPeriod;
        };
        const auto verify_footprint = [&](uint32_t binding, uint32_t byte_offset,
                                          uint32_t active, uint32_t groups) {
            const auto* output = mapped + binding * PairRegionWords;
            const uint32_t first = byte_offset / 2;
            const uint32_t count = std::min(active, groups * 64);
            bool changed = false;
            for (uint32_t i = 0; i < PairRegionWords * 2; ++i) {
                const uint32_t value = (output[i / 2] >> (i % 2 * 16)) & 0xffff;
                if (first <= i && i < first + count) {
                    changed |= value != (Sentinel & 0xffff);
                } else {
                    Check(value == (Sentinel & 0xffff), "shader wrote outside its vertex count");
                }
            }
            Check(changed, "original job must write observable output");
        };
        unsigned cases = 0;
        for (uint32_t groups_a : {1u, 2u, 4u}) {
            for (uint32_t groups_b : {1u, 3u}) {
                const uint32_t active_a = groups_a * 64 + 7;
                const uint32_t active_b = groups_b * 64 - 19;
                constexpr uint32_t OffsetB = 24;
                seed(active_a, active_b);
                run(false, groups_a, groups_b, OffsetB);
                verify_footprint(7, 0, active_a, groups_a);
                verify_footprint(16, OffsetB, active_b, groups_b);
                const std::vector<uint32_t> reference(mapped, mapped + N);
                std::array<std::vector<double>, 2> samples;
                for (unsigned repeat = 0; repeat < 16; ++repeat) {
                    for (unsigned order = 0; order < 2; ++order) {
                        const unsigned variant = (repeat + order) % 2;
                        seed(active_a, active_b);
                        samples[variant].push_back(run(variant != 0, groups_a, groups_b, OffsetB));
                        Check(std::equal(reference.begin(), reference.end(), mapped),
                              "paired dispatch differs from two original dispatches");
                    }
                }
                for (auto& sample : samples) {
                    std::ranges::sort(sample);
                }
                std::printf("%u+%u groups: original %.3f us, fused %.3f us\n",
                            groups_a, groups_b, (samples[0][7] + samples[0][8]) / 2000,
                            (samples[1][7] + samples[1][8]) / 2000);
                ++cases;
            }
        }
        std::printf("PASS: %u fused pairs, all %u words per pair equal on %s\n",
                    cases, N, prop.deviceName);
        vkDestroyQueryPool(device, queries, nullptr);
        vkDestroyPipeline(device, paired, nullptr);
    } else if (tile) {
        const auto alternative = make_pipeline(ReadShader(argv[2]));
        VkQueryPoolCreateInfo query_info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        query_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        query_info.queryCount = 2;
        VkQueryPool queries{};
        Vk(vkCreateQueryPool(device, &query_info, nullptr, &queries));
        const uint32_t groups = N / 2;
        const uint32_t valid_bits = families[family].timestampValidBits;
        const uint64_t mask = valid_bits == 64 ? UINT64_MAX : (uint64_t{1} << valid_bits) - 1;
        const auto run = [&](VkPipeline selected) {
            Vk(vkResetCommandPool(device, commandPool, 0));
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            Vk(vkBeginCommandBuffer(cmd, &begin));
            vkCmdResetQueryPool(cmd, queries, 0, 2);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, selected);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout,
                                    0, 1, &set, 0, nullptr);
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, queries, 0);
            vkCmdDispatch(cmd, groups, 1, 1);
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, queries, 1);
            VkMemoryBarrier2 host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            host.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            host.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
            host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
            host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.memoryBarrierCount = 1;
            dep.pMemoryBarriers = &host;
            vkCmdPipelineBarrier2(cmd, &dep);
            Vk(vkEndCommandBuffer(cmd));
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &cmd;
            Vk(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
            Vk(vkQueueWaitIdle(queue));
            std::array<uint64_t, 2> ticks{};
            Vk(vkGetQueryPoolResults(device, queries, 0, 2, sizeof(ticks), ticks.data(),
                                     sizeof(uint64_t), VK_QUERY_RESULT_64_BIT));
            return static_cast<double>((ticks[1] - ticks[0]) & mask) * prop.limits.timestampPeriod;
        };
        run(pipeline);
        std::vector<uint32_t> reference(mapped, mapped + N);
        for (uint32_t group = 0; group < groups; ++group) {
            int32_t lo = INT32_MAX, hi = INT32_MIN;
            for (uint32_t lane = 0; lane < 256 && (group & 7); ++lane) {
                const uint32_t bits = (lane + 1u) * 2654435761u ^ group * 2246822519u;
                if (bits & 7) {
                    lo = std::min(lo, std::bit_cast<int32_t>(bits));
                    hi = std::max(hi, std::bit_cast<int32_t>(bits));
                }
            }
            Check(reference[2 * group] == std::bit_cast<uint32_t>(lo) &&
                      reference[2 * group + 1] == std::bit_cast<uint32_t>(hi),
                  "baseline tile differs from CPU reference");
        }
        std::array<std::vector<double>, 2> times;
        for (unsigned pair = 0; pair < 12; ++pair) {
            for (unsigned order = 0; order < 2; ++order) {
                const unsigned variant = (pair + order) % 2;
                times[variant].push_back(run(variant ? alternative : pipeline));
                Check(std::equal(reference.begin(), reference.end(), mapped), "GPU tile mismatch");
            }
        }
        for (auto& values : times) {
            std::sort(values.begin(), values.end());
        }
        std::printf("PASS: %u exact min/max tiles per dispatch; median %.3f/%.3f ms on %s\n",
                    groups, (times[0][5] + times[0][6]) / 2e6,
                    (times[1][5] + times[1][6]) / 2e6, prop.deviceName);
        vkDestroyQueryPool(device, queries, nullptr);
        vkDestroyPipeline(device, alternative, nullptr);
    } else if (scan) {
        const auto alternative = make_pipeline(ReadShader(argv[2]));
        VkQueryPoolCreateInfo query_info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        query_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        query_info.queryCount = 2;
        VkQueryPool queries{};
        Check(families[family].timestampValidBits != 0, "no scan timestamps");
        Vk(vkCreateQueryPool(device, &query_info, nullptr, &queries));
        const uint32_t valid_bits = families[family].timestampValidBits;
        const uint64_t mask = valid_bits == 64 ? UINT64_MAX : (uint64_t{1} << valid_bits) - 1;
        uint32_t total_outputs = 0;
        uint32_t slot_order_differences = 0;
        uint32_t equal_key_order_differences = 0;
        struct ScanFixture {
            uint32_t count;
            bool wrapped;
            bool clipped;
            uint32_t gate = 1;
            uint32_t record_offset_words = 0;
            bool tight = false;
        };
        std::vector<ScanFixture> fixtures;
        for (uint32_t count : {0u, 1u, 17u, 33u, 64u, 77u, 129u, 256u, 257u}) {
            fixtures.push_back({count, false, false});
            fixtures.push_back({count, true, false});
        }
        for (uint32_t count : {17u, 64u, 129u, 256u}) {
            fixtures.push_back({count, true, true});
        }
        if (scan_gate) {
            fixtures.push_back({129, false, false, 0});
            fixtures.push_back({256, false, false, 0});
            fixtures.push_back({256, false, false, 2});
            fixtures.push_back({129, false, false, 1, 1, true});
            fixtures.push_back({129, false, true, 1, 1, true});
            fixtures.push_back({256, true, false, 1, 63, true});
            fixtures.push_back({256, true, true, 1, 63, true});
        }
        for (const auto& [count, wrapped, clipped, gate, record_offset_words, tight] : fixtures) {
            std::vector<uint32_t> initial(N);
            const uint32_t base = wrapped ? 3 : 0;
            uint32_t descriptor_words = RegionWords;
            if (tight) {
                descriptor_words = record_offset_words + (base + 3 * count + 1) * 24 - clipped;
            } else if (clipped) {
                // The input fits but the output range required by the guard does not.
                descriptor_words = (base + count) * 24;
            }
            buffers[1].range = descriptor_words * sizeof(uint32_t);
            vkUpdateDescriptorSets(device, 1, &writes[1], 0, nullptr);
            initial[2] = base;
            initial[3] = count;
            std::fill_n(initial.begin() + RegionWords, record_offset_words, 0xdeadbeefu);
            for (uint32_t i = 0; i < count; ++i) {
                auto* record = initial.data() + RegionWords + record_offset_words + (base + i) * 24;
                record[0] = std::bit_cast<uint32_t>(wrapped ? 0.0f : static_cast<float>(i));
                record[1] = std::bit_cast<uint32_t>(2.5f);
                record[2] = std::bit_cast<uint32_t>(0.25f);
                record[7] = (wrapped ? (0xffff0u + (i / 2) * 8) : i * 71u) & 0xfffffu;
            }
            const auto dispatch = [&](VkPipeline selected) {
                std::copy(initial.begin(), initial.end(), mapped);
                Vk(vkResetCommandPool(device, commandPool, 0));
                VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                Vk(vkBeginCommandBuffer(cmd, &begin));
                vkCmdResetQueryPool(cmd, queries, 0, 2);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, selected);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout,
                                        0, 1, &set, 0, nullptr);
                std::array<uint32_t, 15> push{};
                // Binding 1 is the second byte of the packed offsets at byte 16.
                push[4] = record_offset_words * sizeof(uint32_t) << 8;
                push.back() = gate;
                vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                   push_range.size, push.data());
                vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, queries, 0);
                vkCmdDispatch(cmd, std::max(1u, (count + 63) / 64), 1, 1);
                vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, queries, 1);
                VkMemoryBarrier2 host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
                host.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                host.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
                host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
                host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
                VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dep.memoryBarrierCount = 1;
                dep.pMemoryBarriers = &host;
                vkCmdPipelineBarrier2(cmd, &dep);
                Vk(vkEndCommandBuffer(cmd));
                VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                submit.commandBufferCount = 1;
                submit.pCommandBuffers = &cmd;
                Vk(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
                Vk(vkQueueWaitIdle(queue));
                std::array<uint64_t, 2> ticks{};
                Vk(vkGetQueryPoolResults(device, queries, 0, 2, sizeof(ticks), ticks.data(),
                                         sizeof(uint64_t), VK_QUERY_RESULT_64_BIT));
                const double us = static_cast<double>((ticks[1] - ticks[0]) & mask) *
                                  prop.limits.timestampPeriod / 1000.0;
                return std::pair{std::vector<uint32_t>(mapped, mapped + N), us};
            };
            auto [baseline, baseline_us] = dispatch(pipeline);
            auto [candidate, candidate_us] = dispatch(alternative);
            std::printf("scan N=%u wrapped=%u clipped=%u gate=%u offset_words=%u %.2f/%.2f us\n",
                        count,
                        static_cast<unsigned>(wrapped), static_cast<unsigned>(clipped),
                        gate, record_offset_words, baseline_us, candidate_us);
            const uint32_t outputs = baseline[5 * RegionWords + 3];
            Check(outputs <= count && outputs == candidate[5 * RegionWords + 3],
                  "scan counter mismatch");
            total_outputs += outputs;
            const auto list_start = 4 * RegionWords;
            slot_order_differences += !std::equal(
                baseline.begin() + list_start, baseline.begin() + list_start + 2 * outputs,
                candidate.begin() + list_start);
            const auto order_by_key = [&](const std::vector<uint32_t>& snapshot) {
                std::vector<std::pair<uint32_t, uint32_t>> pairs;
                for (uint32_t i = 0; i < outputs; ++i) {
                    const auto key = snapshot[list_start + 2 * i];
                    pairs.emplace_back((key & 0x7fff'ffff) ? key : 0,
                                       snapshot[list_start + 2 * i + 1]);
                }
                std::stable_sort(pairs.begin(), pairs.end(), [](const auto& a, const auto& b) {
                    return a.first < b.first;
                });
                return pairs;
            };
            equal_key_order_differences += order_by_key(baseline) != order_by_key(candidate);
            // Atomic allocations from different lanes need not retire in slot order.
            const auto canonicalize = [&](std::vector<uint32_t>& snapshot) {
                std::vector<std::pair<uint32_t, uint32_t>> pairs;
                auto* list = snapshot.data() + 4 * RegionWords;
                for (uint32_t i = 0; i < outputs; ++i) {
                    pairs.emplace_back(list[2 * i], list[2 * i + 1]);
                }
                std::sort(pairs.begin(), pairs.end());
                for (uint32_t i = 0; i < outputs; ++i) {
                    list[2 * i] = pairs[i].first;
                    list[2 * i + 1] = pairs[i].second;
                }
            };
            canonicalize(baseline);
            canonicalize(candidate);
            Check(baseline == candidate, "scan buffer mismatch");
        }
        Check(total_outputs > 0, "fixture never reached atomic output");
        std::printf("PASS: %zu scan fixtures, %u emitted pairs, %u slot-order differences, "
                    "%u equal-key order differences on %s\n",
                    fixtures.size(), total_outputs, slot_order_differences,
                    equal_key_order_differences,
                    prop.deviceName);
        vkDestroyQueryPool(device, queries, nullptr);
        vkDestroyPipeline(device, alternative, nullptr);
    } else for (bool candidate : {false, true}) {
        Vk(vkResetCommandPool(device, commandPool, 0));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        Vk(vkBeginCommandBuffer(cmd, &begin));
        auto barrier = [&](VkPipelineStageFlags2 src, VkAccessFlags2 access, bool compute) {
            vk::MemoryBarrier2 pending{.srcStageMask = vk::PipelineStageFlags2{src},
                                       .srcAccessMask = vk::AccessFlags2{access},
                                       .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                                       .dstAccessMask = vk::AccessFlagBits2::eMemoryRead |
                                                        vk::AccessFlagBits2::eMemoryWrite};
            if (candidate && compute && Vulkan::CanNarrowComputeBarrier(pending, false))
                pending = Vulkan::ComputeBarrier(pending);
            VkMemoryBarrier2 raw = pending;
            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.memoryBarrierCount = 1;
            dep.pMemoryBarriers = &raw;
            vkCmdPipelineBarrier2(cmd, &dep);
        };
        vkCmdFillBuffer(cmd, buffer, 0, Bytes, 0);
        barrier(VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT, false);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &set, 0,
                                nullptr);
        for (unsigned i = 0; i < 64; ++i) {
            if (i)
                barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, true);
            vkCmdDispatch(cmd, N / 64, 1, 1);
        }
        // Full dependency is still needed for a later non-compute consumer/write.
        barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, false);
        vkCmdFillBuffer(cmd, buffer, 0, Bytes / 2, 7);
        barrier(VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, false);
        for (unsigned i = 0; i < 16; ++i) {
            if (i)
                barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, true);
            vkCmdDispatch(cmd, N / 64, 1, 1);
        }
        if (readback_stage) {
            using Stage = vk::PipelineStageFlagBits2;
            using Access = vk::AccessFlagBits2;
            const vk::MemoryBarrier2 pending{
                .srcStageMask = Stage::eComputeShader | Stage::eAllGraphics,
                .srcAccessMask = Access::eShaderWrite,
                .dstStageMask = Stage::eAllCommands,
                .dstAccessMask = Access::eMemoryRead | Access::eMemoryWrite,
            };
            const auto narrow = Vulkan::NarrowReadbackCopyBarrier(
                pending, Stage::eComputeShader, true, false, false);
            Check(bool(narrow), "readback barrier eligibility");
            const VkMemoryBarrier2 copy_scope = candidate ? *narrow : pending;
            VkDependencyInfo copy_dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            copy_dep.memoryBarrierCount = 1;
            copy_dep.pMemoryBarriers = &copy_scope;
            vkCmdPipelineBarrier2(cmd, &copy_dep);
            const VkBufferCopy copy{0, 0, Bytes};
            vkCmdCopyBuffer(cmd, buffer, download, 1, &copy);
            // The later source write must wait for the copy read. In the candidate,
            // this full barrier also retires the scope that preceded the copy.
            barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT |
                        VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT, false);
            vkCmdFillBuffer(cmd, buffer, 0, sizeof(uint32_t), 0x12345678);
        }
        VkMemoryBarrier2 host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        host.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        host.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
        if (readback_stage) {
            host.srcStageMask |= VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT;
            host.srcAccessMask |= VK_ACCESS_2_TRANSFER_WRITE_BIT;
        }
        host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &host;
        vkCmdPipelineBarrier2(cmd, &dep);
        Vk(vkEndCommandBuffer(cmd));
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        Vk(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
        Vk(vkQueueWaitIdle(queue));
        for (uint32_t i = 0; i < N; ++i) {
            const uint32_t expected = i < N / 2 ? 23u : 80u;
            Check(mapped[i] == (readback_stage && i == 0 ? 0x12345678u : expected),
                  "GPU result mismatch");
            if (readback_stage) Check(downloaded[i] == expected, "readback result mismatch");
        }
    }
    Vk(vkDeviceWaitIdle(device));
    vkDestroyCommandPool(device, commandPool, nullptr);
    vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyDescriptorPool(device, pool, nullptr);
    vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
    vkDestroyDescriptorSetLayout(device, layout, nullptr);
    vkUnmapMemory(device, memory);
    vkDestroyBuffer(device, buffer, nullptr);
    vkFreeMemory(device, memory, nullptr);
    if (readback_stage) {
        vkUnmapMemory(device, download_memory);
        vkDestroyBuffer(device, download, nullptr);
        vkFreeMemory(device, download_memory, nullptr);
        std::puts("PASS: 65536 exact copied words before the later source write");
    }
    vkDestroyDevice(device, nullptr);
    removeDebug(instance, messenger, nullptr);
    vkDestroyInstance(instance, nullptr);
    Check(errors.load() == 0, "validation reported errors");
    if (!scan && !tile && !dispatch_pair && !descriptor_template) std::printf("PASS: %s; A/B compute/transfer/host visibility, 65536 exact results each; "
                "synchronization validation errors=0.\n",
                prop.deviceName);
}
