/*
 * GPUBrew runner - executes GPUBrewLauncher ABI 1 shader apps as a native
 * HBLauncher package.
 *
 * The host turns a GPUBrew app directory (app.json + SPIR-V) into a launch of
 * this image with the app's files as assets (host/lib/packages.cjs):
 *
 *   gpubrew/desc.bin       struct gpubrew_desc
 *   gpubrew/{vertex,fragment,compute}.spv
 *   gpubrew/{compute_begin,compute_after}.spv   (computeSequence only)
 *   gpubrew/state.bin      first <= 1024 bytes of the storage buffer
 *   gpubrew/data.bin       copied to byte 1024 onwards
 *   gpubrew/memory.bin     initial private memory (binding 1, optional)
 *
 * Contract kept from GPUBrewLauncher (REFERENCE.md "GPU programming
 * interface"): an 8 MiB std430 storage buffer at set 0 binding 0 visible to
 * every stage; one (1,1,1) dispatch per frame (or begin + repeat x
 * [compute, after] with barriers); a full-screen 3-vertex draw; controller
 * input in raw[0..3] and touch[] each frame; exit when reserved.z != 0.
 *
 * Not bridged yet: the hardware extension block at 0x7f0000 (motion,
 * keyboard, mouse, Joy-Con mouse, NFC, UDP, FTP) and audio effects.  Apps that
 * test hbAvailable() fall back to plain pad/touch input.
 */
#include "hbx_sdk.h"
#include "hbx_app.h"
#include "hbx_vk.h"

#define GB_DESC_MAGIC    0x31574247u  /* "GBW1" */
#define GB_STATE_BYTES   1024u
#define GB_RAW           0u
#define GB_TOUCH         128u
#define GB_RESERVED_Z    1016u
#define GB_MAX_STAGES    5u

struct gpubrew_desc {
    uint32_t magic;
    uint32_t repeat;          /* computeSequence.repeat, 0 = single dispatch */
    uint32_t memory_bytes;    /* private binding 1, 0 = none */
    uint32_t storage_bytes;   /* binding 0 size (8 MiB) */
    uint32_t reserved[4];
};

enum { ST_VERTEX, ST_FRAGMENT, ST_COMPUTE, ST_BEGIN, ST_AFTER };

struct runner {
    const struct hbx_vk_dispatch *d;
    VkDevice device;
    VkQueue queue;
    VkSwapchainKHR swapchain;
    VkImage images[HBX_MAX_IMAGES];
    VkImageView views[HBX_MAX_IMAGES];
    uint32_t image_count, width, height;
    VkFormat format;

    struct gpubrew_desc desc;
    VkBuffer storage, memory;
    VkDeviceMemory storage_mem, memory_mem;
    uint8_t *state;
    VkShaderModule modules[GB_MAX_STAGES];
    VkDescriptorSetLayout set_layout;
    VkDescriptorPool pool;
    VkDescriptorSet set;
    VkPipelineLayout layout;
    VkPipeline graphics, compute[GB_MAX_STAGES];
    VkCommandPool commands_pool;
    VkCommandBuffer commands[HBX_MAX_IMAGES];
    VkSemaphore acquire, draw_done[HBX_MAX_IMAGES];
    uint32_t frame;
};

static const char *const STAGE_ASSET[GB_MAX_STAGES] = {
    "gpubrew/vertex.spv", "gpubrew/fragment.spv", "gpubrew/compute.spv",
    "gpubrew/compute_begin.spv", "gpubrew/compute_after.spv"
};

static int load_module(const struct hbx_ctx *ctx, struct runner *r, uint32_t stage) {
    VkShaderModuleCreateInfo info;
    uint32_t size = 0;
    const void *words = hbx_asset(ctx, STAGE_ASSET[stage], &size);
    if (!words || size < 20u || (size & 3u)) {
        hbx_logf(ctx, "gpubrew: missing %s", STAGE_ASSET[stage]);
        return HBX_ERR_ASSET;
    }
    hbx_memset(&info, 0, sizeof info);
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = size;
    info.pCode = (const uint32_t *)words;
    return r->d->vkCreateShaderModule(r->device, &info, NULL, &r->modules[stage]) == VK_SUCCESS
        ? HBX_OK : HBX_ERR_GPU;
}

