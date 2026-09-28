/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/Format.h>
#include <AK/Vector.h>
#include <LibGfx/VulkanContext.h>
#include <cstdlib>
#include <cstring>

namespace Gfx {

static bool photon_verbose_logging_enabled()
{
    auto* value = std::getenv("PHOTON_VERBOSE");
    return value && value[0] != '\0' && value[0] != '0';
}

#if VULKAN_VALIDATION_LAYERS_DEBUG
static void setup_vulkan_validation_layers_callback(VkInstanceCreateInfo& create_info, VkDebugUtilsMessengerCreateInfoEXT& debug_messenger_create_info);
static void enable_vulkan_validation_layers_callback(VkInstance const& instance, VkDebugUtilsMessengerCreateInfoEXT const& debug_messenger_create_info);
#endif

static ErrorOr<VkInstance> create_instance(uint32_t api_version)
{
    VkInstance instance;

    VkApplicationInfo app_info {};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "Ladybird";
    app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.pEngineName = nullptr;
    app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.apiVersion = api_version;

    VkInstanceCreateInfo create_info {};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &app_info;

#if VULKAN_VALIDATION_LAYERS_DEBUG
    VkDebugUtilsMessengerCreateInfoEXT debug_messenger_create_info {};
    setup_vulkan_validation_layers_callback(create_info, debug_messenger_create_info);
#endif

    auto result = vkCreateInstance(&create_info, nullptr, &instance);
    if (result != VK_SUCCESS) {
        dbgln("vkCreateInstance returned {}", to_underlying(result));
        return Error::from_string_literal("Application instance creation failed");
    }

#if VULKAN_VALIDATION_LAYERS_DEBUG
    enable_vulkan_validation_layers_callback(instance, debug_messenger_create_info);
#endif

    return instance;
}

#ifdef USE_VULKAN_DMABUF_IMAGES
static bool supports_photon_shared_image_extensions(VkPhysicalDevice device)
{
    static constexpr Array<char const*, 4> required_extensions = {
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
        VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    };
    uint32_t extension_count = 0;
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &extension_count, nullptr) != VK_SUCCESS)
        return false;
    Vector<VkExtensionProperties> available_extensions;
    available_extensions.resize(extension_count);
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &extension_count, available_extensions.data()) != VK_SUCCESS)
        return false;
    for (auto* required_extension : required_extensions) {
        if (!available_extensions.contains([&](auto const& extension) {
                return std::strcmp(extension.extensionName, required_extension) == 0;
            }))
            return false;
    }
    return true;
}
#endif

static ErrorOr<VkPhysicalDevice> pick_physical_device(VkInstance instance)
{
    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(instance, &device_count, nullptr);

    if (device_count == 0)
        return Error::from_string_literal("Can't find any physical devices available");

    Vector<VkPhysicalDevice> devices;
    devices.resize(device_count);
    vkEnumeratePhysicalDevices(instance, &device_count, devices.data());

    VkPhysicalDevice picked_discrete_device = VK_NULL_HANDLE;
    VkPhysicalDevice first_device = VK_NULL_HANDLE;
    // Prefer a discrete adapter that supports the enabled external-memory
    // extensions. The frontend logs its own device for comparison.
    for (auto const& device : devices) {
#ifdef USE_VULKAN_DMABUF_IMAGES
        if (!supports_photon_shared_image_extensions(device))
            continue;
#endif
        if (first_device == VK_NULL_HANDLE)
            first_device = device;

        VkPhysicalDeviceProperties device_properties;
        vkGetPhysicalDeviceProperties(device, &device_properties);
        if (device_properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            picked_discrete_device = device;
            break;
        }
    }

    if (picked_discrete_device != VK_NULL_HANDLE)
        return picked_discrete_device;
    if (first_device != VK_NULL_HANDLE)
        return first_device;

    return Error::from_string_literal("No usable Vulkan physical device found");
}

static ErrorOr<VkDevice> create_logical_device(VkPhysicalDevice physical_device, uint32_t* graphics_queue_family)
{
    VkDevice device;

    uint32_t queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, nullptr);
    Vector<VkQueueFamilyProperties> queue_families;
    queue_families.resize(queue_family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, queue_families.data());

    bool graphics_queue_family_found = false;
    uint32_t graphics_queue_family_index = 0;
    for (; graphics_queue_family_index < queue_families.size(); ++graphics_queue_family_index) {
        if (queue_families[graphics_queue_family_index].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            graphics_queue_family_found = true;
            break;
        }
    }

    if (!graphics_queue_family_found) {
        return Error::from_string_literal("Graphics queue family not found");
    }

    *graphics_queue_family = graphics_queue_family_index;

    if (photon_verbose_logging_enabled()) {
        VkPhysicalDeviceProperties properties {};
        vkGetPhysicalDeviceProperties(physical_device, &properties);
        dbgln("Engine Vulkan queue: GPU={} family={} flags={:#x} count={}", StringView { properties.deviceName, std::strlen(properties.deviceName) }, graphics_queue_family_index, queue_families[graphics_queue_family_index].queueFlags, queue_families[graphics_queue_family_index].queueCount);
    }

    VkDeviceQueueCreateInfo queue_create_info {};
    queue_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_create_info.queueFamilyIndex = graphics_queue_family_index;
    queue_create_info.queueCount = 1;

    float const queue_priority = 1.0f;
    queue_create_info.pQueuePriorities = &queue_priority;

    VkPhysicalDeviceFeatures deviceFeatures {};
