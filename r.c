#include "r.h"
#include "dstuff/file/path.h"
//#include "r_vk.h"
#include "dstuff/containers/stack_list.h"
#include "dstuff/containers/list.h"
#include <stdlib.h>
#include <string.h>
#include "stb/stb_image.h"

struct r_renderer_t r_renderer;



VkPhysicalDevice r_physical_device;
VkInstance r_instance;
VkDevice r_device;
VkSurfaceKHR r_surface;
//VkSwapchainKHR r_swapchain;
struct r_swapchain_handle_t r_swapchain;
struct r_image_handle_t *r_swapchain_images;

uint32_t r_graphics_queue_family_index = 0xffffffff;
uint32_t r_present_queue_family_index = 0xffffffff;
VkQueue r_graphics_queue;
VkQueue r_present_queue;
VkCommandPool r_command_pool;
VkCommandBuffer r_draw_command_buffer;
VkCommandBuffer r_transfer_command_buffer;
VkFence r_draw_fence;
VkFence r_transfer_fence;

uint32_t r_supports_push_descriptors = 0;
PFN_vkCmdPushDescriptorSetKHR vkCmdPushDescriptorSet = NULL;
VkDeviceMemory r_texture_copy_memory;

struct r_render_pass_handle_t r_render_pass;
struct r_framebuffer_handle_t r_framebuffer;


/* resources may be created concurrently */
SDL_SpinLock r_resource_spinlock;
struct stack_list_t r_images;
struct stack_list_t r_image_descriptions;
struct stack_list_t r_textures;
struct stack_list_t r_framebuffers;
struct stack_list_t r_render_passes;
struct stack_list_t r_samplers;
struct stack_list_t r_shaders;
struct stack_list_t r_swapchains;

