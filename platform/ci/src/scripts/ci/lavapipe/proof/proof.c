// Uses two limits of the bundled lavapipe for real, beyond what it advertises:
//  A. a storage buffer binding of 2^31-4 bytes, written at its last u32;
//  B. a fragment shader writing 64 storage buffers and 64 storage images.
// Loads the library given on the command line directly (no loader).
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkGetDeviceProcAddr gdpa;
static VkInstance inst;
static VkDevice dev;
static VkPhysicalDevice pd;
static VkQueue q;
static uint32_t qf;

#define I(n) PFN_vk##n n = (PFN_vk##n)gipa(inst, "vk" #n)
#define D(n) PFN_vk##n n = (PFN_vk##n)gdpa(dev, "vk" #n)
#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("FAIL %s = %d (line %d)\n", #x, r_, __LINE__); exit(1); } } while (0)

static uint32_t *read_file(const char *p, size_t *n) {
  FILE *f = fopen(p, "rb"); if (!f) { printf("no %s\n", p); exit(1); }
  fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
  uint32_t *b = malloc(*n); fread(b, 1, *n, f); fclose(f); return b;
}
static VkShaderModule shader(const char *p) {
  D(CreateShaderModule); size_t n; uint32_t *c = read_file(p, &n);
  VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, n, c};
  VkShaderModule m; CK(CreateShaderModule(dev, &ci, 0, &m)); return m;
}
static uint32_t memtype(uint32_t bits, VkMemoryPropertyFlags want) {
  I(GetPhysicalDeviceMemoryProperties); VkPhysicalDeviceMemoryProperties mp;
  GetPhysicalDeviceMemoryProperties(pd, &mp);
  for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
    if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
  printf("no memory type\n"); exit(1);
}
static VkDeviceMemory bind_buffer(VkBuffer b, void **map) {
  D(GetBufferMemoryRequirements); D(AllocateMemory); D(BindBufferMemory); D(MapMemory);
  VkMemoryRequirements r; GetBufferMemoryRequirements(dev, b, &r);
  VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, r.size,
    memtype(r.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
  VkDeviceMemory m; CK(AllocateMemory(dev, &ai, 0, &m)); CK(BindBufferMemory(dev, b, m, 0));
  if (map) CK(MapMemory(dev, m, 0, VK_WHOLE_SIZE, 0, map));
  return m;
}
static VkBuffer buffer(VkDeviceSize size, VkBufferUsageFlags usage) {
  D(CreateBuffer); VkBufferCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, size, usage};
  VkBuffer b; CK(CreateBuffer(dev, &ci, 0, &b)); return b;
}
static VkCommandBuffer begin(VkCommandPool *pool) {
  D(CreateCommandPool); D(AllocateCommandBuffers); D(BeginCommandBuffer);
  VkCommandPoolCreateInfo pc = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, 0, qf};
  CK(CreateCommandPool(dev, &pc, 0, pool));
  VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, *pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
  VkCommandBuffer cb; CK(AllocateCommandBuffers(dev, &ai, &cb));
  VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  CK(BeginCommandBuffer(cb, &bi)); return cb;
}
static void submit(VkCommandBuffer cb) {
  D(EndCommandBuffer); D(QueueSubmit); D(QueueWaitIdle);
  CK(EndCommandBuffer(cb));
  VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &cb};
  CK(QueueSubmit(q, 1, &si, VK_NULL_HANDLE)); CK(QueueWaitIdle(q));
}