static int create_buffers(const struct hbx_ctx *ctx, struct runner *r) {
    const VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    uint32_t size = 0;
    const void *bytes;
    void *mapped = NULL;

    if (hbx_vk_host_buffer(ctx, r->desc.storage_bytes, usage, &r->storage,
                           &r->storage_mem, &mapped) != VK_SUCCESS || !mapped)
        return HBX_ERR_GPU;
    r->state = (uint8_t *)mapped;
    /* GPUBrew clears the whole buffer, then copies state and data. */
    hbx_memset(r->state, 0, r->desc.storage_bytes);
    bytes = hbx_asset(ctx, "gpubrew/state.bin", &size);
    if (bytes && size) hbx_memcpy(r->state, bytes, size > GB_STATE_BYTES ? GB_STATE_BYTES : size);
    bytes = hbx_asset(ctx, "gpubrew/data.bin", &size);
    if (bytes && size && size <= r->desc.storage_bytes - GB_STATE_BYTES)
        hbx_memcpy(r->state + GB_STATE_BYTES, bytes, size);
    /* Clear launch/exit requests (reserved.y/.z). */
    hbx_memset(r->state + 1012u, 0, 8u);

    if (r->desc.memory_bytes) {
        mapped = NULL;
        if (hbx_vk_host_buffer(ctx, r->desc.memory_bytes, usage, &r->memory,
                               &r->memory_mem, &mapped) != VK_SUCCESS || !mapped)
            return HBX_ERR_GPU;
        hbx_memset(mapped, 0, r->desc.memory_bytes);
        bytes = hbx_asset(ctx, "gpubrew/memory.bin", &size);
        if (bytes && size && size <= r->desc.memory_bytes) hbx_memcpy(mapped, bytes, size);
    }
    return HBX_OK;
}

static int create_descriptors(struct runner *r) {
    const struct hbx_vk_dispatch *d = r->d;
    VkDescriptorSetLayoutBinding bindings[2];
    VkDescriptorSetLayoutCreateInfo set_info;
    VkDescriptorPoolSize pool_size;
    VkDescriptorPoolCreateInfo pool_info;
    VkDescriptorSetAllocateInfo allocate;
    VkPipelineLayoutCreateInfo layout_info;
    VkDescriptorBufferInfo buffers[2];
    VkWriteDescriptorSet writes[2];
    uint32_t count = r->desc.memory_bytes ? 2u : 1u, i;

    hbx_memset(bindings, 0, sizeof bindings);
    for (i = 0; i < count; i++) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                                 VK_SHADER_STAGE_COMPUTE_BIT;
    }
    hbx_memset(&set_info, 0, sizeof set_info);
    set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_info.bindingCount = count;
    set_info.pBindings = bindings;
    if (d->vkCreateDescriptorSetLayout(r->device, &set_info, NULL, &r->set_layout) != VK_SUCCESS)
        return HBX_ERR_GPU;

    pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_size.descriptorCount = count;
    hbx_memset(&pool_info, 0, sizeof pool_info);
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    if (d->vkCreateDescriptorPool(r->device, &pool_info, NULL, &r->pool) != VK_SUCCESS)
        return HBX_ERR_GPU;

    hbx_memset(&allocate, 0, sizeof allocate);
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool = r->pool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &r->set_layout;
    if (d->vkAllocateDescriptorSets(r->device, &allocate, &r->set) != VK_SUCCESS)
        return HBX_ERR_GPU;

    hbx_memset(buffers, 0, sizeof buffers);
    hbx_memset(writes, 0, sizeof writes);
    buffers[0].buffer = r->storage;
    buffers[0].range = r->desc.storage_bytes;
    buffers[1].buffer = r->memory;
    buffers[1].range = r->desc.memory_bytes;
    for (i = 0; i < count; i++) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = r->set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &buffers[i];
    }
    d->vkUpdateDescriptorSets(r->device, count, writes, 0, NULL);

    hbx_memset(&layout_info, 0, sizeof layout_info);
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &r->set_layout;
    return d->vkCreatePipelineLayout(r->device, &layout_info, NULL, &r->layout) == VK_SUCCESS
        ? HBX_OK : HBX_ERR_GPU;
}