void r_InitRenderer()
{
    struct r_alloc_t free_alloc;
    SDL_Thread *renderer_thread;
    FILE *file;
    struct r_shader_description_t vertex_description = {};
    struct r_shader_description_t fragment_description = {};
    struct r_render_pass_description_t render_pass_description = {};

    r_renderer.allocd_blocks[0] = create_stack_list(sizeof(struct r_alloc_t), 512);
    r_renderer.allocd_blocks[1] = create_stack_list(sizeof(struct r_alloc_t), 512);
    r_renderer.free_blocks[0] = create_list(sizeof(struct r_alloc_t), 512);
    r_renderer.free_blocks[1] = create_list(sizeof(struct r_alloc_t), 512);
    r_renderer.cmd_buffer = create_ringbuffer(sizeof(struct r_cmd_t), 8192);
    r_renderer.cmd_buffer_data = create_ringbuffer(R_CMD_DATA_ELEM_SIZE, 8192 * 10);
    r_renderer.submiting_draw_cmd_buffer = NULL;
    r_renderer.materials = create_stack_list(sizeof(struct r_material_t), 256);
    r_renderer.lights = create_stack_list(sizeof(struct r_light_t), 256);
    free_alloc.start = 0;
    free_alloc.size = R_HEAP_SIZE;
    free_alloc.align = 0;
    add_list_element(&r_renderer.free_blocks[0], &free_alloc);
    add_list_element(&r_renderer.free_blocks[1], &free_alloc);
    mat4_t_identity(&r_renderer.view_matrix);
    mat4_t_identity(&r_renderer.projection_matrix);

    r_textures = create_stack_list(sizeof(struct r_texture_t), 512);
    r_images = create_stack_list(sizeof(struct r_image_t), 512);
    r_image_descriptions = create_stack_list(sizeof(VkImageCreateInfo), 512);
//    r_texture_descriptions = create_stack_list(sizeof(struct r_texture_description_t), 512);
    r_framebuffers = create_stack_list(sizeof(struct r_framebuffer_t), 16);
//    r_framebuffer_descriptions = create_stack_list(sizeof(struct r_framebuffer_descriptions), 16);
    r_samplers = create_stack_list(sizeof(struct r_sampler_t), 16);
    r_shaders = create_stack_list(sizeof(struct r_shader_t), 128);
    r_render_passes = create_stack_list(sizeof(struct r_render_pass_t), 16);
    r_swapchains = create_stack_list(sizeof(struct r_swapchain_t), 4);

    r_renderer.z_far = 1000.0;
    r_renderer.z_near = 0.01;
    r_renderer.fov_y = 0.68;
    r_renderer.width = 800;
    r_renderer.height = 600;
    r_RecomputeViewProjectionMatrix();

    r_renderer.window = SDL_CreateWindow("Vulkan", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, r_renderer.width, r_renderer.height, SDL_WINDOW_VULKAN);
    SDL_GL_SetSwapInterval(1);
    r_InitVulkan();
    r_CreateDefaultTexture();

    file = fopen("shader.vert.spv", "rb");
    read_file(file, &vertex_description.code, &vertex_description.code_size);
    fclose(file);
    vertex_description.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertex_description.vertex_binding_count = 1;
    vertex_description.vertex_bindings = (struct r_vertex_binding_t []){
        [0] = {
            .size = sizeof(struct vertex_t),
            .attribs = (struct r_vertex_attrib_t []){
                [0] = {
                    .format = VK_FORMAT_R32G32B32A32_SFLOAT,
                    .offset = offsetof(struct vertex_t, position),
                },
                [1] = {
                    .format = VK_FORMAT_R32G32B32A32_SFLOAT,
                    .offset = offsetof(struct vertex_t, normal),
                },
                [2] = {
                    .format = VK_FORMAT_R32G32B32A32_SFLOAT,
                    .offset = offsetof(struct vertex_t, tex_coords),
                }
            },
            .attrib_count = 3
        }
    };
    vertex_description.push_constant_count = 1;
    vertex_description.push_constants = (struct r_push_constant_t []){
        [0] = {
            .offset = 0,
            .size = sizeof(mat4_t) * 2
        },
    };

    struct r_shader_t *vertex_shader = r_GetShaderPointer(r_CreateShader(&vertex_description));
    free(vertex_description.code);

    fopen("shader.frag.spv", "rb");
    read_file(file, &fragment_description.code, &fragment_description.code_size);
    fclose(file);
    fragment_description.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragment_description.resource_count = 1;
    fragment_description.resources = (struct r_resource_binding_t []){
        [0] = {
            .descriptor_type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .count = 1,
        }
    };
    struct r_shader_t *fragment_shader = r_GetShaderPointer(r_CreateShader(&fragment_description));
    free(fragment_description.code);

    render_pass_description = (struct r_render_pass_description_t){
        .attachments = (VkAttachmentDescription []){
            {.format = VK_FORMAT_R32G32B32A32_SFLOAT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR},
//            {.format = VK_FORMAT_R32G32B32A32_SFLOAT},
//            {.format = VK_FORMAT_R32G32B32A32_SFLOAT},
            {.format = VK_FORMAT_D32_SFLOAT}
        },
        .attachment_count = 2,
        .subpass_count = 1,
        .subpasses = (struct r_subpass_description_t[]){
            {
                .color_attachment_count = 1,
                .color_attachments = (VkAttachmentReference []){
                    {.attachment = 0}
                },
                .depth_stencil_attachment = &(VkAttachmentReference){
                    .attachment = 1
                },
                .pipeline_description = &(struct r_pipeline_description_t){
                    .shader_count = 2,
                    .shaders = (struct r_shader_t *[]){
                        vertex_shader, fragment_shader
                    },
                    .vertex_input_state = &(VkPipelineVertexInputStateCreateInfo){
                        .vertexBindingDescriptionCount = 1,
                        .pVertexBindingDescriptions = (VkVertexInputBindingDescription []){
                            {.stride = sizeof(struct vertex_t)}
                        },
                        .vertexAttributeDescriptionCount = 3,
                        .pVertexAttributeDescriptions = (VkVertexInputAttributeDescription []){
                            {
                                .location = 0,
                                .format = VK_FORMAT_R32G32B32A32_SFLOAT,
                                .offset = offsetof(struct vertex_t, position)
                            },
                            {
                                .location = 1,
                                .format = VK_FORMAT_R32G32B32A32_SFLOAT,
                                .offset = offsetof(struct vertex_t, normal)
                            },
                            {
                                .location = 2,
                                .format = VK_FORMAT_R32G32B32A32_SFLOAT,
                                .offset = offsetof(struct vertex_t, tex_coords)
                            }
                        }
                    }
                },
            }
        },
    };

    r_render_pass = r_CreateRenderPass(&render_pass_description);

    struct r_framebuffer_description_t framebuffer_description = (struct r_framebuffer_description_t){
        .attachment_count = render_pass_description.attachment_count,
        .attachments = render_pass_description.attachments,
        .frame_count = 2,
        .width = 800,
        .height = 600,
        .render_pass = r_GetRenderPassPointer(r_render_pass)
    };

    r_framebuffer = r_CreateFramebuffer(&framebuffer_description);

    while(1)
    {
        SDL_PollEvent(NULL);

        VkCommandBufferBeginInfo command_buffer_begin_info = {};
        command_buffer_begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        command_buffer_begin_info.pNext = NULL;
        command_buffer_begin_info.flags = 0;
        command_buffer_begin_info.pInheritanceInfo = NULL;

        struct r_render_pass_t *render_pass = r_GetRenderPassPointer(r_render_pass);
        struct r_framebuffer_t *framebuffer = r_GetFramebufferPointer(r_framebuffer);
        struct r_texture_t *framebuffer_texture = r_GetTexturePointer(framebuffer->textures[0]);
        struct r_texture_t *default_texture = r_GetDefaultTexturePointer();
        struct r_swapchain_t *swapchain = r_GetSwapchainPointer(r_swapchain);

        VkRenderPassBeginInfo render_pass_begin_info = {};
        render_pass_begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        render_pass_begin_info.pNext = NULL;
        render_pass_begin_info.renderPass = render_pass->render_pass;
        render_pass_begin_info.framebuffer = framebuffer->buffers[0];
        render_pass_begin_info.renderArea.offset.x = 0;
        render_pass_begin_info.renderArea.offset.y = 0;
        render_pass_begin_info.renderArea.extent.width = 800;
        render_pass_begin_info.renderArea.extent.height = 600;
        render_pass_begin_info.clearValueCount = 1;
        if(swapchain->current_image)
        {
            render_pass_begin_info.pClearValues = (VkClearValue []){
                {.color.float32 = {0.0, 0.0, 1.0, 1.0}}
            };
        }
        else
        {
            render_pass_begin_info.pClearValues = (VkClearValue []){
                {.color.float32 = {1.0, 0.0, 0.0, 1.0}}
            };
        }


        r_NextImage(r_swapchain);
        vkBeginCommandBuffer(r_transfer_command_buffer, &command_buffer_begin_info);
        vkCmdBeginRenderPass(r_transfer_command_buffer, &render_pass_begin_info, VK_SUBPASS_CONTENTS_INLINE);
//        vkCmdBindPipeline(r_transfer_command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, render_pass->pipeline.pipeline);
        vkCmdEndRenderPass(r_transfer_command_buffer);
        r_CmdBlitImage(default_texture->image, framebuffer_texture->image, NULL);
        r_CmdBlitImage(framebuffer_texture->image, swapchain->images[swapchain->current_image], NULL);
        r_CmdSetImageLayout(swapchain->images[swapchain->current_image], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        vkEndCommandBuffer(r_transfer_command_buffer);

        vkResetFences(r_device, 1, &r_draw_fence);

        VkSubmitInfo submit_info = {};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.pNext = NULL;
        submit_info.waitSemaphoreCount = 0;
        submit_info.pWaitSemaphores = NULL;
        submit_info.pWaitDstStageMask = (VkPipelineStageFlagBits []){VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &r_transfer_command_buffer;
        submit_info.signalSemaphoreCount = 0;
        submit_info.pSignalSemaphores = NULL;
        vkQueueSubmit(r_graphics_queue, 1, &submit_info, r_draw_fence);
        vkWaitForFences(r_device, 1, &r_draw_fence, VK_TRUE, 0xffffffffffffffff);

        VkPresentInfoKHR present_info = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present_info.swapchainCount = 1;
        present_info.pSwapchains = &swapchain->swapchain;
        present_info.pImageIndices = (int []){swapchain->current_image};
        vkQueuePresentKHR(r_graphics_queue, &present_info);

//        SDL_Delay(16);
//        continue;
    }

//    r_LoadTexture("doggo.png", "doggo0");
//    r_LoadTexture("doggo.png", "doggo1");
//    r_LoadTexture("doggo.png", "doggo2");
//    r_LoadTexture("doggo.png", "doggo3");
//    r_LoadTexture("doggo.png", "doggo4");

    renderer_thread = SDL_CreateThread(r_ExecuteCmds, "renderer thread", NULL);
    SDL_DetachThread(renderer_thread);
//    r_InitBuiltinTextures();
}

/*
=================================================================
=================================================================
=================================================================
*/

void r_InitVulkan()
{
    const char **extensions = NULL;
    uint32_t extension_count = 0;
    uint32_t physical_device_count = 1;
    uint32_t queue_family_property_count;
    uint32_t queue_create_info_count;
    uint32_t present_supported;

    VkExtensionProperties *extension_properties;
    vkEnumerateInstanceExtensionProperties(NULL, &extension_count, NULL);
    extension_properties = alloca(sizeof(VkExtensionProperties) * extension_count);
    extensions = alloca(sizeof(char *) * extension_count);
    vkEnumerateInstanceExtensionProperties(NULL, &extension_count, extension_properties);

    for(uint32_t i = 0; i < extension_count; i++)
    {
        extensions[i] = extension_properties[i].extensionName;
    }

    VkInstanceCreateInfo instance_create_info = {};
    instance_create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_create_info.pNext = NULL;
    instance_create_info.flags = 0;
    instance_create_info.pApplicationInfo = NULL;
    instance_create_info.enabledLayerCount = 1;
    instance_create_info.ppEnabledLayerNames = (const char * const []){"VK_LAYER_KHRONOS_validation"};
    instance_create_info.enabledExtensionCount = extension_count;
    instance_create_info.ppEnabledExtensionNames = extensions;
    vkCreateInstance(&instance_create_info, NULL, &r_instance);

    SDL_Vulkan_CreateSurface(r_renderer.window, r_instance, &r_surface);

    vkEnumeratePhysicalDevices(r_instance, &physical_device_count, &r_physical_device);
    VkQueueFamilyProperties *queue_family_properties;
    vkGetPhysicalDeviceQueueFamilyProperties(r_physical_device, &queue_family_property_count, NULL);
    queue_family_properties = alloca(sizeof(VkQueueFamilyProperties) * queue_family_property_count);
    vkGetPhysicalDeviceQueueFamilyProperties(r_physical_device, &queue_family_property_count, queue_family_properties);

    for(uint32_t i = 0; i < queue_family_property_count; i++)
    {
        if(queue_family_properties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
        {
            r_graphics_queue_family_index = i;
            break;
        }
    }

//    for(uint32_t i = 0; i < queue_family_property_count; i++)
//    {
//        /* the spec states that every command that is valid in a transfer
//        capable queue is also valid in a graphics capable queue, which means
//        that a graphics queue is able to perform transfer operations. However,
//        since we'll wait until the command buffer used to transfer data finishes
//        executing,  */
//        if(queue_family_properties[i].queueCount & VK_QUEUE_TRANSFER_BIT)
//        {
//            r_transfer_queue_family_index = i;
//            if(r_transfer_queue_family_index != r_graphics_queue_family_index)
//            {
//                break;
//            }
//        }
//    }

    for(uint32_t i = 0; i < queue_family_property_count; i++)
    {
        vkGetPhysicalDeviceSurfaceSupportKHR(r_physical_device, i, r_surface, &present_supported);
        if(present_supported)
        {
            r_present_queue_family_index = i;
            break;
        }
    }

    VkDeviceQueueCreateInfo *queue_create_info;
    queue_create_info_count = 1 + (r_graphics_queue_family_index != r_present_queue_family_index);
    queue_create_info = alloca(sizeof(VkDeviceQueueCreateInfo) * queue_create_info_count);

    queue_create_info[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_create_info[0].pNext = NULL;
    queue_create_info[0].flags = 0;
    /* all the queues means all the pleasure... */
    queue_create_info[0].queueCount = queue_family_properties[r_graphics_queue_family_index].queueCount;
    queue_create_info[0].queueFamilyIndex = r_graphics_queue_family_index;

    if(queue_create_info_count > 1)
    {
        queue_create_info[1].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_create_info[1].pNext = NULL;
        queue_create_info[1].flags = 0;
        /* a single queue for presenting is enough */
        queue_create_info[1].queueCount = 1;
        queue_create_info[1].queueFamilyIndex = r_present_queue_family_index;
    }

    vkEnumerateDeviceExtensionProperties(r_physical_device, NULL, &extension_count, NULL);
    extensions = alloca(sizeof(char **) * extension_count);
    extension_properties = alloca(sizeof(VkExtensionProperties) * extension_count);
    vkEnumerateDeviceExtensionProperties(r_physical_device, NULL, &extension_count, extension_properties);

    for(uint32_t i = 0; i < extension_count; i++)
    {
        extensions[i] = extension_properties[i].extensionName;
        if(!strcmp(extension_properties[i].extensionName, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME))
        {
            r_supports_push_descriptors = 1;
        }
    }

    VkDeviceCreateInfo device_create_info = {};
    device_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_create_info.pNext = NULL;
    device_create_info.flags = 0;
    device_create_info.queueCreateInfoCount = queue_create_info_count;
    device_create_info.pQueueCreateInfos = queue_create_info;
    device_create_info.enabledExtensionCount = extension_count;
    device_create_info.ppEnabledExtensionNames = extensions;
    device_create_info.enabledLayerCount = 0;
    device_create_info.ppEnabledLayerNames = NULL;
    device_create_info.pEnabledFeatures = NULL;
    vkCreateDevice(r_physical_device, &device_create_info, NULL, &r_device);



//    VkSurfaceFormatKHR format;
//    vkGetPhysicalDeviceSurfaceFormatsKHR(r_physical_device, r_surface, &(int){1}, &format);
//
    VkSwapchainCreateInfoKHR swapchain_create_info = {};
    swapchain_create_info.surface = r_surface;
    swapchain_create_info.imageExtent.width = 800;
    swapchain_create_info.imageExtent.height = 600;
    r_swapchain = r_CreateSwapchain(&swapchain_create_info);
//
//    VkImageCreateInfo image_create_info = {};
//    image_create_info.imageType = VK_IMAGE_TYPE_2D;
//    image_create_info.format = format.format;
//    image_create_info.extent.width = 800;
//    image_create_info.extent.height = 600;
//    image_create_info.extent.depth = 1;
//    image_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
//    uint32_t image_count = 0;
//    VkResult result;
//
//    result = vkGetSwapchainImagesKHR(r_device, r_swapchain, &image_count, NULL);
//    r_swapchain_images = calloc(sizeof(struct r_image_handle_t), image_count);
//    VkImage *images = alloca(sizeof(VkImage) * image_count);
//    result = vkGetSwapchainImagesKHR(r_device, r_swapchain, &image_count, images);
//
//    for(uint32_t image_index = 0; image_index < image_count; image_index++)
//    {
//        r_swapchain_images[image_index] = r_AllocImage(&image_create_info);
//        struct r_image_t *image = r_GetImagePointer(r_swapchain_images[image_index]);
//        vkDestroyImage(r_device, image->image, NULL);
//        image->image = images[image_index];
//    }

    if(r_supports_push_descriptors)
    {
        vkCmdPushDescriptorSet = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(r_device, "vkCmdPushDescriptorSetKHR");
    }

    vkGetDeviceQueue(r_device, r_graphics_queue_family_index, 0, &r_graphics_queue);
    vkGetDeviceQueue(r_device, r_present_queue_family_index, 0, &r_present_queue);

    VkFenceCreateInfo fence_create_info = {};
    fence_create_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_create_info.pNext = NULL;
    fence_create_info.flags = 0;
    vkCreateFence(r_device, &fence_create_info, NULL, &r_transfer_fence);
    vkCreateFence(r_device, &fence_create_info, NULL, &r_draw_fence);

    /*
    =================================================================
    =================================================================
    =================================================================
    */

    VkCommandPoolCreateInfo command_pool_create_info = {};
    command_pool_create_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    command_pool_create_info.pNext = NULL;
    command_pool_create_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    command_pool_create_info.queueFamilyIndex = r_graphics_queue_family_index;
    vkCreateCommandPool(r_device, &command_pool_create_info, NULL, &r_command_pool);

    /* for now a single command buffer will suffice */
    VkCommandBufferAllocateInfo allocate_info = {};
    allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate_info.pNext = NULL;
    allocate_info.commandPool = r_command_pool;
    allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate_info.commandBufferCount = 1;
    vkAllocateCommandBuffers(r_device, &allocate_info, &r_draw_command_buffer);
    vkAllocateCommandBuffers(r_device, &allocate_info, &r_transfer_command_buffer);

    /*
    =================================================================
    =================================================================
    =================================================================
    */

    /* create an image to allocate the image copy memory */
    VkImageCreateInfo copy_image_create_info = {};
    copy_image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    copy_image_create_info.pNext = NULL;
    copy_image_create_info.flags = 0;
    copy_image_create_info.imageType = VK_IMAGE_TYPE_2D;
    copy_image_create_info.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    copy_image_create_info.extent.width = 2048;
    copy_image_create_info.extent.height = 2048;
    copy_image_create_info.extent.depth = 1;
    copy_image_create_info.mipLevels = 1;
    copy_image_create_info.arrayLayers = 1;
    copy_image_create_info.samples = VK_SAMPLE_COUNT_1_BIT;
    copy_image_create_info.tiling = VK_IMAGE_TILING_LINEAR;
    copy_image_create_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    copy_image_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    copy_image_create_info.queueFamilyIndexCount = 0;
    copy_image_create_info.pQueueFamilyIndices = NULL;
    copy_image_create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage copy_image;
    vkCreateImage(r_device, &copy_image_create_info, NULL, &copy_image);

    VkMemoryRequirements memory_requirements;
    vkGetImageMemoryRequirements(r_device, copy_image, &memory_requirements);

    VkMemoryAllocateInfo texture_memory_allocate_info = {};
    texture_memory_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    texture_memory_allocate_info.pNext = NULL;
    texture_memory_allocate_info.allocationSize = memory_requirements.size;
    texture_memory_allocate_info.memoryTypeIndex = r_MemoryIndexWithProperties(memory_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                                                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(r_device, &texture_memory_allocate_info, NULL, &r_texture_copy_memory);
    /* don't need this anymore */
    vkDestroyImage(r_device, copy_image, NULL);
}

/*
=================================================================
=================================================================
=================================================================
*/

uint32_t r_MemoryIndexWithProperties(uint32_t type_bits, uint32_t properties)
{
    VkPhysicalDeviceMemoryProperties memory_properties;
    vkGetPhysicalDeviceMemoryProperties(r_physical_device, &memory_properties);

    for(uint32_t i = 0; i < memory_properties.memoryTypeCount; i++)
    {
        if(type_bits & 1)
        {
            if((memory_properties.memoryTypes[i].propertyFlags & properties) == properties)
            {
                return i;
            }
        }

        type_bits >>= 1;
    }

    return 0xffffffff;
}

/*
=================================================================
=================================================================
=================================================================
*/

struct r_image_handle_t r_CreateImage(VkImageCreateInfo *description)
{
    struct r_image_handle_t handle;
    handle = r_AllocImage(description);
    r_AllocImageMemory(handle);
    return handle;
}

struct r_image_handle_t r_AllocImage(VkImageCreateInfo *description)
{
    struct r_image_handle_t handle = R_INVALID_IMAGE_HANDLE;
    struct r_image_t *image;
    VkImageCreateInfo *description_copy;
    VkMemoryRequirements memory_requirements;
    VkMemoryAllocateInfo allocate_info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};

    handle.index = add_stack_list_element(&r_images, NULL);
    add_stack_list_element(&r_image_descriptions, description);
    image = get_stack_list_element(&r_images, handle.index);
    description = get_stack_list_element(&r_image_descriptions, handle.index);
    description->sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    memset(image, 0, sizeof(struct r_image_t));

//    if(!description->extent.width)
//    {
//        description->extent.width = 1;
//    }
//
//    if(!description->extent.height)
//    {
//        description->imageType = VK_IMAGE_TYPE_1D;
//    }
//    else if(!description->extent.depth)
//    {
//        description->imageType = VK_IMAGE_TYPE_2D;
//    }

    if(!description->mipLevels)
    {
        description->mipLevels = 1;
    }

    if(!description->arrayLayers)
    {
        description->arrayLayers = 1;
    }

    if(!description->samples)
    {
        description->samples = VK_SAMPLE_COUNT_1_BIT;
    }

    if(!description->usage)
    {
        description->usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if(r_IsDepthStencilFormat(description->format))
        {
            description->usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        }
        else
        {
            description->usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        }
    }

    description->initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    image->aspect_mask = r_GetFormatAspectFlags(description->format);
    vkCreateImage(r_device, description, NULL, &image->image);
    return handle;
}

void r_AllocImageMemory(struct r_image_handle_t handle)
{
    struct r_image_t *image;
    VkMemoryRequirements memory_requirements;
    VkMemoryAllocateInfo allocate_info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};

    image = r_GetImagePointer(handle);
    vkGetImageMemoryRequirements(r_device, image->image, &memory_requirements);

    allocate_info.memoryTypeIndex = r_MemoryIndexWithProperties(memory_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    allocate_info.allocationSize = memory_requirements.size;
    /* TODO: this is not the ideal way of doing things. The most efficient
    way would be to preallocate a big chunk of memory and then share
    it amongst all VkImage. Same thing for VkBuffer. */
    vkAllocateMemory(r_device, &allocate_info, NULL, &image->memory);
    vkBindImageMemory(r_device, image->image, image->memory, (VkDeviceSize){0});
}

struct r_image_handle_t r_CreateImageFrom(struct r_image_t *image)
{
    struct r_image_handle_t handle = R_INVALID_IMAGE_HANDLE;
    handle.index = add_stack_list_element(&r_images, image);
    return handle;
}

void r_DestroyImage(struct r_image_handle_t handle)
{
    struct r_image_t *image;
    image = r_GetImagePointer(handle);
    if(image)
    {
        vkDestroyImage(r_device, image->image, NULL);
        if(image->memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(r_device, image->memory, NULL);
        }
        remove_stack_list_element(&r_images, handle.index);
        remove_stack_list_element(&r_image_descriptions, handle.index);
    }
}

struct r_image_t *r_GetImagePointer(struct r_image_handle_t handle)
{
    struct r_image_t *image = NULL;
    image = get_stack_list_element(&r_images, handle.index);
    return image;
}

VkImageCreateInfo *r_GetImageDescriptionPointer(struct r_image_handle_t handle)
{
    VkImageCreateInfo *description = NULL;
    description = get_stack_list_element(&r_image_descriptions, handle.index);
    return description;
}

void r_BlitImage(struct r_image_handle_t src_handle, struct r_image_handle_t dst_handle, VkImageBlit *blit)
{
    VkCommandBufferBeginInfo command_buffer_begin_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VkSubmitInfo submit_info = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};

    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &r_transfer_command_buffer;

    vkResetCommandBuffer(r_transfer_command_buffer, 0);
    vkBeginCommandBuffer(r_transfer_command_buffer, &command_buffer_begin_info);
    r_CmdBlitImage(src_handle, dst_handle, blit);
    vkEndCommandBuffer(r_transfer_command_buffer);

    vkResetFences(r_device, 1, &r_transfer_fence);
    vkQueueSubmit(r_graphics_queue, 1, &submit_info, r_transfer_fence);
    vkWaitForFences(r_device, 1, &r_transfer_fence, VK_TRUE, 0xffffffffffffffff);
}

void r_CmdBlitImage(struct r_image_handle_t src_handle, struct r_image_handle_t dst_handle, VkImageBlit *blit)
{
    struct r_image_t *src_image;
    struct r_image_t *dst_image;
    VkImageCreateInfo *src_description;
    VkImageCreateInfo *dst_description;
    uint32_t old_src_layout;
    uint32_t old_dst_layout;
    VkCommandBufferBeginInfo begin_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};

    src_image = r_GetImagePointer(src_handle);
    src_description = r_GetImageDescriptionPointer(src_handle);
    dst_image = r_GetImagePointer(dst_handle);
    dst_description = r_GetImageDescriptionPointer(dst_handle);

    if(!blit)
    {
        blit = alloca(sizeof(VkImageBlit));

        blit->srcOffsets[0].x = 0;
        blit->srcOffsets[0].y = 0;
        blit->srcOffsets[0].z = 0;
        blit->srcOffsets[1].x = src_description->extent.width;
        blit->srcOffsets[1].y = src_description->extent.height;
        blit->srcOffsets[1].z = src_description->extent.depth;
        blit->srcSubresource.aspectMask = src_image->aspect_mask;
        blit->srcSubresource.baseArrayLayer = 0;
        blit->srcSubresource.layerCount = 1;
        blit->srcSubresource.mipLevel = 0;

        blit->dstOffsets[0].x = 0;
        blit->dstOffsets[0].y = 0;
        blit->dstOffsets[0].z = 0;
        blit->dstOffsets[1].x = dst_description->extent.width;
        blit->dstOffsets[1].y = dst_description->extent.height;
        blit->dstOffsets[1].z = dst_description->extent.depth;
        blit->dstSubresource.aspectMask = dst_image->aspect_mask;
        blit->dstSubresource.baseArrayLayer = 0;
        blit->dstSubresource.layerCount = 1;
        blit->dstSubresource.mipLevel = 0;
    }

    old_src_layout = src_image->current_layout;
    old_dst_layout = dst_image->current_layout;

    r_CmdSetImageLayout(src_handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    r_CmdSetImageLayout(dst_handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdBlitImage(r_transfer_command_buffer, src_image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                              dst_image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, blit, VK_FILTER_NEAREST);
    r_CmdSetImageLayout(src_handle, old_src_layout);
    r_CmdSetImageLayout(dst_handle, old_dst_layout);
}

void r_CmdSetImageLayout(struct r_image_handle_t handle, uint32_t new_layout)
{
    VkImageMemoryBarrier memory_barrier = {};
    struct r_image_t *image;
    VkImageCreateInfo *description;

    image = r_GetImagePointer(handle);
    description = r_GetImageDescriptionPointer(handle);

    if(new_layout == VK_IMAGE_LAYOUT_UNDEFINED)
    {
        if(r_IsDepthStencilFormat(description->format))
        {
            new_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        }
        else
        {
            new_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
    }

    memory_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    memory_barrier.pNext = NULL;
    /* all writes to this texture must become visible before the layout can change */
    memory_barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    /* new layout in place, now the memory becomes accessible */
    memory_barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    memory_barrier.oldLayout = image->current_layout;
    memory_barrier.newLayout = new_layout;
    memory_barrier.srcQueueFamilyIndex = r_graphics_queue_family_index;
    memory_barrier.dstQueueFamilyIndex = r_graphics_queue_family_index;
    memory_barrier.image = image->image;
    memory_barrier.subresourceRange.aspectMask = image->aspect_mask;
    memory_barrier.subresourceRange.baseArrayLayer = 0;
    memory_barrier.subresourceRange.baseMipLevel = 0;
    memory_barrier.subresourceRange.layerCount = 1;
    memory_barrier.subresourceRange.levelCount = 1;

    vkCmdPipelineBarrier(r_transfer_command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &memory_barrier);
    image->current_layout = new_layout;
}

void r_LoadImageData(struct r_image_handle_t handle, void *data)
{
    struct r_image_handle_t copy_image_handle;
    struct r_image_t *copy_image;
    VkImageCreateInfo *description;
    VkSubresourceLayout subresource_layout;
    VkImageSubresource subresource;
    uint32_t memory_offset;
    uint32_t data_offset;
    uint32_t data_row_pitch;
    uint32_t data_layer_pitch;
    char *memory;

//    description = r_GetImageDescriptionPointer(handle);
//    vkCreateImage(r_device, description, NULL, &copy_image);
    description = alloca(sizeof(VkImageCreateInfo));
    memcpy(description, r_GetImageDescriptionPointer(handle), sizeof(VkImageCreateInfo));
    description->tiling = VK_IMAGE_TILING_LINEAR;
    copy_image_handle = r_AllocImage(description);
    copy_image = r_GetImagePointer(copy_image_handle);
    vkBindImageMemory(r_device, copy_image->image, r_texture_copy_memory, (VkDeviceSize){0});

    subresource.arrayLayer = 0;
    subresource.mipLevel = 0;
    subresource.aspectMask = r_GetFormatAspectFlags(description->format);
    vkGetImageSubresourceLayout(r_device, copy_image->image, &subresource, &subresource_layout);
    vkMapMemory(r_device, r_texture_copy_memory, (VkDeviceSize){0}, subresource_layout.size, 0, (void **)&memory);

    data_row_pitch = description->extent.width * r_GetFormatPixelPitch(description->format);
    data_layer_pitch = description->extent.height * data_row_pitch;

    for(uint32_t layer_index = 0; layer_index < description->extent.depth; layer_index++)
    {
        for(uint32_t row_index = 0; row_index < description->extent.height; row_index++)
        {
            memory_offset = row_index * subresource_layout.rowPitch + layer_index * subresource_layout.depthPitch;
            data_offset = row_index * data_row_pitch + layer_index * data_layer_pitch;
            memcpy(memory + memory_offset, (char *)data + data_offset, data_row_pitch);
        }
    }

    vkUnmapMemory(r_device, r_texture_copy_memory);
    r_BlitImage(copy_image_handle, handle, NULL);
//    r_DestroyImage(copy_image_handle);
}

VkImageAspectFlagBits r_GetFormatAspectFlags(VkFormat format)
{
    VkImageAspectFlagBits flags;

    switch(format)
    {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_D32_SFLOAT:
            flags = VK_IMAGE_ASPECT_DEPTH_BIT;
        break;

        case VK_FORMAT_D16_UNORM_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            flags = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        break;

        default:
            flags = VK_IMAGE_ASPECT_COLOR_BIT;
        break;
    }

    return flags;
}

uint32_t r_GetFormatPixelPitch(VkFormat format)
{
    switch(format)
    {
        case VK_FORMAT_R8G8B8_UINT:
        case VK_FORMAT_R8G8B8_SINT:
            return 3;

        case VK_FORMAT_R8G8B8A8_UINT:
        case VK_FORMAT_R8G8B8A8_SINT:
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R32_UINT:
        case VK_FORMAT_R32_SINT:
        case VK_FORMAT_R32_SFLOAT:
            return 4;

        case VK_FORMAT_R32G32B32_UINT:
        case VK_FORMAT_R32G32B32_SINT:
        case VK_FORMAT_R32G32B32_SFLOAT:
            return 12;

        case VK_FORMAT_R32G32B32A32_UINT:
        case VK_FORMAT_R32G32B32A32_SINT:
        case VK_FORMAT_R32G32B32A32_SFLOAT:
            return 16;
    }
}


void r_CreateDefaultTexture()
{
    struct r_texture_handle_t default_texture;
    struct r_texture_description_t description = {};;
    struct r_texture_t *texture;
    uint32_t *default_texture_pixels;
    uint32_t pixel_pitch;
    uint32_t row_pitch;

    description.extent.width = 8;
    description.extent.height = 8;
    description.extent.depth = 1;
    description.image_type = VK_IMAGE_TYPE_2D;
    description.name = "default_texture";
    description.format = VK_FORMAT_R8G8B8A8_UNORM;
    description.sampler_params.min_filter = VK_FILTER_NEAREST;
    description.sampler_params.mag_filter = VK_FILTER_NEAREST;
    description.sampler_params.mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    description.sampler_params.addr_mode_u = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    description.sampler_params.addr_mode_v = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    description.sampler_params.addr_mode_w = VK_SAMPLER_ADDRESS_MODE_REPEAT;

    default_texture = r_CreateTexture(&description);
    pixel_pitch = r_GetFormatPixelPitch(description.format);
    row_pitch = pixel_pitch * description.extent.width;
    default_texture_pixels = calloc(pixel_pitch, description.extent.width * description.extent.height);
    texture = r_GetTexturePointer(default_texture);
    for(uint32_t y = 0; y < description.extent.height; y++)
    {
        for(uint32_t x = 0; x < description.extent.width; x++)
        {
            default_texture_pixels[y * description.extent.width + x] = ((x + y) % 2) ? 0xff111111 : 0xff222222;
        }
    }
//    memset(default_texture_pixels, 0xffffffff, sizeof(uint32_t) * description.extent.width * description.extent.height);
    r_LoadImageData(texture->image, default_texture_pixels);
    free(default_texture_pixels);
}

struct r_texture_handle_t r_CreateTexture(struct r_texture_description_t *description)
{
    struct r_texture_handle_t handle = R_INVALID_TEXTURE_HANDLE;
    struct r_texture_t *texture;
    struct r_image_t *image;
    struct r_texture_description_t texture_description;
    VkImageViewCreateInfo image_view_create_info = {};

    if(description)
    {
        handle.index = add_stack_list_element(&r_textures, NULL);
        texture = get_stack_list_element(&r_textures, handle.index);
        memcpy(&texture_description, description, sizeof(struct r_texture_description_t));
        texture->image = r_CreateImage((VkImageCreateInfo *)&texture_description);
        memcpy(&texture_description, r_GetImageDescriptionPointer(texture->image), sizeof(VkImageCreateInfo));
        image = r_GetImagePointer(texture->image);

        image_view_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        image_view_create_info.pNext = NULL;
        image_view_create_info.flags = 0;
        image_view_create_info.image = image->image;

        switch(texture_description.image_type)
        {
            case VK_IMAGE_TYPE_1D:
                if(texture_description.array_layers > 1)
                {
                    image_view_create_info.viewType = VK_IMAGE_VIEW_TYPE_1D_ARRAY;
                }
                else
                {
                    image_view_create_info.viewType = VK_IMAGE_VIEW_TYPE_1D;
                }
            break;

            case VK_IMAGE_TYPE_2D:
                if(texture_description.array_layers > 1)
                {
                    image_view_create_info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
                }
                else
                {
                    image_view_create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
                }
            break;
        }

        image_view_create_info.format = texture_description.format;
        image_view_create_info.components.r = VK_COMPONENT_SWIZZLE_R;
        image_view_create_info.components.g = VK_COMPONENT_SWIZZLE_G;
        image_view_create_info.components.b = VK_COMPONENT_SWIZZLE_B;
        image_view_create_info.components.a = VK_COMPONENT_SWIZZLE_A;

        image_view_create_info.subresourceRange.aspectMask = r_GetFormatAspectFlags(texture_description.format);
        image_view_create_info.subresourceRange.baseArrayLayer = 0;
        image_view_create_info.subresourceRange.baseMipLevel = 0;
        image_view_create_info.subresourceRange.levelCount = texture_description.mip_levels;
        image_view_create_info.subresourceRange.layerCount = texture_description.array_layers;
        vkCreateImageView(r_device, &image_view_create_info, NULL, &texture->image_view);

        texture->sampler = r_TextureSampler(&texture_description.sampler_params);
    }

    return handle;
}

void r_DestroyTexture(struct r_texture_handle_t handle)
{
    struct r_texture_t *texture = r_GetTexturePointer(handle);

    if(texture && handle.index != R_DEFAULT_TEXTURE_INDEX /*&& handle.index != R_MISSING_TEXTURE_INDEX*/ )
    {
        vkDestroyImageView(r_device, texture->image_view, NULL);
        r_DestroyImage(texture->image);
//        SDL_AtomicLock(&r_resource_spinlock);
        remove_stack_list_element(&r_textures, handle.index);
//        SDL_AtomicUnlock(&r_resource_spinlock);
    }
}


VkSampler r_TextureSampler(struct r_sampler_params_t *params)
{
    struct r_sampler_t *sampler = NULL;
    VkSampler vk_sampler = VK_NULL_HANDLE;
    uint32_t sampler_index;

    for(sampler_index = 0; sampler_index < r_samplers.cursor; sampler_index++)
    {
        sampler = get_stack_list_element(&r_samplers, sampler_index);

        if(!memcmp(&sampler->params, params, sizeof(struct r_sampler_params_t)))
        {
            break;
        }
    }

    if(sampler_index >= r_samplers.cursor)
    {
        VkSamplerCreateInfo sampler_create_info = {};
        sampler_create_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler_create_info.pNext = NULL;
        sampler_create_info.flags = 0;
        sampler_create_info.magFilter = params->mag_filter;
        sampler_create_info.minFilter = params->min_filter;
        sampler_create_info.mipmapMode = params->mipmap_mode;
        sampler_create_info.addressModeU = params->addr_mode_u;
        sampler_create_info.addressModeV = params->addr_mode_v;
        sampler_create_info.addressModeW = params->addr_mode_w;
        sampler_create_info.mipLodBias = 0.0;
        sampler_create_info.anisotropyEnable = VK_FALSE;
        sampler_create_info.maxAnisotropy = 2.0;
        sampler_create_info.compareEnable = VK_FALSE;
        sampler_create_info.compareOp = VK_COMPARE_OP_NEVER;
        sampler_create_info.minLod = 0.0;
        sampler_create_info.maxLod = 0.0;
        sampler_create_info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        sampler_create_info.unnormalizedCoordinates = VK_FALSE;

        sampler_index = add_stack_list_element(&r_samplers, NULL);
        sampler = (struct r_sampler_t *)get_stack_list_element(&r_samplers, sampler_index);

        vkCreateSampler(r_device, &sampler_create_info, NULL, &vk_sampler);
        sampler->sampler = vk_sampler;
        memcpy(&sampler->params, params, sizeof(struct r_sampler_params_t));
    }

    return vk_sampler;
}

struct r_texture_handle_t r_LoadTexture(char *file_name, char *texture_name)
{
    unsigned char *pixels;
    int width;
    int height;
    int channels;
    struct r_texture_handle_t handle = R_INVALID_TEXTURE_HANDLE;
    struct r_texture_t *texture;
    struct r_texture_description_t description = {};

    file_name = format_path(file_name);

    pixels = stbi_load(file_name, &width, &height, &channels, STBI_rgb_alpha);

    if(pixels)
    {
        description.extent.width = width;
        description.extent.height = height;
        description.format = VK_FORMAT_R8G8B8A8_UNORM;
        description.sampler_params.addr_mode_u = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        description.sampler_params.addr_mode_v = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        description.sampler_params.addr_mode_w = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        description.sampler_params.mag_filter = VK_FILTER_LINEAR;
        description.sampler_params.min_filter = VK_FILTER_LINEAR;
        description.sampler_params.mipmap_mode = VK_SAMPLER_MIPMAP_MODE_LINEAR;

        handle = r_CreateTexture(&description);
        texture = r_GetTexturePointer(handle);

        if(texture_name)
        {
            texture->name = strdup(texture_name);
        }
        else
        {
            texture->name = strdup(file_name);
        }

        r_LoadImageData(texture->image, pixels);
        printf("texture %s loaded!\n", description.name);
    }

    return handle;
}

struct r_texture_t *r_GetTexturePointer(struct r_texture_handle_t handle)
{
    struct r_texture_description_t *description;
    struct r_texture_t *texture = NULL;
//    description = r_GetTextureDescriptionPointer(handle);

//    if(description)
//    {
    texture = get_stack_list_element(&r_textures, handle.index);
//    }

    return texture;
}

struct r_texture_t* r_GetDefaultTexturePointer()
{
    return r_GetTexturePointer(R_TEXTURE_HANDLE(R_DEFAULT_TEXTURE_INDEX));
}

struct r_texture_handle_t r_GetTextureHandle(char *name)
{
    struct r_texture_handle_t handle = R_INVALID_TEXTURE_HANDLE;
//    struct r_texture_t *texture;
//
//    for(uint32_t i = 0; i < r_renderer.textures.cursor; i++)
//    {
//        texture = r_GetTexturePointer(R_TEXTURE_HANDLE(i));
//
//        if(texture && !strcmp(texture->name, name))
//        {
//            handle = R_TEXTURE_HANDLE(i);
//            break;
//        }
//    }

    return handle;
}

/*
=================================================================
=================================================================
=================================================================
*/

struct r_shader_handle_t r_CreateShader(struct r_shader_description_t *description)
{
    struct r_shader_t *shader;
    struct r_shader_handle_t handle = R_INVALID_SHADER_HANDLE;
    struct r_vertex_attrib_t *attribs;
    struct r_vertex_attrib_t *attrib;
    struct r_vertex_binding_t *binding;
    struct r_resource_binding_t *resource_binding;
    uint32_t attrib_count;
    uint32_t alloc_size;
    VkDescriptorSetLayoutBinding *layout_bindings;
    VkDescriptorSetLayoutBinding *layout_binding;

    if(description)
    {
        handle.index = add_stack_list_element(&r_shaders, NULL);
        shader = (struct r_shader_t *)get_stack_list_element(&r_shaders, handle.index);
        memset(shader, 0, sizeof(struct r_shader_t));
        shader->stage = description->stage;
        shader->push_constant_count = description->push_constant_count;
        shader->vertex_binding_count = description->vertex_binding_count;

        if(description->push_constant_count)
        {
            shader->push_constants = (struct r_push_constant_t *)calloc(description->push_constant_count,
                                                                    sizeof(struct r_push_constant_t));
            memcpy(shader->push_constants, description->push_constants, description->push_constant_count *
                                                                    sizeof(struct r_push_constant_t));
        }

        if(description->vertex_binding_count)
        {
            attrib_count = 0;
            for(uint32_t i = 0; i < description->vertex_binding_count; i++)
            {
                attrib_count += description->vertex_bindings[i].attrib_count;
            }
            alloc_size = attrib_count * sizeof(struct r_vertex_attrib_t);
            alloc_size += description->vertex_binding_count * sizeof(struct r_vertex_binding_t);

            /* single memory block for bindings and attributes */
            shader->vertex_bindings = (struct r_vertex_binding_t *)calloc(1, alloc_size);
            /* attributes exist in memory right after the bindings */
            attribs = (struct r_vertex_attrib_t *)(shader->vertex_bindings + description->vertex_binding_count);
            memcpy(shader->vertex_bindings, description->vertex_bindings, sizeof(struct r_vertex_binding_t) *
                   description->vertex_binding_count);
            attrib_count = 0;
            for(uint32_t i = 0; i < description->vertex_binding_count; i++)
            {
                binding = shader->vertex_bindings + i;
                binding->attribs = attribs + attrib_count;
                attrib_count += binding->attrib_count;

                memcpy(binding->attribs, description->vertex_bindings[i].attribs,
                    sizeof(struct r_vertex_attrib_t) * binding->attrib_count);
            }
        }

        if(description->resource_count)
        {
            layout_bindings = alloca(sizeof(VkDescriptorSetLayoutBinding) * description->resource_count);

            for(uint32_t i = 0; i < description->resource_count; i++)
            {
                layout_binding = layout_bindings + i;
                resource_binding = description->resources + i;

                layout_binding->binding = i;
                layout_binding->descriptorType = resource_binding->descriptor_type;
                layout_binding->descriptorCount = resource_binding->count;
                layout_binding->pImmutableSamplers = NULL;
                layout_binding->stageFlags = description->stage;
            }

            VkDescriptorSetLayoutCreateInfo layout_create_info = {};
            layout_create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            layout_create_info.pNext = NULL;
            layout_create_info.flags = 0;
            layout_create_info.bindingCount = description->resource_count;
            layout_create_info.pBindings = layout_bindings;
            vkCreateDescriptorSetLayout(r_device, &layout_create_info, NULL, &shader->descriptor_set_layout);
        }

        VkShaderModuleCreateInfo module_create_info = {};
        module_create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        module_create_info.pNext = NULL;
        module_create_info.flags = 0;
        module_create_info.codeSize = description->code_size;
        module_create_info.pCode = description->code;
        vkCreateShaderModule(r_device, &module_create_info, NULL, &shader->module);
    }

    return handle;
}

void r_DestroyShader(struct r_shader_handle_t handle)
{
//    struct r_shader_t *shader;
//    shader = r_GetShaderPointer(handle);
//    if(shader)
//    {
//        r_vk_DestroyShader(shader);
//
//        if(shader->description.push_constants)
//        {
//            free(shader->description.push_constants);
//        }
//
//        if(shader->description.resources)
//        {
//            free(shader->description.resources);
//        }
//
//        if(shader->description.vertex_bindings)
//        {
//            free(shader->description.vertex_bindings);
//        }
//
//        shader->description.resource_count = 0xffffffff;
//        shader->description.vertex_binding_count = 0xffffffff;
//        remove_stack_list_element(&r_renderer.shaders, handle.index);
//    }
}

struct r_shader_t *r_GetShaderPointer(struct r_shader_handle_t handle)
{
    struct r_shader_t *shader = NULL;
    shader = get_stack_list_element(&r_shaders, handle.index);
//    if(shader && shader->description.resource_count == 0xffffffff)
//    {
//        shader = NULL;
//    }

    return shader;
}

/*
=================================================================
=================================================================
=================================================================
*/

uint32_t r_IsDepthStencilFormat(VkFormat format)
{
    switch(format)
    {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_D16_UNORM_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            return 1;
    }

    return 0;
}

struct r_render_pass_handle_t r_CreateRenderPass(struct r_render_pass_description_t *description)
{
    struct r_render_pass_handle_t handle = R_INVALID_RENDER_PASS_HANDLE;
    struct r_render_pass_t *render_pass;
    struct r_render_pass_description_t *description_copy;
    struct r_pipeline_description_t pipeline_description;

    struct r_subpass_description_t *subpass_descriptions; /* will contain the subpass descriptions that came
    in, or the one that was created in here in the case one wasn't provided by the caller */

    struct r_subpass_description_t *subpass_description; /* used to iterate over the subpass_descriptions array */

    VkAttachmentReference *color_attachment_references;
    VkAttachmentReference *depth_stencil_attachment_references;
    VkAttachmentReference *attachment_reference; /* used to iterate over an array of attachment references */

    VkAttachmentDescription *attachment_description; /* used to iterate over an array of attachment descriptions */

    VkGraphicsPipelineCreateInfo pipeline_create_info = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    VkRenderPassCreateInfo render_pass_create_info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};

    VkPipelineColorBlendAttachmentState *attachment_state; /* used to iterate over the array of attachment states. */

    VkPipelineVertexInputStateCreateInfo vertex_input_state;
    uint32_t max_vertex_bindings = 0;
    uint32_t max_vertex_attributes = 0;
    uint32_t color_attachment_count = 0;
    VkVertexInputBindingDescription *vertex_bindings;
    VkVertexInputAttributeDescription *vertex_attributes;

    /* these are just used to fill any of the attributes that might be missing
    in the pipeline description that we got.   */
    VkPipelineInputAssemblyStateCreateInfo input_assembly_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .primitiveRestartEnable = VK_FALSE
    };

    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };

    VkPipelineRasterizationStateCreateInfo rasterization_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_BACK_BIT,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .depthBiasEnable = VK_FALSE,
        .lineWidth = 1.0
    };

    VkPipelineMultisampleStateCreateInfo multisample_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
        .sampleShadingEnable = VK_FALSE,
        .minSampleShading = 0.0,
        .alphaToCoverageEnable = VK_FALSE,
        .alphaToOneEnable = VK_FALSE,
    };

    VkPipelineDepthStencilStateCreateInfo depth_stencil_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthCompareOp = VK_COMPARE_OP_LESS,
        .front = (VkStencilOpState){
            .failOp = VK_STENCIL_OP_KEEP,
            .passOp = VK_STENCIL_OP_KEEP,
            .depthFailOp = VK_STENCIL_OP_KEEP,
            .compareOp = VK_COMPARE_OP_ALWAYS,
            .writeMask = 0xffffffff,
            .reference = 0xffffffff
        },
        .back = (VkStencilOpState){
            .failOp = VK_STENCIL_OP_KEEP,
            .passOp = VK_STENCIL_OP_KEEP,
            .depthFailOp = VK_STENCIL_OP_KEEP,
            .compareOp = VK_COMPARE_OP_ALWAYS,
            .writeMask = 0xffffffff,
            .reference = 0xffffffff
        },
    };

    VkPipelineColorBlendStateCreateInfo color_blend_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable = VK_FALSE,
        .logicOp = VK_LOGIC_OP_NO_OP,
        .blendConstants = {1.0, 1.0, 1.0, 1.0}
    };

    VkPipelineDynamicStateCreateInfo dynamic_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = (VkDynamicState []){
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR,
        }
    };

    VkPipelineLayoutCreateInfo pipeline_layout_create_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    struct r_pipeline_t *pipelines;

    uint32_t depth_stencil_index = 0xffffffff;

    handle.index = add_stack_list_element(&r_render_passes, NULL);
    render_pass = (struct r_render_pass_t *)get_stack_list_element(&r_render_passes, handle.index);

    subpass_descriptions = alloca(sizeof(struct r_subpass_description_t) * description->subpass_count);
    memcpy(subpass_descriptions, description->subpasses, sizeof(struct r_subpass_description_t) * description->subpass_count);

    for(uint32_t attachment_index = 0; attachment_index < description->attachment_count; attachment_index++)
    {
        /* we may have a depth stencil attachment in the middle of the color attachments, so
        find out how many are color attachments */
        if(!r_IsDepthStencilFormat(description->attachments[attachment_index].format))
        {
            color_attachment_count++;
        }
    }

    color_attachment_references = alloca(sizeof(VkAttachmentReference) * color_attachment_count * description->subpass_count);
    depth_stencil_attachment_references = alloca(sizeof(VkAttachmentReference) * description->subpass_count);
    /* Each sub pass will have its own VkPipeline object. Each pipeline has a VkPipelineColorBlendStateCreateInfo,
    which contains an array of VkPipelineColorBlendAttachmentState. Each element of this array matches an element
    of the pColorAttachments array in the sub pass. Since pColorAttachments will have at most color_attachment_count
    elements, it's safe to allocate the same amount, as just a single VkPipeline will be created at each time, and
    a VkPipeline refers only to a single sub pass */
    color_blend_state.pAttachments = alloca(sizeof(VkPipelineColorBlendAttachmentState) * color_attachment_count);

    /* since missing attachment state will be all the same, just pre fill everything with the same
    values upfront */
    for(uint32_t attachment_index = 0; attachment_index < color_attachment_count; attachment_index++)
    {
        /* drop the pesky const */
        attachment_state = (VkPipelineColorBlendAttachmentState *)color_blend_state.pAttachments + attachment_index;
        attachment_state->blendEnable = VK_FALSE;
        attachment_state->srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        attachment_state->dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        attachment_state->colorBlendOp = VK_BLEND_OP_ADD;
        attachment_state->srcAlphaBlendFactor = 1.0;
        attachment_state->dstAlphaBlendFactor = 1.0;
        attachment_state->alphaBlendOp = VK_BLEND_OP_ADD;
        attachment_state->colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    }

    for(uint32_t subpass_index = 0; subpass_index < description->subpass_count; subpass_index++)
    {
        uint32_t set_layout_count = 0;
        uint32_t push_constant_count = 0;
        uint32_t vertex_binding_count = 0;
        uint32_t vertex_attribute_count = 0;

        subpass_description = subpass_descriptions + subpass_index;
        subpass_description->pipeline_bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS;

        if(subpass_description->pipeline_description->shader_count > pipeline_create_info.stageCount)
        {
            pipeline_create_info.stageCount = subpass_description->pipeline_description->shader_count;
        }

        for(uint32_t shader_index = 0; shader_index < subpass_description->pipeline_description->shader_count; shader_index++)
        {
            struct r_shader_t *shader = subpass_description->pipeline_description->shaders[shader_index];
            set_layout_count += shader->descriptor_set_layout != VK_NULL_HANDLE;
            push_constant_count += shader->push_constant_count;
        }

        if(set_layout_count > pipeline_layout_create_info.setLayoutCount)
        {
            pipeline_layout_create_info.setLayoutCount = set_layout_count;
        }

        if(push_constant_count > pipeline_layout_create_info.pushConstantRangeCount)
        {
            pipeline_layout_create_info.pushConstantRangeCount = push_constant_count;
        }

        VkAttachmentReference *attachment_references = color_attachment_references;

        for(uint32_t attachment_index = 0; attachment_index < description->attachment_count; attachment_index++)
        {
            if(r_IsDepthStencilFormat(description->attachments[attachment_index].format))
            {
                /* if we got a non-color attachment here it means we won't use this
                color reference. To allow us to take this same reference in the next
                loop iteration we need to decrement it here to compensate for the
                attachment_index counter being incremented */
                color_attachment_references--;
                continue;
            }

            attachment_reference = color_attachment_references + attachment_index;
            attachment_reference->attachment = VK_ATTACHMENT_UNUSED;
            attachment_reference->layout = VK_IMAGE_LAYOUT_UNDEFINED;

            for(uint32_t reference_index = 0; reference_index < subpass_description->color_attachment_count; reference_index++)
            {
                /* if this sub pass uses no color reference all the references will be marked as unused */
                if(attachment_index == subpass_description->color_attachments[reference_index].attachment)
                {
                    memcpy(attachment_reference, subpass_description->color_attachments + reference_index, sizeof(VkAttachmentReference));
                    if(attachment_reference->layout == VK_IMAGE_LAYOUT_UNDEFINED)
                    {
                        /* since VK_IMAGE_LAYOUT_UNDEFINED is 0, this means this attachment
                        probably had this field zero initialized. So, fill in a sane value here */
                        attachment_reference->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    }
                }
            }
        }

        subpass_description->color_attachments = attachment_references;
        subpass_description->color_attachment_count = color_attachment_count;
        color_attachment_references += description->attachment_count;

        attachment_reference = depth_stencil_attachment_references;
        attachment_reference->attachment = VK_ATTACHMENT_UNUSED;
        attachment_reference->layout = VK_IMAGE_LAYOUT_UNDEFINED;

        if(subpass_description->depth_stencil_attachment)
        {
            memcpy(attachment_reference, subpass_description->depth_stencil_attachment, sizeof(VkAttachmentReference));
            if(attachment_reference->layout == VK_IMAGE_LAYOUT_UNDEFINED)
            {
                /* since VK_IMAGE_LAYOUT_UNDEFINED is 0, this means this attachment
                probably had this field zero initialized. So, fill in a sane value here */
                attachment_reference->layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            }
        }
        subpass_description->depth_stencil_attachment = depth_stencil_attachment_references;
        depth_stencil_attachment_references++;
    }

    render_pass_create_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    render_pass_create_info.pNext = NULL;
    render_pass_create_info.flags = 0;
    render_pass_create_info.attachmentCount = description->attachment_count;
    render_pass_create_info.pAttachments = alloca(sizeof(VkAttachmentDescription) * description->attachment_count);

    if(description->attachment_count)
    {
        memcpy((void *)render_pass_create_info.pAttachments, description->attachments, sizeof(VkAttachmentDescription) * description->attachment_count);
        for(uint32_t attachment_index = 0; attachment_index < description->attachment_count; attachment_index++)
        {
            attachment_description = (VkAttachmentDescription *)render_pass_create_info.pAttachments + attachment_index;

            if(!attachment_description->samples)
            {
                /* probably zero initialized. No samples means one sample */
                attachment_description->samples = VK_SAMPLE_COUNT_1_BIT;
            }

            if(attachment_description->finalLayout == VK_IMAGE_LAYOUT_UNDEFINED)
            {
                /* fill in the final layout with a sane value if we
                didn't get one */
                if(r_IsDepthStencilFormat(attachment_description->format))
                {
                    attachment_description->finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                }
                else
                {
                    attachment_description->finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                }
            }
        }
    }

    render_pass_create_info.subpassCount = description->subpass_count;
    render_pass_create_info.pSubpasses = alloca(sizeof(VkSubpassDescription) * description->subpass_count);
    memset(render_pass_create_info.pSubpasses, 0, sizeof(VkSubpassDescription) * description->subpass_count);

    for(uint32_t subpass_index = 0; subpass_index < description->subpass_count; subpass_index++)
    {
        subpass_description = subpass_descriptions + subpass_index;
        VkSubpassDescription *subpass = render_pass_create_info.pSubpasses + subpass_index;

        subpass->pipelineBindPoint = subpass_description->pipeline_bind_point;
        subpass->inputAttachmentCount = subpass_description->input_attachment_count;
        subpass->pInputAttachments = subpass_description->input_attachments;
        subpass->colorAttachmentCount = subpass_description->color_attachment_count;
        subpass->pColorAttachments = subpass_description->color_attachments;
        subpass->pResolveAttachments = subpass_description->resolve_attachments;
        subpass->pDepthStencilAttachment = subpass_description->depth_stencil_attachment;
        subpass->preserveAttachmentCount = subpass_description->preserve_attachment_count;
        subpass->pPreserveAttachments = subpass_description->preserve_attachments;
    }

    vkCreateRenderPass(r_device, &render_pass_create_info, NULL, &render_pass->render_pass);

    if(description->subpass_count > 1)
    {
        render_pass->pipelines = calloc(description->subpass_count, sizeof(struct r_pipeline_t));
        pipelines = render_pass->pipelines;
    }
    else
    {
        pipelines = &render_pass->pipeline;
    }

    pipeline_create_info.pStages = alloca(sizeof(VkPipelineShaderStageCreateInfo) * pipeline_create_info.stageCount);
    pipeline_layout_create_info.pSetLayouts = alloca(sizeof(VkDescriptorSetLayout) * pipeline_layout_create_info.setLayoutCount);
    pipeline_layout_create_info.pPushConstantRanges = alloca(sizeof(VkPushConstantRange) * pipeline_layout_create_info.pushConstantRangeCount);

    for(uint32_t subpass_index = 0; subpass_index < description->subpass_count; subpass_index++)
    {
        /* create a pipeline object for each subpass in this render pass to reduce verbosity
        some fields in the pipeline description may be zero initialized (but not left uninitialized!).
        Those zero initialized fields will be filled with some default values. */

        pipeline_layout_create_info.setLayoutCount = 0;
        pipeline_layout_create_info.pushConstantRangeCount = 0;
        pipeline_create_info.stageCount = 0;

        subpass_description = subpass_descriptions + subpass_index;

        for(uint32_t shader_index = 0; shader_index < subpass_description->pipeline_description->shader_count; shader_index++)
        {
            struct r_shader_t *shader = subpass_description->pipeline_description->shaders[shader_index];
            if(shader->descriptor_set_layout != VK_NULL_HANDLE)
            {
                VkDescriptorSetLayout *layout = (VkDescriptorSetLayout *)pipeline_layout_create_info.pSetLayouts + pipeline_layout_create_info.setLayoutCount;
                *layout = shader->descriptor_set_layout;
                pipeline_layout_create_info.setLayoutCount++;
            }
            if(shader->push_constant_count)
            {
                for(uint32_t range_index = 0; range_index < shader->push_constant_count; range_index++)
                {
                    VkPushConstantRange *range = (VkPushConstantRange *)pipeline_layout_create_info.pPushConstantRanges + pipeline_layout_create_info.pushConstantRangeCount;
                    pipeline_layout_create_info.pushConstantRangeCount++;
                    range->stageFlags = shader->stage;
                    range->size = shader->push_constants[range_index].size;
                    range->offset = shader->push_constants[range_index].offset;
                }
            }

            VkPipelineShaderStageCreateInfo *shader_stage = (VkPipelineShaderStageCreateInfo *)pipeline_create_info.pStages + shader_index;
            pipeline_create_info.stageCount++;
            shader_stage->sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            shader_stage->pNext = NULL;
            shader_stage->flags = 0;
            shader_stage->pSpecializationInfo = NULL;
            shader_stage->stage = shader->stage;
            shader_stage->module = shader->module;
            shader_stage->pName = "main";
            shader_stage->pSpecializationInfo = NULL;
        }

        vkCreatePipelineLayout(r_device, &pipeline_layout_create_info, NULL, &pipelines[subpass_index].layout);
        /* copy the pipeline description to a local variable so zero initialized fields can be filled
        without affecting the description passed in. */
        memcpy(&pipeline_description, subpass_description->pipeline_description, sizeof(struct r_pipeline_description_t));

        memcpy(&vertex_input_state, pipeline_description.vertex_input_state, sizeof(VkPipelineVertexInputStateCreateInfo));
        vertex_input_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        pipeline_description.vertex_input_state = &vertex_input_state;

        if(!pipeline_description.input_assembly_state)
        {
            /* the default value here is triangle lists */
            pipeline_description.input_assembly_state = &input_assembly_state;
        }

        if(!pipeline_description.viewport_state)
        {
            /* by default assume outside code will provide viewport and
            scissor rects info */
            pipeline_description.viewport_state = &viewport_state;
        }

        if(!pipeline_description.rasterization_state)
        {
            pipeline_description.rasterization_state = &rasterization_state;
        }

        if(!pipeline_description.multisample_state)
        {
            pipeline_description.multisample_state = &multisample_state;
        }

        if(!pipeline_description.depth_stencil_state)
        {
            /* OpenGL disabled depth testing, by default. Here we'll enable
            it by default if there's a depth-stencil attachment reference in this subpass.
            Stencil test remains disabled by default. */
            pipeline_description.depth_stencil_state = &depth_stencil_state;
            depth_stencil_state.depthTestEnable = subpass_description->depth_stencil_attachment != NULL;
            depth_stencil_state.depthWriteEnable = subpass_description->depth_stencil_attachment!= NULL;
        }

        if(!pipeline_description.color_blend_state)
        {
            pipeline_description.color_blend_state = &color_blend_state;
            color_blend_state.attachmentCount = color_attachment_count;
        }

        if(!pipeline_description.dynamic_state)
        {
            pipeline_description.dynamic_state = &dynamic_state;
        }

        pipeline_create_info.pVertexInputState = pipeline_description.vertex_input_state;
        pipeline_create_info.pInputAssemblyState = pipeline_description.input_assembly_state;
        pipeline_create_info.pTessellationState = pipeline_description.tesselation_state;
        pipeline_create_info.pViewportState = pipeline_description.viewport_state;
        pipeline_create_info.pRasterizationState = pipeline_description.rasterization_state;
        pipeline_create_info.pMultisampleState = pipeline_description.multisample_state;
        pipeline_create_info.pDepthStencilState = pipeline_description.depth_stencil_state;
        pipeline_create_info.pColorBlendState = pipeline_description.color_blend_state;
        pipeline_create_info.pDynamicState = pipeline_description.dynamic_state;
        pipeline_create_info.layout = pipelines[subpass_index].layout;
        pipeline_create_info.renderPass = render_pass->render_pass;
        pipeline_create_info.subpass = subpass_index;
        pipeline_create_info.basePipelineHandle = VK_NULL_HANDLE;
        pipeline_create_info.basePipelineIndex = 0;

        vkCreateGraphicsPipelines(r_device, VK_NULL_HANDLE, 1, &pipeline_create_info, NULL, &pipelines[subpass_index].pipeline);
    }

    return handle;
}