static int test_big(void) {
  D(CreateDescriptorSetLayout); D(CreatePipelineLayout); D(CreateComputePipelines); D(CreateDescriptorPool);
  D(AllocateDescriptorSets); D(UpdateDescriptorSets); D(CmdBindPipeline); D(CmdBindDescriptorSets); D(CmdDispatch);
  const VkDeviceSize size = (1ull << 31) - 4;
  VkBuffer b = buffer(size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  void *map; bind_buffer(b, &map);
  memset(map, 0, 16); memset((char *)map + size - 16, 0, 16);
  VkDescriptorSetLayoutBinding lb = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT};
  VkDescriptorSetLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, 0, 0, 1, &lb};
  VkDescriptorSetLayout sl; CK(CreateDescriptorSetLayout(dev, &lci, 0, &sl));
  VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, 1, &sl};
  VkPipelineLayout pl; CK(CreatePipelineLayout(dev, &pli, 0, &pl));
  VkComputePipelineCreateInfo cpi = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, 0, 0,
    {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_COMPUTE_BIT, shader("big.comp.spv"), "main"}, pl};
  VkPipeline p; CK(CreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, 0, &p));
  VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
  VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, 0, 0, 1, 1, &ps};
  VkDescriptorPool dp; CK(CreateDescriptorPool(dev, &dpi, 0, &dp));
  VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, 0, dp, 1, &sl};
  VkDescriptorSet ds; CK(AllocateDescriptorSets(dev, &dai, &ds));
  VkDescriptorBufferInfo bi = {b, 0, size};
  VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, ds, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0, &bi};
  UpdateDescriptorSets(dev, 1, &w, 0, 0);
  VkCommandPool pool; VkCommandBuffer cb = begin(&pool);
  CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
  CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, 0);
  CmdDispatch(cb, 1, 1, 1);
  submit(cb);
  uint32_t first = ((uint32_t *)map)[0], last = ((uint32_t *)((char *)map + size - 4))[0];
  int ok = first == 0x12345678u && last == 0xdeadbeefu;
  printf("A storage buffer binding of %llu bytes: first %#x last %#x -> %s\n",
         (unsigned long long)size, first, last, ok ? "OK" : "WRONG");
  return ok;
}

static int test_many(void) {
  enum { N = 64 };
  D(CreateDescriptorSetLayout); D(CreatePipelineLayout); D(CreateGraphicsPipelines); D(CreateDescriptorPool);
  D(AllocateDescriptorSets); D(UpdateDescriptorSets); D(CmdBindPipeline); D(CmdBindDescriptorSets); D(CmdDraw);
  D(CreateImage); D(GetImageMemoryRequirements); D(AllocateMemory); D(BindImageMemory); D(CreateImageView);
  D(CmdPipelineBarrier); D(CmdCopyImageToBuffer); D(CmdBeginRendering); D(CmdEndRendering);
  VkBuffer sb = buffer(N * 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  void *smap; bind_buffer(sb, &smap); memset(smap, 0, N * 256);
  VkBuffer rb = buffer(N * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
  void *rmap; bind_buffer(rb, &rmap); memset(rmap, 0, N * 4);
  VkImage img[N]; VkImageView view[N];
  for (int i = 0; i < N; i++) {
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 0, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R32_UINT,
      {1, 1, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
      VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, 0, VK_IMAGE_LAYOUT_UNDEFINED};
    CK(CreateImage(dev, &ici, 0, &img[i]));
    VkMemoryRequirements r; GetImageMemoryRequirements(dev, img[i], &r);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, r.size, memtype(r.memoryTypeBits, 0)};
    VkDeviceMemory m; CK(AllocateMemory(dev, &ai, 0, &m)); CK(BindImageMemory(dev, img[i], m, 0));
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0, 0, img[i], VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R32_UINT,
      {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    CK(CreateImageView(dev, &vci, 0, &view[i]));
  }
  VkDescriptorSetLayoutBinding lb[2] = {
    {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, N, VK_SHADER_STAGE_FRAGMENT_BIT},
    {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, N, VK_SHADER_STAGE_FRAGMENT_BIT}};
  VkDescriptorSetLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, 0, 0, 2, lb};
  VkDescriptorSetLayout sl; CK(CreateDescriptorSetLayout(dev, &lci, 0, &sl));
  VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, 1, &sl};
  VkPipelineLayout pl; CK(CreatePipelineLayout(dev, &pli, 0, &pl));
  VkPipelineShaderStageCreateInfo st[2] = {
    {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_VERTEX_BIT, shader("many.vert.spv"), "main"},
    {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_FRAGMENT_BIT, shader("many.frag.spv"), "main"}};
  VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, 0, 0, VK_PRIMITIVE_TOPOLOGY_POINT_LIST};
  VkViewport vp = {0, 0, 1, 1, 0, 1}; VkRect2D sc = {{0, 0}, {1, 1}};
  VkPipelineViewportStateCreateInfo vs = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, 0, 0, 1, &vp, 1, &sc};
  VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, 0, 0, VK_SAMPLE_COUNT_1_BIT};
  VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  VkPipelineRenderingCreateInfo rci = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  VkGraphicsPipelineCreateInfo gpi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rci, 0, 2, st, &vi, &ia, 0, &vs, &rs, &ms, 0, &cbs, 0, pl};
  VkPipeline p; CK(CreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, 0, &p));
  VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, N}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, N}};
  VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, 0, 0, 1, 2, ps};
  VkDescriptorPool dp; CK(CreateDescriptorPool(dev, &dpi, 0, &dp));
  VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, 0, dp, 1, &sl};
  VkDescriptorSet ds; CK(AllocateDescriptorSets(dev, &dai, &ds));
  VkDescriptorBufferInfo bi[N]; VkDescriptorImageInfo ii[N];
  for (int i = 0; i < N; i++) { bi[i] = (VkDescriptorBufferInfo){sb, i * 256, 4}; ii[i] = (VkDescriptorImageInfo){0, view[i], VK_IMAGE_LAYOUT_GENERAL}; }
  VkWriteDescriptorSet w[2] = {
    {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, ds, 0, 0, N, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0, bi},
    {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, ds, 1, 0, N, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, ii}};
  UpdateDescriptorSets(dev, 2, w, 0, 0);
  VkCommandPool pool; VkCommandBuffer cb = begin(&pool);
  VkImageMemoryBarrier toGeneral[N];
  for (int i = 0; i < N; i++)
    toGeneral[i] = (VkImageMemoryBarrier){VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0, 0, VK_ACCESS_SHADER_WRITE_BIT,
      VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img[i],
      {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
  CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, 0, 0, 0, N, toGeneral);
  VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, 0, 0, {{0, 0}, {1, 1}}, 1, 0, 0, 0, 0, 0};
  CmdBeginRendering(cb, &ri);
  CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
  CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &ds, 0, 0);
  CmdDraw(cb, 1, 1, 0, 0);
  CmdEndRendering(cb);
  VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_HOST_READ_BIT};
  CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, 0, 0, 0);
  for (int i = 0; i < N; i++) {
    VkBufferImageCopy c = {i * 4, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {1, 1, 1}};
    CmdCopyImageToBuffer(cb, img[i], VK_IMAGE_LAYOUT_GENERAL, rb, 1, &c);
  }
  submit(cb);
  int bad = 0;
  for (int i = 0; i < N; i++) {
    uint32_t bv = *(uint32_t *)((char *)smap + i * 256), iv = ((uint32_t *)rmap)[i];
    if (bv != (uint32_t)(i + 1) || iv != (uint32_t)(i + 100)) { bad++; if (bad < 4) printf("  slot %d: buffer %u image %u\n", i, bv, iv); }
  }
  printf("B fragment shader with %d storage buffers and %d storage images: %d wrong -> %s\n", N, N, bad, bad ? "WRONG" : "OK");
  return !bad;
}

