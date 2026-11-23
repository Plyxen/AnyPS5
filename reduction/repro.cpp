#include <vulkan/vulkan.h>
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

static LONG CALLBACK OnFault(EXCEPTION_POINTERS* info) {
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_INT_DIVIDE_BY_ZERO) return EXCEPTION_CONTINUE_SEARCH;
    const auto address = reinterpret_cast<std::uintptr_t>(info->ExceptionRecord->ExceptionAddress);
    HMODULE module = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(address), &module) && module != nullptr) GetModuleFileNameA(module, name, sizeof(name));
    const char* base = std::strrchr(name, '\\');
    std::printf("RESULT crash code=0x%08lx at %s+0x%llx", code, base != nullptr ? base + 1 : name, static_cast<unsigned long long>(address - reinterpret_cast<std::uintptr_t>(module)));
    if (code == EXCEPTION_ACCESS_VIOLATION) std::printf(" %s of 0x%llx", info->ExceptionRecord->ExceptionInformation[0] == 0 ? "read" : "write", static_cast<unsigned long long>(info->ExceptionRecord->ExceptionInformation[1]));
    std::printf("\n");
    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 3);
    return EXCEPTION_CONTINUE_SEARCH;
}

static std::vector<std::uint32_t> ReadWords(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        std::printf("cannot open %s\n", path.c_str());
        std::exit(2);
    }
    const auto size = static_cast<std::size_t>(file.tellg());
    std::vector<std::uint32_t> words(size / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(words.data()), static_cast<std::streamsize>(words.size() * 4));
    return words;
}

static void Check(VkResult result, const char* what) {
    if (result == VK_SUCCESS) return;
    std::printf("RESULT error %s = %d\n", what, static_cast<int>(result));
    std::fflush(stdout);
    std::exit(2);
}