void r_DestroyRenderPass(struct r_render_pass_handle_t handle)
{

}

struct r_render_pass_t *r_GetRenderPassPointer(struct r_render_pass_handle_t handle)
{
    struct r_render_pass_t *render_pass;

    render_pass = get_stack_list_element(&r_render_passes, handle.index);
//
//    if(render_pass && !(render_pass->description.attachment_count && render_pass->description.subpass_count))
//    {
//        render_pass = NULL;
//    }

    return render_pass;
}

/*
=================================================================
=================================================================
=================================================================
*/

struct r_framebuffer_handle_t r_AllocFramebuffer()
{
//    struct r_framebuffer_handle_t handle;
//    struct r_framebuffer_t *framebuffer;
//
//    SDL_AtomicLock(&r_resource_spinlock);
//    handle.index = add_stack_list_element(&r_framebuffers, NULL);
//    add_stack_list_element(&r_framebuffer_descriptions, NULL);
//    SDL_AtomicUnlock(&r_resource_spinlock);
//
//    framebuffer = get_stack_list_element(&r_framebuffers, handle.index);
//    framebuffer->texture_count = 0;
//    return handle;
}

struct r_framebuffer_handle_t r_CreateFramebuffer(struct r_framebuffer_description_t *description)
{
    struct r_framebuffer_handle_t handle = R_INVALID_FRAMEBUFFER_HANDLE;
    struct r_framebuffer_t *framebuffer;
    struct r_texture_description_t texture_description = {};
    struct r_texture_handle_t *textures;
    struct r_texture_t *texture;
    VkFramebufferCreateInfo framebuffer_create_info = {};
    VkAttachmentDescription *attachment_description;