int main(int argc, char **argv) {
  void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!lib) { printf("dlopen: %s\n", dlerror()); return 1; }
  gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
  PFN_vkCreateInstance CreateInstance = (PFN_vkCreateInstance)gipa(0, "vkCreateInstance");
  VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "proof", 1, 0, 0, VK_API_VERSION_1_3};
  VkInstanceCreateInfo ic = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &app};
  CK(CreateInstance(&ic, 0, &inst));
  I(EnumeratePhysicalDevices); I(GetPhysicalDeviceProperties); I(CreateDevice); I(GetPhysicalDeviceQueueFamilyProperties);
  uint32_t n = 1; CK(EnumeratePhysicalDevices(inst, &n, &pd));
  VkPhysicalDeviceProperties pr; GetPhysicalDeviceProperties(pd, &pr);
  printf("device %s; advertised maxStorageBufferRange %u, maxFragmentCombinedOutputResources %u\n",
         pr.deviceName, pr.limits.maxStorageBufferRange, pr.limits.maxFragmentCombinedOutputResources);
  uint32_t nq = 8; VkQueueFamilyProperties qp[8]; GetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp);
  for (qf = 0; qf < nq && !(qp[qf].queueFlags & VK_QUEUE_GRAPHICS_BIT); qf++) {}
  float prio = 1.0f;
  VkDeviceQueueCreateInfo dq = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, qf, 1, &prio};
  VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  f13.dynamicRendering = VK_TRUE;
  VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
  VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12};
  f2.features.fragmentStoresAndAtomics = VK_TRUE;
  f2.features.shaderStorageBufferArrayDynamicIndexing = VK_TRUE;
  f2.features.shaderStorageImageArrayDynamicIndexing = VK_TRUE;
  VkDeviceCreateInfo dc = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2, 0, 1, &dq};
  CK(CreateDevice(pd, &dc, 0, &dev));
  gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
  D(GetDeviceQueue); GetDeviceQueue(dev, qf, 0, &q);
  int a = test_big(), b = test_many();
  return a && b ? 0 : 2;
}