#ifdef USE_VULKAN_DMABUF_IMAGES
    Array<char const*, 4> device_extensions = {
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
        VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    };
    uint32_t extension_count = 0;
    vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &extension_count, nullptr);
    Vector<VkExtensionProperties> available_extensions;
    available_extensions.resize(extension_count);
    vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &extension_count, available_extensions.data());
    for (auto* required_extension : device_extensions) {
        bool const available = available_extensions.contains([&](auto const& extension) {
            return std::strcmp(extension.extensionName, required_extension) == 0;
        });
        if (photon_verbose_logging_enabled())
            dbgln("Engine Vulkan extension {}: {}", required_extension, available ? "available" : "missing");
        if (!available)
            return Error::from_string_literal("required Vulkan external-memory extension unavailable");
    }
#else
    Array<char const*, 0> device_extensions;
#endif
    VkDeviceCreateInfo create_device_info {};
    create_device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create_device_info.pQueueCreateInfos = &queue_create_info;
    create_device_info.queueCreateInfoCount = 1;
    create_device_info.pEnabledFeatures = &deviceFeatures;
    create_device_info.enabledExtensionCount = device_extensions.size();
    create_device_info.ppEnabledExtensionNames = device_extensions.data();

    if (vkCreateDevice(physical_device, &create_device_info, nullptr, &device) != VK_SUCCESS) {
        return Error::from_string_literal("Logical device creation failed");
    }

    return device;
}

#ifdef USE_VULKAN_DMABUF_IMAGES
static ErrorOr<VkCommandPool> create_command_pool(VkDevice logical_device, uint32_t queue_family_index)
{
    VkCommandPoolCreateInfo command_pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = queue_family_index,
    };
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkResult result = vkCreateCommandPool(logical_device, &command_pool_info, nullptr, &command_pool);
    if (result != VK_SUCCESS) {
        dbgln("vkCreateCommandPool returned {}", to_underlying(result));
        return Error::from_string_literal("command pool creation failed");
    }
    return command_pool;
}

static ErrorOr<VkCommandBuffer> allocate_command_buffer(VkDevice logical_device, VkCommandPool commandPool)
{
    VkCommandBufferAllocateInfo command_buffer_alloc_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .pNext = nullptr,
        .commandPool = commandPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkResult result = vkAllocateCommandBuffers(logical_device, &command_buffer_alloc_info, &command_buffer);
    if (result != VK_SUCCESS) {
        dbgln("vkAllocateCommandBuffers returned {}", to_underlying(result));
        return Error::from_string_literal("command buffer allocation failed");
    }
    return command_buffer;
}
#endif