    if(description)
    {
        handle.index = add_stack_list_element(&r_framebuffers, NULL);
        framebuffer = get_stack_list_element(&r_framebuffers, handle.index);
        framebuffer->buffers = calloc(description->frame_count, sizeof(VkFramebuffer));
        framebuffer->textures = calloc(description->attachment_count * description->frame_count, sizeof(struct r_texture_handle_t));
//        framebuffer->attachment_count = description->attachment_count;

        framebuffer_create_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebuffer_create_info.pNext = NULL;
        framebuffer_create_info.flags = 0;
        framebuffer_create_info.renderPass = description->render_pass->render_pass;
        framebuffer_create_info.attachmentCount = description->attachment_count;
        framebuffer_create_info.pAttachments = alloca(sizeof(VkImageView) * description->attachment_count);
        framebuffer_create_info.width = description->width;
        framebuffer_create_info.height = description->height;
        framebuffer_create_info.layers = 1;

        texture_description.extent.width = description->width;
        texture_description.extent.height = description->height;
        texture_description.extent.depth = 1;
        texture_description.array_layers = 1;
        texture_description.image_type = VK_IMAGE_TYPE_2D;
        texture_description.mip_levels = 1;
        texture_description.sampler_params.addr_mode_u = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        texture_description.sampler_params.addr_mode_v = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        texture_description.sampler_params.addr_mode_w = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        texture_description.sampler_params.mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        texture_description.sampler_params.min_filter = VK_FILTER_NEAREST;
        texture_description.sampler_params.mag_filter = VK_FILTER_NEAREST;

        for(uint32_t frame_index = 0; frame_index < description->frame_count; frame_index++)
        {
            textures = framebuffer->textures + frame_index * description->attachment_count;
            for(uint32_t attachment_index = 0; attachment_index < description->attachment_count; attachment_index++)
            {
                attachment_description = description->attachments + attachment_index;
                texture_description.format = attachment_description->format;
                textures[attachment_index] = r_CreateTexture(&texture_description);
                texture = r_GetTexturePointer(textures[attachment_index]);
                *(VkImageView *)(framebuffer_create_info.pAttachments + attachment_index) = texture->image_view;
            }

            vkCreateFramebuffer(r_device, &framebuffer_create_info, NULL, framebuffer->buffers + frame_index);
        }
    }