int main(int argc, char** argv) {
    std::string meshPath;
    std::string fragPath;
    std::string computePath;
    std::string gpu;
    bool subgroup32 = false;
    bool depth = false;
    bool robust = false;
    bool raw = false;
    bool title = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--mesh" && i + 1 < argc) meshPath = argv[++i];
        else if (arg == "--frag" && i + 1 < argc) fragPath = argv[++i];
        else if (arg == "--compute" && i + 1 < argc) computePath = argv[++i];
        else if (arg == "--gpu" && i + 1 < argc) gpu = argv[++i];
        else if (arg == "--subgroup32") subgroup32 = true;
        else if (arg == "--depth") depth = true;
        else if (arg == "--robust") robust = true;
        else if (arg == "--raw") raw = true;
        else if (arg == "--title") title = true;
        else {
            std::printf("usage: repro --mesh mesh.spv [--frag frag.spv] [--gpu name] [--subgroup32] [--depth]\n");
            return 2;
        }
    }
    if (meshPath.empty() && computePath.empty()) return 2;
    if (!raw) AddVectoredExceptionHandler(1, OnFault);

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    Check(vkCreateInstance(&instanceInfo, nullptr, &instance), "vkCreateInstance");

    std::uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties{};
    for (const auto candidate : devices) {
        vkGetPhysicalDeviceProperties(candidate, &properties);
        if (gpu.empty() || std::strstr(properties.deviceName, gpu.c_str()) != nullptr) {
            physical = candidate;
            break;
        }
    }
    if (physical == VK_NULL_HANDLE) {
        std::printf("RESULT error no device matches '%s'\n", gpu.c_str());
        return 2;
    }
    std::printf("device %s vendor=0x%x device=0x%x driver=%u api=%u.%u.%u\n", properties.deviceName, properties.vendorID, properties.deviceID, properties.driverVersion, VK_API_VERSION_MAJOR(properties.apiVersion), VK_API_VERSION_MINOR(properties.apiVersion), VK_API_VERSION_PATCH(properties.apiVersion));

    vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, available.data());
    const auto has = [&](const char* name) {
        for (const auto& extension : available) {
            if (std::strcmp(extension.extensionName, name) == 0) return true;
        }
        return false;
    };
    if (!has(VK_EXT_MESH_SHADER_EXTENSION_NAME)) {
        std::printf("RESULT error no VK_EXT_mesh_shader\n");
        return 2;
    }
    const bool barycentric = has(VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME);
    std::vector<const char*> extensions{VK_EXT_MESH_SHADER_EXTENSION_NAME};
    if (barycentric) extensions.push_back(VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME);
    const bool robustness2 = title && has(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
    if (robustness2) extensions.push_back(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
    VkPhysicalDeviceRobustness2FeaturesEXT robustnessFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    robustnessFeatures.robustBufferAccess2 = VK_TRUE;
    robustnessFeatures.nullDescriptor = VK_TRUE;

    VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR barycentricFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR};
    VkPhysicalDeviceMeshShaderFeaturesEXT meshFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
    VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan11Features v11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &v11;
    v11.pNext = &v12;
    v12.pNext = &v13;
    v13.pNext = &meshFeatures;
    if (barycentric) meshFeatures.pNext = &barycentricFeatures;
    vkGetPhysicalDeviceFeatures2(physical, &features);
    meshFeatures.primitiveFragmentShadingRateMeshShader = VK_FALSE;
    if (!robust && !title) features.features.robustBufferAccess = VK_FALSE;
    if (title) {
        meshFeatures.taskShader = VK_FALSE;
        meshFeatures.multiviewMeshShader = VK_FALSE;
        meshFeatures.meshShaderQueries = VK_FALSE;
    }
    if (robustness2) {
        robustnessFeatures.pNext = features.pNext;
        features.pNext = &robustnessFeatures;
    }
    std::printf("title=%d robustness2=%d\n", title ? 1 : 0, robustness2 ? 1 : 0);
    std::printf("features meshShader=%u taskShader=%u barycentric=%u subgroupSizeControl=%u\n", meshFeatures.meshShader, meshFeatures.taskShader, barycentric ? barycentricFeatures.fragmentShaderBarycentric : 0u, v13.subgroupSizeControl);

    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    std::uint32_t family = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
            family = i;
            break;
        }
    }
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex = family;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.pNext = &features;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queue;
    deviceInfo.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    deviceInfo.ppEnabledExtensionNames = extensions.data();
    VkDevice device = VK_NULL_HANDLE;
    Check(vkCreateDevice(physical, &deviceInfo, nullptr, &device), "vkCreateDevice");

    const auto module = [&](const std::string& path) {
        const auto words = ReadWords(path);
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = words.size() * 4;
        info.pCode = words.data();
        VkShaderModule result = VK_NULL_HANDLE;
        Check(vkCreateShaderModule(device, &info, nullptr, &result), "vkCreateShaderModule");
        return result;
    };
    if (!computePath.empty()) {
        const VkShaderModule computeModule = module(computePath);
        const VkPushConstantRange computePush{VK_SHADER_STAGE_COMPUTE_BIT, 0, 128};
        VkPipelineLayoutCreateInfo computeLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        computeLayoutInfo.pushConstantRangeCount = 1;
        computeLayoutInfo.pPushConstantRanges = &computePush;
        VkPipelineLayout computeLayout = VK_NULL_HANDLE;
        Check(vkCreatePipelineLayout(device, &computeLayoutInfo, nullptr, &computeLayout), "vkCreatePipelineLayout compute");
        VkComputePipelineCreateInfo computeInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        computeInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        computeInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        computeInfo.stage.module = computeModule;
        computeInfo.stage.pName = "main";
        computeInfo.layout = computeLayout;
        std::printf("creating compute pipeline: %s\n", computePath.c_str());
        std::fflush(stdout);
        VkPipeline computePipeline = VK_NULL_HANDLE;
        const VkResult computeResult = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &computeInfo, nullptr, &computePipeline);
        std::printf("RESULT %s vkCreateComputePipelines = %d\n", computeResult == VK_SUCCESS ? "ok" : "error", static_cast<int>(computeResult));
        std::fflush(stdout);
        return computeResult == VK_SUCCESS ? 0 : 1;
    }
    const VkShaderModule meshModule = module(meshPath);
    const VkShaderModule fragModule = fragPath.empty() ? VK_NULL_HANDLE : module(fragPath);

    const VkShaderStageFlags all = VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_FRAGMENT_BIT;
    const VkDescriptorSetLayoutBinding set0[]{{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2, all, nullptr}, {61, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, all, nullptr}};
    const VkDescriptorSetLayoutBinding set1[]{{68, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, all, nullptr}, {122, VK_DESCRIPTOR_TYPE_SAMPLER, 2, all, nullptr}, {127, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, all, nullptr}};
    const VkDescriptorSetLayoutBinding titleSet[]{{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2, VK_SHADER_STAGE_MESH_BIT_EXT, nullptr}, {61, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_MESH_BIT_EXT, nullptr}, {68, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}, {122, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    VkDescriptorSetLayout setLayouts[2]{};
    VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    setInfo.bindingCount = title ? 4 : 2;
    setInfo.pBindings = title ? titleSet : set0;
    Check(vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &setLayouts[0]), "vkCreateDescriptorSetLayout 0");
    setInfo.bindingCount = 3;
    setInfo.pBindings = set1;
    Check(vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &setLayouts[1]), "vkCreateDescriptorSetLayout 1");
    const VkPushConstantRange push{all, 0, 128};
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = title ? 1 : 2;
    layoutInfo.pSetLayouts = setLayouts;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &push;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    Check(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout), "vkCreatePipelineLayout");

    const VkPipelineShaderStageRequiredSubgroupSizeCreateInfo required{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, nullptr, 32u};
    std::vector<VkPipelineShaderStageCreateInfo> stages(1, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO});
    stages[0].stage = VK_SHADER_STAGE_MESH_BIT_EXT;
    stages[0].module = meshModule;
    stages[0].pName = "main";
    if (subgroup32) stages[0].pNext = &required;
    if (fragModule != VK_NULL_HANDLE) {
        VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stage.module = fragModule;
        stage.pName = "main";
        stages.push_back(stage);
    }

    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    raster.rasterizerDiscardEnable = fragModule == VK_NULL_HANDLE ? VK_TRUE : VK_FALSE;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depthStencil{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depthStencil.depthTestEnable = depth ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable = depth ? VK_TRUE : VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = fragModule == VK_NULL_HANDLE ? 0u : 1u;
    blend.pAttachments = &attachment;
    const VkDynamicState dynamics[]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamics;
    const VkFormat color = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = fragModule == VK_NULL_HANDLE ? 0u : 1u;
    rendering.pColorAttachmentFormats = &color;
    rendering.depthAttachmentFormat = depth ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_UNDEFINED;

    VkRenderPass renderPass = VK_NULL_HANDLE;
    if (title && fragModule != VK_NULL_HANDLE) {
        VkAttachmentDescription description{};
        description.format = VK_FORMAT_B8G8R8A8_UNORM;
        description.samples = VK_SAMPLE_COUNT_1_BIT;
        description.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        description.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        description.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        description.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        description.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
        description.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
        const VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_GENERAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &reference;
        VkRenderPassCreateInfo passInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        passInfo.attachmentCount = 1;
        passInfo.pAttachments = &description;
        passInfo.subpassCount = 1;
        passInfo.pSubpasses = &subpass;
        Check(vkCreateRenderPass(device, &passInfo, nullptr, &renderPass), "vkCreateRenderPass");
    }
    VkGraphicsPipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    if (renderPass == VK_NULL_HANDLE) pipelineInfo.pNext = &rendering;
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.stageCount = static_cast<std::uint32_t>(stages.size());
    pipelineInfo.pStages = stages.data();
    pipelineInfo.pViewportState = &viewport;
    pipelineInfo.pRasterizationState = &raster;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &blend;
    pipelineInfo.pDynamicState = &dynamic;
    pipelineInfo.layout = layout;
    std::printf("creating pipeline: mesh=%s frag=%s subgroup32=%d depth=%d\n", meshPath.c_str(), fragPath.empty() ? "(none, rasterizer discard)" : fragPath.c_str(), subgroup32 ? 1 : 0, depth ? 1 : 0);
    std::printf("robustBufferAccess=%d\n", robust ? 1 : 0);
    std::fflush(stdout);
    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);
    std::printf("RESULT %s vkCreateGraphicsPipelines = %d\n", result == VK_SUCCESS ? "ok" : "error", static_cast<int>(result));
    std::fflush(stdout);
    if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyPipelineLayout(device, layout, nullptr);
    vkDestroyDescriptorSetLayout(device, setLayouts[0], nullptr);
    vkDestroyDescriptorSetLayout(device, setLayouts[1], nullptr);
    vkDestroyShaderModule(device, meshModule, nullptr);
    if (fragModule != VK_NULL_HANDLE) vkDestroyShaderModule(device, fragModule, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    return result == VK_SUCCESS ? 0 : 1;
}