static int create_graphics(struct runner *r) {
    VkPipelineShaderStageCreateInfo stages[2];
    VkPipelineVertexInputStateCreateInfo vertex_input;
    VkPipelineInputAssemblyStateCreateInfo assembly;
    VkViewport viewport;
    VkRect2D scissor;
    VkPipelineViewportStateCreateInfo viewport_state;
    VkPipelineRasterizationStateCreateInfo raster;
    VkPipelineMultisampleStateCreateInfo multisample;
    VkPipelineColorBlendAttachmentState blend_attachment;
    VkPipelineColorBlendStateCreateInfo blend;
    VkPipelineRenderingCreateInfo rendering;
    VkGraphicsPipelineCreateInfo info;
    uint32_t i;

    hbx_memset(stages, 0, sizeof stages);
    for (i = 0; i < 2; i++) {
        stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
        stages[i].module = r->modules[i ? ST_FRAGMENT : ST_VERTEX];
        stages[i].pName = "main";
    }
    hbx_memset(&vertex_input, 0, sizeof vertex_input);
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    hbx_memset(&assembly, 0, sizeof assembly);
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    /* GPUBrew shaders are written for a fixed 1920x1080 target. */
    hbx_memset(&viewport, 0, sizeof viewport);
    viewport.width = (float)r->width;
    viewport.height = (float)r->height;
    viewport.maxDepth = 1.0f;
    hbx_memset(&scissor, 0, sizeof scissor);
    scissor.extent.width = r->width;
    scissor.extent.height = r->height;
    hbx_memset(&viewport_state, 0, sizeof viewport_state);
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.pViewports = &viewport;
    viewport_state.scissorCount = 1;
    viewport_state.pScissors = &scissor;

    hbx_memset(&raster, 0, sizeof raster);
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    hbx_memset(&multisample, 0, sizeof multisample);
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    hbx_memset(&blend_attachment, 0, sizeof blend_attachment);
    blend_attachment.colorWriteMask = 0xf;
    hbx_memset(&blend, 0, sizeof blend);
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;
    hbx_memset(&rendering, 0, sizeof rendering);
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &r->format;

    hbx_memset(&info, 0, sizeof info);
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext = &rendering;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport_state;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &blend;
    info.layout = r->layout;
    return r->d->vkCreateGraphicsPipelines(r->device, VK_NULL_HANDLE, 1, &info, NULL,
                                           &r->graphics) == VK_SUCCESS ? HBX_OK : HBX_ERR_GPU;
}

static int create_compute(struct runner *r, uint32_t stage) {
    VkComputePipelineCreateInfo info;
    hbx_memset(&info, 0, sizeof info);
    info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = r->modules[stage];
    info.stage.pName = "main";
    info.layout = r->layout;
    return r->d->vkCreateComputePipelines(r->device, VK_NULL_HANDLE, 1, &info, NULL,
                                          &r->compute[stage]) == VK_SUCCESS ? HBX_OK : HBX_ERR_GPU;
}

static void put_u32(uint8_t *base, uint32_t offset, uint32_t value) {
    hbx_memcpy(base + offset, &value, 4);
}

/* raw[0..3] mirrors nn::hid NpadState (buttons at +8, sticks at +16, attributes
 * at +32) and touch[] the GetTouchScreenState<16> layout GPUBrew shaders read.
 * Rebuilt from the SDK's merged input so every pad source works. */