    return handle;
}

void r_DestroyFramebuffer(struct r_framebuffer_handle_t handle)
{

}

struct r_framebuffer_t *r_GetFramebufferPointer(struct r_framebuffer_handle_t handle)
{
    struct r_framebuffer_t *framebuffer;
//
    framebuffer = get_stack_list_element(&r_framebuffers, handle.index);
//    if(framebuffer && framebuffer->description.attachment_count == 0xff)
//    {
//        framebuffer = NULL;
//    }
//
    return framebuffer;
}

void r_PresentFramebuffer(struct r_framebuffer_handle_t handle)
{

}

/*
=================================================================
=================================================================
=================================================================
*/

struct r_swapchain_handle_t r_CreateSwapchain(VkSwapchainCreateInfoKHR *description)
{
    struct r_swapchain_handle_t handle = R_INVALID_SWAPCHAIN_HANDLE;
    struct r_swapchain_t *swapchain;
    VkSwapchainCreateInfoKHR *description_copy;
    VkImageCreateInfo image_description = {};
    VkSurfaceCapabilitiesKHR surface_capabilites;
    VkSurfaceFormatKHR surface_format;
    VkImage *swapchain_images;
    struct r_image_t *image;
    uint32_t swapchain_image_count;

    handle.index = add_stack_list_element(&r_swapchains, NULL);
    swapchain = get_stack_list_element(&r_swapchains, handle.index);