ErrorOr<VulkanContext> create_vulkan_context()
{
    uint32_t const api_version = VK_API_VERSION_1_1; // v1.1 needed for vkGetPhysicalDeviceFormatProperties2
    auto* instance = TRY(create_instance(api_version));
    auto* physical_device = TRY(pick_physical_device(instance));

    if (photon_verbose_logging_enabled()) {
        VkPhysicalDeviceDriverProperties driver_properties {};
        driver_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
        VkPhysicalDeviceIDProperties id_properties {};
        id_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
        id_properties.pNext = &driver_properties;
        VkPhysicalDeviceProperties2 properties {};
        properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        properties.pNext = &id_properties;
        vkGetPhysicalDeviceProperties2(physical_device, &properties);
        auto const& device = properties.properties;
        auto const& uuid = id_properties.deviceUUID;
        dbgln("Engine painting: Vulkan; GPU: {} vendor={:#06x} device={:#06x} driverVersion={} driver={} ({}) UUID={:02x}{:02x}{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
            StringView { device.deviceName, std::strlen(device.deviceName) }, device.vendorID, device.deviceID, device.driverVersion,
            StringView { driver_properties.driverName, std::strlen(driver_properties.driverName) }, StringView { driver_properties.driverInfo, std::strlen(driver_properties.driverInfo) },
            uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6], uuid[7],
            uuid[8], uuid[9], uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
    }

    uint32_t graphics_queue_family = 0;
    auto* logical_device = TRY(create_logical_device(physical_device, &graphics_queue_family));
    VkQueue graphics_queue;
    vkGetDeviceQueue(logical_device, graphics_queue_family, 0, &graphics_queue);

#ifdef USE_VULKAN_DMABUF_IMAGES
    VkCommandPool command_pool = TRY(create_command_pool(logical_device, graphics_queue_family));
    VkCommandBuffer command_buffer = TRY(allocate_command_buffer(logical_device, command_pool));

    auto pfn_vk_get_memory_fd_khr = reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(logical_device, "vkGetMemoryFdKHR"));
    if (pfn_vk_get_memory_fd_khr == nullptr) {
        return Error::from_string_literal("vkGetMemoryFdKHR unavailable");
    }
    auto pfn_vk_get_image_drm_format_modifier_properties_khr = reinterpret_cast<PFN_vkGetImageDrmFormatModifierPropertiesEXT>(vkGetDeviceProcAddr(logical_device, "vkGetImageDrmFormatModifierPropertiesEXT"));
    if (pfn_vk_get_image_drm_format_modifier_properties_khr == nullptr) {
        return Error::from_string_literal("vkGetImageDrmFormatModifierPropertiesEXT unavailable");
    }
#endif

    return VulkanContext {
        .api_version = api_version,
        .instance = instance,
        .physical_device = physical_device,
        .logical_device = logical_device,
        .graphics_queue = graphics_queue,
        .graphics_queue_family = graphics_queue_family,
#ifdef USE_VULKAN_DMABUF_IMAGES
        .command_pool = command_pool,
        .command_buffer = command_buffer,
        .ext_procs = {
            .get_memory_fd = pfn_vk_get_memory_fd_khr,
            .get_image_drm_format_modifier_properties = pfn_vk_get_image_drm_format_modifier_properties_khr,
        },
#endif
    };
}

#if VULKAN_VALIDATION_LAYERS_DEBUG
static bool check_layer_support(StringView layer_name)
{
    uint32_t layer_count = 0;
    vkEnumerateInstanceLayerProperties(&layer_count, nullptr);

    Vector<VkLayerProperties> layers;
    layers.resize(layer_count);
    vkEnumerateInstanceLayerProperties(&layer_count, layers.data());

    return layers.contains([&layer_name](auto const& layer_properties) {
        return layer_properties.layerName == layer_name;
    });
}

static constexpr Array<char const*, 1> vvl_layers = { "VK_LAYER_KHRONOS_validation" };
static constexpr Array<char const*, 1> vvl_extensions = { VK_EXT_DEBUG_UTILS_EXTENSION_NAME };

static void setup_vulkan_validation_layers_callback(VkInstanceCreateInfo& create_info, VkDebugUtilsMessengerCreateInfoEXT& debug_messenger_create_info)
{
    if (!check_layer_support("VK_LAYER_KHRONOS_validation"sv))
        return;

    debug_messenger_create_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    debug_messenger_create_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debug_messenger_create_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debug_messenger_create_info.pfnUserCallback = [](VkDebugUtilsMessageSeverityFlagBitsEXT,
                                                      VkDebugUtilsMessageTypeFlagsEXT,
                                                      VkDebugUtilsMessengerCallbackDataEXT const* pCallbackData,
                                                      void*) {
        dbgln("Vulkan validation layers: {}", pCallbackData->pMessage);
        dump_backtrace(3);
        return VK_FALSE;
    };

    create_info.enabledLayerCount = vvl_layers.size();
    create_info.ppEnabledLayerNames = vvl_layers.data();
    create_info.enabledExtensionCount = vvl_extensions.size();
    create_info.ppEnabledExtensionNames = vvl_extensions.data();
    create_info.pNext = &debug_messenger_create_info;
}

static void enable_vulkan_validation_layers_callback(VkInstance const& instance, VkDebugUtilsMessengerCreateInfoEXT const& debug_messenger_create_info)
{
    // Vulkan validation layers not available if struct not setup properly
    if (debug_messenger_create_info.sType != VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT) {
        dbgln("Vulkan validation layers: not available");
        return;
    }

    auto pfn_create_debug_messenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
    VkDebugUtilsMessengerEXT debug_messenger { VK_NULL_HANDLE };
    auto result = pfn_create_debug_messenger(instance, &debug_messenger_create_info, nullptr, &debug_messenger);
    if (result != VK_SUCCESS)
        dbgln("vkCreateDebugUtilsMessengerEXT returned {}", to_underlying(result));
    else
        dbgln("Vulkan validation layers: active");
}
#endif

}