static void write_input(struct runner *r, const struct hbx_input_state *input) {
    uint8_t *s = r->state;
    uint32_t i, count = input->touch_count > HBX_MAX_TOUCH ? HBX_MAX_TOUCH : input->touch_count;
    hbx_memset(s + GB_RAW, 0, 64);
    put_u32(s, GB_RAW + 0, r->frame);
    put_u32(s, GB_RAW + 8, input->buttons);
    put_u32(s, GB_RAW + 16, (uint32_t)(input->stick_lx * 32767 / 100));
    put_u32(s, GB_RAW + 20, (uint32_t)(input->stick_ly * 32767 / 100));
    put_u32(s, GB_RAW + 24, (uint32_t)(input->stick_rx * 32767 / 100));
    put_u32(s, GB_RAW + 28, (uint32_t)(input->stick_ry * 32767 / 100));
    put_u32(s, GB_RAW + 32, input->connected ? 1u : 0u);
    hbx_memset(s + GB_TOUCH, 0, 656);
    put_u32(s, GB_TOUCH + 8, count);
    for (i = 0; i < count; i++) {
        put_u32(s, GB_TOUCH + (8u + i * 10u) * 4u, (uint32_t)input->touch[i].x);
        put_u32(s, GB_TOUCH + (9u + i * 10u) * 4u, (uint32_t)input->touch[i].y);
    }
}

static void compute_barrier(const struct hbx_vk_dispatch *d, VkCommandBuffer cmd,
                            VkPipelineStageFlags dst_stage, VkAccessFlags dst_access) {
    VkMemoryBarrier barrier;
    hbx_memset(&barrier, 0, sizeof barrier);
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = dst_access;
    d->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, dst_stage, 0,
                            1, &barrier, 0, NULL, 0, NULL);
}

static void dispatch(const struct hbx_vk_dispatch *d, struct runner *r,
                     VkCommandBuffer cmd, uint32_t stage) {
    d->vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->compute[stage]);
    d->vkCmdDispatch(cmd, 1, 1, 1);
    compute_barrier(d, cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
}

static VkResult draw_frame(struct runner *r) {
    const struct hbx_vk_dispatch *d = r->d;
    VkCommandBufferBeginInfo begin;
    VkImageMemoryBarrier barrier;
    VkRenderingAttachmentInfo attachment;
    VkRenderingInfo rendering;
    VkSubmitInfo submit;
    VkPresentInfoKHR present_info;
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkCommandBuffer cmd;
    uint32_t index = 0, i;
    VkResult res;

    res = d->vkAcquireNextImageKHR(r->device, r->swapchain, UINT64_MAX, r->acquire,
                                   VK_NULL_HANDLE, &index);
    if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) return res;
    if (index >= r->image_count) index = 0;
    cmd = r->commands[index];
    d->vkResetCommandBuffer(cmd, 0);

    hbx_memset(&begin, 0, sizeof begin);
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    d->vkBeginCommandBuffer(cmd, &begin);

    d->vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->layout, 0, 1,
                               &r->set, 0, NULL);
    if (r->desc.repeat) {
        dispatch(d, r, cmd, ST_BEGIN);
        for (i = 0; i < r->desc.repeat; i++) {
            dispatch(d, r, cmd, ST_COMPUTE);
            dispatch(d, r, cmd, ST_AFTER);
        }
    } else {
        dispatch(d, r, cmd, ST_COMPUTE);
    }
    compute_barrier(d, cmd, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);

    hbx_memset(&barrier, 0, sizeof barrier);
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = r->images[index];
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    d->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                            0, NULL, 0, NULL, 1, &barrier);

    hbx_memset(&attachment, 0, sizeof attachment);
    attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    attachment.imageView = r->views[index];
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.clearValue.color.float32[0] = 0.012f;
    attachment.clearValue.color.float32[1] = 0.02f;
    attachment.clearValue.color.float32[2] = 0.045f;
    attachment.clearValue.color.float32[3] = 1.0f;
    hbx_memset(&rendering, 0, sizeof rendering);
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea.extent.width = r->width;
    rendering.renderArea.extent.height = r->height;
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &attachment;
    d->vkCmdBeginRendering(cmd, &rendering);
    d->vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->graphics);
    d->vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->layout, 0, 1,
                               &r->set, 0, NULL);
    d->vkCmdDraw(cmd, 3, 1, 0, 0);
    d->vkCmdEndRendering(cmd);

    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = 0;
    d->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                            0, NULL, 0, NULL, 1, &barrier);
    d->vkEndCommandBuffer(cmd);

    hbx_memset(&submit, 0, sizeof submit);
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &r->acquire;
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &r->draw_done[index];
    res = d->vkQueueSubmit(r->queue, 1, &submit, VK_NULL_HANDLE);
    if (res != VK_SUCCESS) return res;

    hbx_memset(&present_info, 0, sizeof present_info);
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = &r->draw_done[index];
    present_info.swapchainCount = 1;
    present_info.pSwapchains = &r->swapchain;
    present_info.pImageIndices = &index;
    res = d->vkQueuePresentKHR(r->queue, &present_info);
    /* The CPU writes input into the shared buffer next frame: wait first. */
    d->vkQueueWaitIdle(r->queue);
    return res;
}