    description_copy = alloca(sizeof(VkSwapchainCreateInfoKHR));
    memcpy(description_copy, description, sizeof(VkSwapchainCreateInfoKHR));
    description = description_copy;

    description->sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;

    if(!description->minImageCount)
    {
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(r_physical_device, description->surface, &surface_capabilites);
        description->minImageCount = surface_capabilites.minImageCount;
    }

    if(!description->imageArrayLayers)
    {
        description->imageArrayLayers = 1;
    }

    if(!description->imageUsage)
    {
        description->imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }

    description->imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    if(!description->imageFormat)
    {
        vkGetPhysicalDeviceSurfaceFormatsKHR(r_physical_device, description->surface, &(int){1}, &surface_format);
        description->imageFormat = surface_format.format;
        description->imageColorSpace = surface_format.colorSpace;
    }

    if(!description->preTransform)
    {
        description->preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    }

    if(!description->compositeAlpha)
    {
        description->compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    }

    vkCreateSwapchainKHR(r_device, description, NULL, &swapchain->swapchain);
    swapchain->image_count = description->minImageCount;
    swapchain->images = calloc(sizeof(struct r_image_handle_t), swapchain->image_count);
    swapchain_images = alloca(sizeof(VkImage) * swapchain->image_count);

    vkGetSwapchainImagesKHR(r_device, swapchain->swapchain, &swapchain_image_count, NULL);
    vkGetSwapchainImagesKHR(r_device, swapchain->swapchain, &swapchain_image_count, swapchain_images);

    image_description.imageType = VK_IMAGE_TYPE_2D;
    image_description.format = description->imageFormat;
    image_description.extent.width = description->imageExtent.width;
    image_description.extent.height = description->imageExtent.height;
    image_description.extent.depth = 1;

    for(uint32_t image_index = 0; image_index < swapchain_image_count; image_index++)
    {
        swapchain->images[image_index] = r_CreateImage(&image_description);
        image = r_GetImagePointer(swapchain->images[image_index]);
        vkDestroyImage(r_device, image->image, NULL);
        image->image = swapchain_images[image_index];
    }

    return handle;
}

void r_DestroySwapchain(struct r_swapchain_handle_t handle)
{

}

struct r_swapchain_t *r_GetSwapchainPointer(struct r_swapchain_handle_t handle)
{
    struct r_swapchain_t *swapchain = NULL;
    swapchain = get_stack_list_element(&r_swapchains, handle.index);
    return swapchain;
}

void r_NextImage(struct r_swapchain_handle_t handle)
{
    struct r_swapchain_t *swapchain;
    uint32_t image_index;
    swapchain = r_GetSwapchainPointer(handle);
    vkResetFences(r_device, 1, &r_transfer_fence);
    vkAcquireNextImageKHR(r_device, swapchain->swapchain, 0xffffffffffffffff, VK_NULL_HANDLE, r_transfer_fence, &image_index);
    vkWaitForFences(r_device, 1, &r_transfer_fence, VK_TRUE, 0xffffffffffffffff);
    swapchain->current_image = image_index;
}

/*
=================================================================
=================================================================
=================================================================
*/

struct r_alloc_handle_t r_Alloc(uint32_t size, uint32_t align, uint32_t index_alloc)
{
    struct r_alloc_t *alloc;
    struct r_alloc_t new_alloc;
    struct r_alloc_handle_t handle = R_INVALID_ALLOC_HANDLE;
    uint32_t start;
    uint32_t block_size;
    struct list_t *free_blocks;

    index_alloc = index_alloc && 1;
    // align--;

    free_blocks = &r_renderer.free_blocks[index_alloc];

    for(uint32_t i = 0; i < free_blocks->cursor; i++)
    {
        alloc = (struct r_alloc_t *)get_list_element(free_blocks, i);
        /* align the start of the allocation to the desired alignment */
        // start = (alloc->start + align) & (~align);

        start = alloc->start;

        while(start % align)
        {
            start += align - (start % align);
        }

        /* subtract the bytes before the aligned start from
        the total size */
        block_size = alloc->size - (start - alloc->start);

        if(block_size >= size)
        {
            /* compute the how many bytes of alignment we need */
            align = start - alloc->start;

            if(block_size == size)
            {
                /* taking away the aligment bytes, this buffer is just the right size for
                the allocation required, so just move this allocation header from the
                free list to the allocation list*/
                alloc->align = align;
                handle.alloc_index = add_stack_list_element(&r_renderer.allocd_blocks[index_alloc], alloc);
                remove_list_element(free_blocks, i);
            }
            else
            {
                /* this buffer is bigger than the requested
                size, even after adjusting for alignment */
                block_size = size + align;

                /* we'll create a new allocation header to add
                in the allocations list */
                new_alloc.start = alloc->start;
                new_alloc.size = block_size;
                new_alloc.align = align;

                handle.alloc_index = add_stack_list_element(&r_renderer.allocd_blocks[index_alloc], &new_alloc);

                /* chop a chunk of this free block */
                alloc->start += block_size;
                alloc->size -= block_size;
            }

            // handle.alloc_index = i;
            handle.is_index = index_alloc;
            break;
        }
    }

    return handle;
}

struct r_alloc_t *r_GetAllocPointer(struct r_alloc_handle_t handle)
{
    struct r_alloc_t *alloc;
    alloc = (struct r_alloc_t *)get_stack_list_element(&r_renderer.allocd_blocks[handle.is_index], handle.alloc_index);

    if(alloc && !alloc->size)
    {
        /* zero sized allocs are considered invalid */
        alloc = NULL;
    }

    return alloc;
}

void r_Free(struct r_alloc_handle_t handle)
{
    struct r_alloc_t *alloc = r_GetAllocPointer(handle);

    if(alloc)
    {
        add_list_element(&r_renderer.free_blocks[handle.is_index], alloc);

        /* zero sized allocs are considered invalid */
        alloc->size = 0;
    }
}

void r_Memcpy(struct r_alloc_handle_t handle, void *data, uint32_t size)
{
    void *memory;
    struct r_alloc_t *alloc;
    uint32_t alloc_size;

    alloc = r_GetAllocPointer(handle);

    if(alloc)
    {
//        alloc_size = alloc->size - alloc->align;
//
//        if(size > alloc_size)
//        {
//            size = alloc_size;
//        }
//
//        /* backend specific mapping function. It's necessary to pass both
//        the handle and the alloc, as the backend doesn't use any of the
//        interface's functions, and can't get the alloc pointer from the
//        handle. It also needs the handle to know whether it's an vertex
//        or and index alloc. */
//        memory = r_vk_MapAlloc(handle, alloc);
//        memcpy(memory, data, size);
//        r_vk_UnmapAlloc(handle);
    }
}

/*
=================================================================
=================================================================
=================================================================
*/

void r_SetZPlanes(float z_near, float z_far)
{
    r_renderer.z_near = z_near;
    r_renderer.z_far = z_far;
    r_RecomputeViewProjectionMatrix();
}

void r_SetFovY(float fov_y)
{
    r_renderer.fov_y = fov_y;
    r_RecomputeViewProjectionMatrix();
}

void r_RecomputeViewProjectionMatrix()
{
    mat4_t_persp(&r_renderer.projection_matrix, r_renderer.fov_y,
        (float)r_renderer.width / (float)r_renderer.height, r_renderer.z_near, r_renderer.z_far);
//    r_vk_AdjustProjectionMatrix(&r_renderer.projection_matrix);
    mat4_t_mul(&r_renderer.view_projection_matrix, &r_renderer.view_matrix, &r_renderer.projection_matrix);
}


void r_SetViewMatrix(mat4_t *view_matrix)
{
    memcpy(&r_renderer.view_matrix, view_matrix, sizeof(mat4_t));
    mat4_t_invvm(&r_renderer.view_matrix, &r_renderer.view_matrix);
    r_RecomputeViewProjectionMatrix();
}

void r_GetWindowSize(uint32_t *width, uint32_t *height)
{
    *width = r_renderer.width;
    *height = r_renderer.height;
}

/*
=================================================================
=================================================================
=================================================================
*/

struct r_material_handle_t r_AllocMaterial()
{
    struct r_material_handle_t handle = R_INVALID_MATERIAL_HANDLE;
    struct r_material_t *material;

    handle.index = add_stack_list_element(&r_renderer.materials, NULL);

    material = (struct r_material_t*)get_stack_list_element(&r_renderer.materials, handle.index);
    material->flags = 0;
    material->diffuse_texture = NULL;
    material->normal_texture = NULL;

    return handle;
}

void r_FreeMaterial(struct r_material_handle_t handle)
{
    struct r_material_t *material;

    material = r_GetMaterialPointer(handle);

    if(material)
    {
        material->flags = R_MATERIAL_FLAG_INVALID;
        free(material->name);
        remove_stack_list_element(&r_renderer.materials, handle.index);
    }
}

struct r_material_t *r_GetMaterialPointer(struct r_material_handle_t handle)
{
    struct r_material_t *material;
    material = (struct r_material_t*)get_stack_list_element(&r_renderer.materials, handle.index);

    if(material && (material->flags & R_MATERIAL_FLAG_INVALID))
    {
        material = NULL;
    }

    return material;
}

struct r_material_handle_t r_GetMaterialHandle(char *name)
{
    struct r_material_handle_t handle = R_INVALID_MATERIAL_HANDLE;
    struct r_material_t *material;

    for(uint32_t i = 0; i < r_renderer.materials.cursor; i++)
    {
        material = r_GetMaterialPointer(R_MATERIAL_HANDLE(i));

        if(material && !strcmp(name, material->name))
        {
            handle = R_MATERIAL_HANDLE(i);
            break;
        }
    }

    return handle;
}

/*
=================================================================
=================================================================
=================================================================
*/

struct r_light_handle_t r_CreateLight(vec3_t *position, float radius, vec3_t *color)
{
    struct r_light_handle_t handle = R_INVALID_LIGHT_HANDLE;
    struct r_light_t *light;

    handle.index = add_stack_list_element(&r_renderer.lights, NULL);

    light = (struct r_light_t*)get_stack_list_element(&r_renderer.lights, handle.index);
    light->pos_radius.comps[0] = position->comps[0];
    light->pos_radius.comps[1] = position->comps[1];
    light->pos_radius.comps[2] = position->comps[2];
    light->pos_radius.comps[3] = radius;
    light->color.comps[0] = color->comps[0];
    light->color.comps[1] = color->comps[1];
    light->color.comps[2] = color->comps[2];
    light->color.comps[3] = 0.0;

    return handle;
}

struct r_light_t *r_GetLightPointer(struct r_light_handle_t handle)
{
    struct r_light_t *light;
    light = (struct r_light_t*)get_stack_list_element(&r_renderer.lights, handle.index);

    if(light && light->pos_radius.comps[3] < 0.0)
    {
        light = NULL;
    }

    return light;
}

void r_DestroyLight(struct r_light_handle_t handle)
{
    struct r_light_t *light;

    light = r_GetLightPointer(handle);

    if(light)
    {
        light->pos_radius.comps[3] = -1.0;
        remove_stack_list_element(&r_renderer.lights, handle.index);
    }
}

/*
=================================================================
=================================================================
=================================================================
*/

void *r_AllocCmdData(uint32_t size)
{
    uint32_t elem_count;
    void *data;
    elem_count = ((size + R_CMD_DATA_ELEM_SIZE - 1) & (~(R_CMD_DATA_ELEM_SIZE - 1))) / R_CMD_DATA_ELEM_SIZE;
    /* this is not a safe allocation scheme, and it doesn't have to. Data
    WILL be overwritten if the producer end is queueing stuff too fast and
    the consumer end is taking too long to use the data */

    if(elem_count > r_renderer.cmd_buffer_data.next_in - r_renderer.cmd_buffer_data.buffer_size)
    {
        r_renderer.cmd_buffer_data.next_in = 0;
    }

    data = (char *)r_renderer.cmd_buffer_data.buffer + r_renderer.cmd_buffer_data.next_in * R_CMD_DATA_ELEM_SIZE;
    r_renderer.cmd_buffer_data.next_in += elem_count;
    r_renderer.cmd_buffer_data.next_out = r_renderer.cmd_buffer_data.next_in;

    return data;
}