static void destroy(struct runner *r) {
    const struct hbx_vk_dispatch *d = r->d;
    uint32_t i;
    if (!r->device) return;
    d->vkDeviceWaitIdle(r->device);
    if (d->vkDestroyPipeline) {
        if (r->graphics) d->vkDestroyPipeline(r->device, r->graphics, NULL);
        for (i = 0; i < GB_MAX_STAGES; i++)
            if (r->compute[i]) d->vkDestroyPipeline(r->device, r->compute[i], NULL);
    }
    if (d->vkDestroyShaderModule)
        for (i = 0; i < GB_MAX_STAGES; i++)
            if (r->modules[i]) d->vkDestroyShaderModule(r->device, r->modules[i], NULL);
    if (r->layout && d->vkDestroyPipelineLayout) d->vkDestroyPipelineLayout(r->device, r->layout, NULL);
    if (r->pool && d->vkDestroyDescriptorPool) d->vkDestroyDescriptorPool(r->device, r->pool, NULL);
    if (r->set_layout && d->vkDestroyDescriptorSetLayout)
        d->vkDestroyDescriptorSetLayout(r->device, r->set_layout, NULL);
    if (r->commands_pool && d->vkDestroyCommandPool)
        d->vkDestroyCommandPool(r->device, r->commands_pool, NULL);
    if (d->vkDestroySemaphore) {
        if (r->acquire) d->vkDestroySemaphore(r->device, r->acquire, NULL);
        for (i = 0; i < r->image_count && i < HBX_MAX_IMAGES; i++)
            if (r->draw_done[i]) d->vkDestroySemaphore(r->device, r->draw_done[i], NULL);
    }
    if (r->storage) d->vkDestroyBuffer(r->device, r->storage, NULL);
    if (r->storage_mem) d->vkFreeMemory(r->device, r->storage_mem, NULL);
    if (r->memory) d->vkDestroyBuffer(r->device, r->memory, NULL);
    if (r->memory_mem) d->vkFreeMemory(r->device, r->memory_mem, NULL);
}

static int setup(const struct hbx_ctx *ctx, struct runner *r) {
    const struct hbx_vk_dispatch *d = r->d;
    const struct gpubrew_desc *desc;
    VkCommandPoolCreateInfo pool_info;
    VkCommandBufferAllocateInfo allocate_info;
    VkSemaphoreCreateInfo semaphore;
    uint32_t size = 0, i;
    int status;

    desc = (const struct gpubrew_desc *)hbx_asset(ctx, "gpubrew/desc.bin", &size);
    if (!desc || size < sizeof *desc || desc->magic != GB_DESC_MAGIC) {
        hbx_log(ctx, "gpubrew: missing or bad descriptor");
        return HBX_ERR_ASSET;
    }
    r->desc = *desc;
    if (r->desc.storage_bytes < GB_STATE_BYTES) return HBX_ERR_ASSET;
    if (!d->vkCreateComputePipelines || !d->vkCmdDispatch) {
        hbx_log(ctx, "gpubrew: host did not resolve compute entrypoints");
        return HBX_ERR_UNSUPPORTED;
    }

    for (i = 0; i < (r->desc.repeat ? GB_MAX_STAGES : 3u); i++)
        if ((status = load_module(ctx, r, i)) != HBX_OK) return status;
    if ((status = create_buffers(ctx, r)) != HBX_OK) return status;
    if ((status = create_descriptors(r)) != HBX_OK) return status;
    if ((status = create_graphics(r)) != HBX_OK) return status;
    for (i = ST_COMPUTE; i < (r->desc.repeat ? GB_MAX_STAGES : 3u); i++)
        if ((status = create_compute(r, i)) != HBX_OK) return status;

    hbx_memset(&pool_info, 0, sizeof pool_info);
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = ctx->gpu.queue_family;
    if (d->vkCreateCommandPool(r->device, &pool_info, NULL, &r->commands_pool) != VK_SUCCESS)
        return HBX_ERR_GPU;
    hbx_memset(&allocate_info, 0, sizeof allocate_info);
    allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate_info.commandPool = r->commands_pool;
    allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate_info.commandBufferCount = r->image_count;
    if (d->vkAllocateCommandBuffers(r->device, &allocate_info, r->commands) != VK_SUCCESS)
        return HBX_ERR_GPU;
    hbx_memset(&semaphore, 0, sizeof semaphore);
    semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    if (d->vkCreateSemaphore(r->device, &semaphore, NULL, &r->acquire) != VK_SUCCESS)
        return HBX_ERR_GPU;
    for (i = 0; i < r->image_count && i < HBX_MAX_IMAGES; i++)
        if (d->vkCreateSemaphore(r->device, &semaphore, NULL, &r->draw_done[i]) != VK_SUCCESS)
            return HBX_ERR_GPU;
    return HBX_OK;
}

int hbx_app_main(const struct hbx_ctx *ctx) {
    static struct runner r;
    struct hbx_input_state input;
    uint64_t deadline = 0;
    uint32_t exit_word = 0, i;
    int status;

    if (hbx_ctx_check(ctx) != HBX_OK) return HBX_ERR_ABI;
    hbx_memset(&r, 0, sizeof r);
    r.d = hbx_vk(ctx);
    if (!r.d || !ctx->gpu.device || !ctx->gpu.swapchain) {
        hbx_log(ctx, "gpubrew: no borrowed display");
        return HBX_ERR_GPU;
    }
    r.device = (VkDevice)(uintptr_t)ctx->gpu.device;
    r.queue = (VkQueue)(uintptr_t)ctx->gpu.queue;
    r.swapchain = (VkSwapchainKHR)(uintptr_t)ctx->gpu.swapchain;
    r.image_count = ctx->gpu.image_count > HBX_MAX_IMAGES ? HBX_MAX_IMAGES : ctx->gpu.image_count;
    r.width = ctx->gpu.width;
    r.height = ctx->gpu.height;
    r.format = (VkFormat)ctx->gpu.color_format;
    for (i = 0; i < r.image_count; i++) {
        r.images[i] = (VkImage)(uintptr_t)ctx->gpu.images[i];
        r.views[i] = (VkImageView)(uintptr_t)ctx->gpu.views[i];
    }

    status = setup(ctx, &r);
    if (status != HBX_OK) {
        hbx_logf(ctx, "gpubrew: setup failed (%d)", status);
        destroy(&r);
        return status;
    }

    hbx_memset(&input, 0, sizeof input);
    while (!hbx_stop_requested(ctx) && ctx->exit == 0) {
        hbx_input_poll(ctx, &input);
        write_input(&r, &input);
        if (draw_frame(&r) != VK_SUCCESS) {
            hbx_log(ctx, "gpubrew: frame failed");
            status = HBX_ERR_GPU;
            goto done;
        }
        r.frame++;
        hbx_memcpy(&exit_word, r.state + GB_RESERVED_Z, 4);
        if (exit_word) break;
        hbx_frame_end(ctx);
        hbx_pace(ctx, &deadline, 60);
    }
    status = HBX_OK;
done:
    destroy(&r);
    hbx_logf(ctx, "gpubrew: exit after %u frames", r.frame);
    return status;
}