void *r_AtomicAllocCmdData(uint32_t size)
{
    void *data;

    SDL_AtomicLock(&r_renderer.cmd_buffer_lock);
    data = r_AllocCmdData(size);
    SDL_AtomicUnlock(&r_renderer.cmd_buffer_lock);

    return data;
}

void r_BeginSubmission(mat4_t *view_projection_matrix, mat4_t *view_matrix)
{
    r_renderer.submiting_draw_cmd_buffer = (struct r_draw_cmd_buffer_t*)r_AtomicAllocCmdData(sizeof(struct r_draw_cmd_buffer_t));
    r_renderer.submiting_draw_cmd_buffer->draw_cmd_count = 0;
    memcpy(&r_renderer.submiting_draw_cmd_buffer->view_projection_matrix, view_projection_matrix, sizeof(mat4_t));
    memcpy(&r_renderer.submiting_draw_cmd_buffer->view_matrix, view_matrix, sizeof(mat4_t));
}

void r_SubmitDrawCmd(mat4_t *model_matrix, struct r_material_t *material, uint32_t start, uint32_t count)
{
    struct r_draw_cmd_t *draw_cmd;

    if(r_renderer.submiting_draw_cmd_buffer->draw_cmd_count >= R_DRAW_CMD_BUFFER_DRAW_CMDS)
    {
        r_EndSubmission();
        r_BeginSubmission(&r_renderer.submiting_draw_cmd_buffer->view_projection_matrix, &r_renderer.submiting_draw_cmd_buffer->view_matrix);
    }
    draw_cmd = r_renderer.submiting_draw_cmd_buffer->draw_cmds + r_renderer.submiting_draw_cmd_buffer->draw_cmd_count;
    draw_cmd->material = material;
    draw_cmd->range.start = start;
    draw_cmd->range.count = count;
    memcpy(&draw_cmd->model_matrix, model_matrix, sizeof(mat4_t));
    r_renderer.submiting_draw_cmd_buffer->draw_cmd_count++;
}

void r_EndSubmission()
{
    if(r_renderer.submiting_draw_cmd_buffer->draw_cmd_count)
    {
        r_QueueCmd(R_CMD_TYPE_DRAW, r_renderer.submiting_draw_cmd_buffer, 0);
    }
}


// void r_SortDrawCmds(struct r_udraw_cmd_buffer_t *udraw_cmd_buffer)
// {
//     // struct r_udraw_cmd_buffer_t *udraw_cmd_buffer;
//     struct r_udraw_cmd_t *udraw_cmd;
//     struct r_draw_cmd_t *draw_cmd;
//     struct r_draw_cmd_buffer_t *draw_cmd_buffer = NULL;
//     struct r_material_t *current_material = NULL;

//     // udraw_cmd_buffer = (struct r_udraw_cmd_buffer_t *)cmd->data;
//     qsort(udraw_cmd_buffer->draw_cmds, udraw_cmd_buffer->draw_cmd_count, sizeof(struct r_udraw_cmd_t), r_CmpDrawCmd);

//     // for(uint32_t i = 0; i < udraw_cmd_buffer->draw_cmd_count; i++)
//     // {
//     //     printf("material %s\n", udraw_cmd_buffer->draw_cmds[i].material->name);
//     // }

//     draw_cmd_buffer = r_AtomicAllocCmdData(sizeof(struct r_draw_cmd_buffer_t) + sizeof(struct r_draw_cmd_t) * (R_MAX_DRAW_CMDS - 1));
//     draw_cmd_buffer->draw_cmd_count = 0;
//     draw_cmd_buffer->view_projection_matrix = udraw_cmd_buffer->view_projection_matrix;
//     draw_cmd_buffer->view_matrix = udraw_cmd_buffer->view_matrix;
//     draw_cmd_buffer->material = udraw_cmd_buffer->draw_cmds[0].material;
//     current_material = draw_cmd_buffer->material;

//     for(uint32_t i = 0; i < udraw_cmd_buffer->draw_cmd_count; i++)
//     {
//         udraw_cmd = udraw_cmd_buffer->draw_cmds + i;

//         if(udraw_cmd->material != current_material ||
//             draw_cmd_buffer->draw_cmd_count >= R_MAX_DRAW_CMDS)
//         {
//             current_material = udraw_cmd->material;
//             r_QueueCmd(R_CMD_TYPE_DRAW, draw_cmd_buffer, 0);

//             draw_cmd_buffer = r_AtomicAllocCmdData(sizeof(struct r_draw_cmd_buffer_t) + sizeof(struct r_draw_cmd_t) * (R_MAX_DRAW_CMDS - 1));
//             draw_cmd_buffer->material = current_material;
//             draw_cmd_buffer->view_projection_matrix = udraw_cmd_buffer->view_projection_matrix;
//             draw_cmd_buffer->view_matrix = udraw_cmd_buffer->view_matrix;
//             draw_cmd_buffer->draw_cmd_count = 0;
//         }

//         draw_cmd = draw_cmd_buffer->draw_cmds + draw_cmd_buffer->draw_cmd_count;
//         draw_cmd->model_matrix = udraw_cmd->model_matrix;
//         draw_cmd->range = udraw_cmd->range;
//         draw_cmd_buffer->draw_cmd_count++;
//     }

//     if(draw_cmd_buffer->draw_cmd_count)
//     {
//         r_QueueCmd(R_CMD_TYPE_DRAW, draw_cmd_buffer, 0);
//     }
// }

void r_QueueCmd(uint32_t type, void *data, uint32_t data_size)
{
    struct r_cmd_t cmd;

    cmd.cmd_type = type;

    SDL_AtomicLock(&r_renderer.cmd_buffer_lock);

    if(data && data_size)
    {
        cmd.data = r_AllocCmdData(data_size);
        memcpy(cmd.data, data, data_size);
    }
    else
    {
        cmd.data = data;
    }

    add_ringbuffer_element(&r_renderer.cmd_buffer, &cmd);

    SDL_AtomicUnlock(&r_renderer.cmd_buffer_lock);
}
struct r_cmd_t *r_NextCmd()
{
    return (struct r_cmd_t *)peek_ringbuffer_element(&r_renderer.cmd_buffer);
}

void r_AdvanceCmd()
{
    SDL_AtomicLock(&r_renderer.cmd_buffer_lock);
    get_ringbuffer_element(&r_renderer.cmd_buffer);
    SDL_AtomicUnlock(&r_renderer.cmd_buffer_lock);
}

int r_ExecuteCmds(void *data)
{
    struct r_cmd_t *cmd;
    SDL_Delay(1000);
    while(1)
    {


//        VkCommandBufferBeginInfo command_buffer_begin_info = {};
//        command_buffer_begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
//        command_buffer_begin_info.pNext = NULL;
//        command_buffer_begin_info.flags = 0;
//        command_buffer_begin_info.pInheritanceInfo = NULL;
//
//        struct r_render_pass_t *render_pass = r_GetRenderPassPointer(r_render_pass);
//        struct r_framebuffer_t *framebuffer = r_GetFramebufferPointer(r_framebuffer);
//        struct r_texture_t *texture = r_GetDefaultTexturePointer();
//        struct r_swapchain_t *swapchain = r_GetSwapchainPointer(r_swapchain);
//
//        VkRenderPassBeginInfo render_pass_begin_info = {};
//        render_pass_begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
//        render_pass_begin_info.pNext = NULL;
//        render_pass_begin_info.renderPass = render_pass->render_pass;
//        render_pass_begin_info.framebuffer = framebuffer->buffers[0];
//        render_pass_begin_info.renderArea.offset.x = 0;
//        render_pass_begin_info.renderArea.offset.y = 0;
//        render_pass_begin_info.renderArea.extent.width = 800;
//        render_pass_begin_info.renderArea.extent.height = 600;
//        render_pass_begin_info.clearValueCount = 1;
//        render_pass_begin_info.pClearValues = (VkClearValue []){
//            {.color.float32 = {1.0, 0.0, 0.0, 1.0}}
//        };
//
//        r_NextImage(r_swapchain);
//        vkBeginCommandBuffer(r_transfer_command_buffer, &command_buffer_begin_info);
//        r_CmdBlitImage(texture->image, swapchain->images[swapchain->current_image], NULL);
////        vkCmdBeginRenderPass(r_draw_command_buffer, &render_pass_begin_info, VK_SUBPASS_CONTENTS_INLINE);
////        vkCmdEndRenderPass(r_draw_command_buffer);
//        r_CmdSetImageLayout(swapchain->images[swapchain->current_image], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
//        vkEndCommandBuffer(r_transfer_command_buffer);
//
//        vkResetFences(r_device, 1, &r_draw_fence);
//
//        VkSubmitInfo submit_info = {};
//        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
//        submit_info.pNext = NULL;
//        submit_info.waitSemaphoreCount = 0;
//        submit_info.pWaitSemaphores = NULL;
//        submit_info.pWaitDstStageMask = (VkPipelineStageFlagBits []){VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
//        submit_info.commandBufferCount = 1;
//        submit_info.pCommandBuffers = &r_transfer_command_buffer;
//        submit_info.signalSemaphoreCount = 0;
//        submit_info.pSignalSemaphores = NULL;
//        vkQueueSubmit(r_graphics_queue, 1, &submit_info, r_draw_fence);
//        vkWaitForFences(r_device, 1, &r_draw_fence, VK_TRUE, 0xffffffffffffffff);
//
//        VkPresentInfoKHR present_info = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
//        present_info.swapchainCount = 1;
//        present_info.pSwapchains = &swapchain->swapchain;
//        present_info.pImageIndices = (int []){swapchain->current_image};
//        vkQueuePresentKHR(r_graphics_queue, &present_info);
//
//        SDL_Delay(100);
//        continue;

        while(r_renderer.cmd_buffer.next_in != r_renderer.cmd_buffer.next_out)
        {
            cmd = r_NextCmd();

            switch(cmd->cmd_type)
            {
                case R_CMD_TYPE_BEGIN_FRAME:
//                    r_vk_BeginFrame();
                break;

                case R_CMD_TYPE_DRAW:
                    r_Draw(cmd);
                break;

                case R_CMD_TYPE_END_FRAME:
//                    r_vk_EndFrame();
                break;
            }

            r_AdvanceCmd();
        }
    }
}

void r_WaitEmptyQueue()
{
    while(r_renderer.cmd_buffer.next_in != r_renderer.cmd_buffer.next_out);
}

int r_CmpDrawCmd(const void *a, const void *b)
{
    return (ptrdiff_t)((struct r_draw_cmd_t *)a)->material - (ptrdiff_t)((struct r_draw_cmd_t *)b)->material;
}

void r_Draw(struct r_cmd_t *cmd)
{
    struct r_draw_cmd_buffer_t *cmd_buffer;
    struct r_draw_cmd_t *draw_cmds;
    uint32_t draw_cmd_index;
    struct r_material_t *cur_material;

    cmd_buffer = (struct r_draw_cmd_buffer_t *)cmd->data;
    qsort(cmd_buffer->draw_cmds, cmd_buffer->draw_cmd_count, sizeof(struct r_draw_cmd_t), r_CmpDrawCmd);
//    r_vk_PushViewMatrix(&cmd_buffer->view_matrix);
    draw_cmds = cmd_buffer->draw_cmds;
    while(cmd_buffer->draw_cmd_count)
    {
        draw_cmd_index = 0;
        cur_material = draw_cmds[0].material;
        while(cur_material == draw_cmds[draw_cmd_index].material)draw_cmd_index++;
//        r_vk_Draw(cur_material, &cmd_buffer->view_projection_matrix, draw_cmds, draw_cmd_index);
        draw_cmds += draw_cmd_index;
        cmd_buffer->draw_cmd_count -= draw_cmd_index;
    }
}

/*
=================================================================================
=================================================================================
=================================================================================
*/

void r_DrawPoint(vec3_t* position, vec3_t* color)
{

}
