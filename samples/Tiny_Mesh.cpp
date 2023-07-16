/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cassert>
#if !defined(_USE_MATH_DEFINES)
#define _USE_MATH_DEFINES
#endif // _USE_MATH_DEFINES
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <stdio.h>

#include <glm/ext.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/random.hpp>

#include <lvk/LVK.h>
#include <lvk/HelpersGLFW.h>
#include <lvk/HelpersImGui.h>

#include <stb/stb_image.h>

#include <shared/UtilsFPS.h>

constexpr uint32_t kNumCubes = 16;

std::unique_ptr<lvk::ImGuiRenderer> imgui_;

const char* codeVS = R"(
layout (location=0) in vec3 pos;
layout (location=1) in vec3 col;
layout (location=2) in vec2 st;
layout (location=0) out vec3 color;
layout (location=1) out vec2 uv;

layout(std430, buffer_reference) readonly buffer PerFrame {
  mat4 proj;
  mat4 view;
  uint texture0;
  uint texture1;
  uint sampler0;
};

layout(std430, buffer_reference) readonly buffer PerObject {
  mat4 model;
};

layout(push_constant) uniform constants
{
	PerFrame perFrame;
	PerObject perObject;
} pc;

void main() {
  mat4 proj = pc.perFrame.proj;
  mat4 view = pc.perFrame.view;
  mat4 model = pc.perObject.model;
  gl_Position = proj * view * model * vec4(pos, 1.0);
  color = col;
  uv = st;
}
)";

const char* codeFS = R"(
layout (location=0) in vec3 color;
layout (location=1) in vec2 uv;
layout (location=0) out vec4 out_FragColor;

layout(std430, buffer_reference) readonly buffer PerFrame {
  mat4 proj;
  mat4 view;
  uint texture0;
  uint texture1;
  uint sampler0;
};

layout(push_constant) uniform constants
{
	PerFrame perFrame;
} pc;

void main() {
  vec4 t0 = textureBindless2D(pc.perFrame.texture0, pc.perFrame.sampler0, 2.0*uv);
  vec4 t1 = textureBindless2D(pc.perFrame.texture1, pc.perFrame.sampler0, uv);
  out_FragColor = vec4(color * (t0.rgb + t1.rgb), 1.0);
};
)";

using namespace igl;
using glm::mat4;
using glm::vec2;
using glm::vec3;
using glm::vec4;

vec3 axis_[kNumCubes];

GLFWwindow* window_ = nullptr;
uint32_t width_ = 1280;
uint32_t height_ = 1024;
FramesPerSecondCounter fps_;

constexpr uint32_t kNumBufferedFrames = 3;

std::unique_ptr<IDevice> device_;
igl::Framebuffer framebuffer_;
lvk::Holder<lvk::RenderPipelineHandle> renderPipelineState_Mesh_;
std::shared_ptr<IBuffer> vb0_, ib0_; // buffers for vertices and indices
std::vector<std::shared_ptr<IBuffer>> ubPerFrame_, ubPerObject_;
std::shared_ptr<ITexture> texture0_, texture1_;
std::shared_ptr<ISamplerState> sampler_;
igl::RenderPass renderPass_;
igl::DepthStencilState depthStencilState_;

struct VertexPosUvw {
  vec3 pos;
  vec3 color;
  vec2 uv;
};

struct UniformsPerFrame {
  mat4 proj;
  mat4 view;
  uint32_t texture0;
  uint32_t texture1;
  uint32_t sampler;
};
struct UniformsPerObject {
  mat4 model;
};

const float half = 1.0f;

// UV-mapped cube with indices: 24 vertices, 36 indices
static VertexPosUvw vertexData0[] = {
    // top
    {{-half, -half, +half}, {0.0, 0.0, 1.0}, {0, 0}}, // 0
    {{+half, -half, +half}, {1.0, 0.0, 1.0}, {1, 0}}, // 1
    {{+half, +half, +half}, {1.0, 1.0, 1.0}, {1, 1}}, // 2
    {{-half, +half, +half}, {0.0, 1.0, 1.0}, {0, 1}}, // 3
    // bottom
    {{-half, -half, -half}, {1.0, 1.0, 1.0}, {0, 0}}, // 4
    {{-half, +half, -half}, {0.0, 1.0, 0.0}, {0, 1}}, // 5
    {{+half, +half, -half}, {1.0, 1.0, 0.0}, {1, 1}}, // 6
    {{+half, -half, -half}, {1.0, 0.0, 0.0}, {1, 0}}, // 7
    // left
    {{+half, +half, -half}, {1.0, 1.0, 0.0}, {1, 0}}, // 8
    {{-half, +half, -half}, {0.0, 1.0, 0.0}, {0, 0}}, // 9
    {{-half, +half, +half}, {0.0, 1.0, 1.0}, {0, 1}}, // 10
    {{+half, +half, +half}, {1.0, 1.0, 1.0}, {1, 1}}, // 11
    // right
    {{-half, -half, -half}, {1.0, 1.0, 1.0}, {0, 0}}, // 12
    {{+half, -half, -half}, {1.0, 0.0, 0.0}, {1, 0}}, // 13
    {{+half, -half, +half}, {1.0, 0.0, 1.0}, {1, 1}}, // 14
    {{-half, -half, +half}, {0.0, 0.0, 1.0}, {0, 1}}, // 15
    // front
    {{+half, -half, -half}, {1.0, 0.0, 0.0}, {0, 0}}, // 16
    {{+half, +half, -half}, {1.0, 1.0, 0.0}, {1, 0}}, // 17
    {{+half, +half, +half}, {1.0, 1.0, 1.0}, {1, 1}}, // 18
    {{+half, -half, +half}, {1.0, 0.0, 1.0}, {0, 1}}, // 19
    // back
    {{-half, +half, -half}, {0.0, 1.0, 0.0}, {1, 0}}, // 20
    {{-half, -half, -half}, {1.0, 1.0, 1.0}, {0, 0}}, // 21
    {{-half, -half, +half}, {0.0, 0.0, 1.0}, {0, 1}}, // 22
    {{-half, +half, +half}, {0.0, 1.0, 1.0}, {1, 1}}, // 23
};

static uint16_t indexData[] = {0,  1,  2,  2,  3,  0,  4,  5,  6,  6,  7,  4,
                               8,  9,  10, 10, 11, 8,  12, 13, 14, 14, 15, 12,
                               16, 17, 18, 18, 19, 16, 20, 21, 22, 22, 23, 20};

UniformsPerObject perObject[kNumCubes];

static void initIGL() {
  device_ = lvk::createVulkanDeviceWithSwapchain(
      window_, width_, height_, {.maxTextures = 128, .maxSamplers = 128});

  // Vertex buffer, Index buffer and Vertex Input. Buffers are allocated in GPU memory.
  vb0_ = device_->createBuffer({.usage = BufferUsageBits_Vertex,
                                .storage = StorageType_Device,
                                .data = vertexData0,
                                .size = sizeof(vertexData0),
                                .debugName = "Buffer: vertex"},
                               nullptr);
  ib0_ = device_->createBuffer({.usage = BufferUsageBits_Index,
                                .storage = StorageType_Device,
                                .data = indexData,
                                .size = sizeof(indexData),
                                .debugName = "Buffer: index"},
                               nullptr);
  // create an Uniform buffers to store uniforms for 2 objects
  for (uint32_t i = 0; i != kNumBufferedFrames; i++) {
    ubPerFrame_.push_back(device_->createBuffer({.usage = BufferUsageBits_Uniform,
                                                 .storage = StorageType_HostVisible,
                                                 .size = sizeof(UniformsPerFrame),
                                                 .debugName = "Buffer: uniforms (per frame)"},
                                                nullptr));
    ubPerObject_.push_back(device_->createBuffer({.usage = BufferUsageBits_Uniform,
                                                  .storage = StorageType_HostVisible,
                                                  .size = kNumCubes * sizeof(UniformsPerObject),
                                                  .debugName = "Buffer: uniforms (per object)"},
                                                 nullptr));
  }

  depthStencilState_ = {.compareOp = igl::CompareOp_Less, .isDepthWriteEnabled = true};

  {
    const uint32_t texWidth = 256;
    const uint32_t texHeight = 256;
    texture0_ = device_->createTexture(
        {
            .type = TextureType::TwoD,
            .format = igl::TextureFormat::BGRA_UN8,
            .width = texWidth,
            .height = texHeight,
            .usage = igl::TextureUsageBits_Sampled,
            .debugName = "XOR pattern",
        },
        nullptr);
    std::vector<uint32_t> pixels(texWidth * texHeight);
    for (uint32_t y = 0; y != texHeight; y++) {
      for (uint32_t x = 0; x != texWidth; x++) {
        // create a XOR pattern
        pixels[y * texWidth + x] = 0xFF000000 + ((x ^ y) << 16) + ((x ^ y) << 8) + (x ^ y);
      }
    }
    const void* data[] = {pixels.data()};
    texture0_->upload({.width = texWidth, .height = texHeight}, data);
  }
  {
    using namespace std::filesystem;
    path dir = current_path();
    const char* contentFolder = "third-party/content/src/";
    while (dir != current_path().root_path() && !exists(dir / path(contentFolder))) {
      dir = dir.parent_path();
    }
    int32_t texWidth = 0;
    int32_t texHeight = 0;
    int32_t channels = 0;
    uint8_t* pixels = stbi_load(
        (dir / path(contentFolder) / path("bistro/BuildingTextures/wood_polished_01_diff.png"))
            .string()
            .c_str(),
        &texWidth,
        &texHeight,
        &channels,
        4);
    IGL_ASSERT_MSG(pixels,
                   "Cannot load textures. Run `deploy_content.py` before running this app.");
    if (!pixels) {
      printf("Cannot load textures. Run `deploy_content.py` before running this app.");
      std::terminate();
    }
    texture1_ = device_->createTexture(
        {
            .type = TextureType::TwoD,
            .format = igl::TextureFormat::RGBA_UN8,
            .width = (uint32_t)texWidth,
            .height = (uint32_t)texHeight,
            .usage = igl::TextureUsageBits_Sampled,
            .debugName = "wood_polished_01_diff.png",
        },
        nullptr);
    const void* data[] = {pixels};
    texture1_->upload({.width = (uint32_t)texWidth, .height = (uint32_t)texHeight}, data);
    stbi_image_free(pixels);
  }

  sampler_ = device_->createSamplerState({.debugName = "Sampler: linear"}, nullptr);

  renderPass_ = {.colorAttachments = {{
                     .loadOp = LoadOp_Clear,
                     .storeOp = StoreOp_Store,
                     .clearColor = {1.0f, 0.0f, 0.0f, 1.0f},
                 }}};
#if TINY_TEST_USE_DEPTH_BUFFER
  renderPass_.depthAttachment = {.loadOp = LoadOp_Clear, .clearDepth = 1.0};
#else
  renderPass_.depthAttachment = {.loadOp = LoadOp_DontCare};
#endif // TINY_TEST_USE_DEPTH_BUFFER

  // initialize random rotation axes for all cubes
  for (uint32_t i = 0; i != kNumCubes; i++) {
    axis_[i] = glm::sphericalRand(1.0f);
  }
}

static void initObjects() {
  if (renderPipelineState_Mesh_.valid()) {
    return;
  }

  framebuffer_ = {
      .colorAttachments = {{.texture = device_->getCurrentSwapchainTexture()}},
  };

  const VertexInput vdesc = {
      .attributes =
          {{.location = 0, .format = VertexFormat::Float3, .offset = offsetof(VertexPosUvw, pos)},
           {.location = 1, .format = VertexFormat::Float3, .offset = offsetof(VertexPosUvw, color)},
           {.location = 2, .format = VertexFormat::Float2, .offset = offsetof(VertexPosUvw, uv)}},
      .inputBindings = {{.stride = sizeof(VertexPosUvw)}},
  };

  renderPipelineState_Mesh_ = device_->createRenderPipeline(
      {
          .vertexInput = vdesc,
          .shaderStages = device_->createShaderStages(
              codeVS, "Shader Module: main (vert)", codeFS, "Shader Module: main (frag)"),
          .colorAttachments =
              {
                  {.textureFormat = framebuffer_.colorAttachments[0].texture->getFormat()},
              },
          .depthAttachmentFormat = framebuffer_.depthStencilAttachment.texture
                                       ? framebuffer_.depthStencilAttachment.texture->getFormat()
                                       : igl::TextureFormat::Invalid,
          .cullMode = igl::CullMode_Back,
          .frontFaceWinding = igl::WindingMode_CW,
          .debugName = "Pipeline: mesh",
      },
      nullptr);
}

static void render(const std::shared_ptr<ITexture>& nativeDrawable, uint32_t frameIndex) {
  IGL_PROFILER_FUNCTION();

  if (!width_ || !height_) {
    return;
  }

  framebuffer_.colorAttachments[0].texture = nativeDrawable;

  const float fov = float(45.0f * (M_PI / 180.0f));
  const float aspectRatio = (float)width_ / (float)height_;
  const UniformsPerFrame perFrame = {
      .proj = glm::perspectiveLH(fov, aspectRatio, 0.1f, 500.0f),
      // place a "camera" behind the cubes, the distance depends on the total number of cubes
      .view = glm::translate(mat4(1.0f), vec3(0.0f, 0.0f, sqrtf(kNumCubes / 16) * 20.0f * half)),
      .texture0 = texture0_->getTextureId(),
      .texture1 = texture1_ ? texture1_->getTextureId() : 0u,
      .sampler = sampler_->getSamplerId(),
  };
  ubPerFrame_[frameIndex]->upload(&perFrame, sizeof(perFrame));

  // rotate cubes around random axes
  for (uint32_t i = 0; i != kNumCubes; i++) {
    const float direction = powf(-1, (float)(i + 1));
    const uint32_t cubesInLine = (uint32_t)sqrt(kNumCubes);
    const vec3 offset = vec3(-1.5f * sqrt(kNumCubes) + 4.0f * (i % cubesInLine),
                             -1.5f * sqrt(kNumCubes) + 4.0f * (i / cubesInLine),
                             0);
    perObject[i].model =
        glm::rotate(glm::translate(mat4(1.0f), offset), direction * (float)glfwGetTime(), axis_[i]);
  }

  ubPerObject_[frameIndex]->upload(&perObject, sizeof(perObject));

  // Command buffers (1-N per thread): create, submit and forget
  std::shared_ptr<ICommandBuffer> buffer = device_->createCommandBuffer();

  const igl::Viewport viewport = {0.0f, 0.0f, (float)width_, (float)height_, 0.0f, +1.0f};
  const igl::ScissorRect scissor = {0, 0, (uint32_t)width_, (uint32_t)height_};

  // This will clear the framebuffer
  buffer->cmdBeginRendering(renderPass_, framebuffer_);
  {
    buffer->cmdBindRenderPipeline(renderPipelineState_Mesh_);
    buffer->cmdBindViewport(viewport);
    buffer->cmdBindScissorRect(scissor);
    buffer->cmdPushDebugGroupLabel("Render Mesh", igl::Color(1, 0, 0));
    buffer->cmdBindVertexBuffer(0, vb0_, 0);
    buffer->cmdBindDepthStencilState(depthStencilState_);
    // Draw 2 cubes: we use uniform buffer to update matrices
    for (uint32_t i = 0; i != kNumCubes; i++) {
      struct {
        uint64_t perFrame;
        uint64_t perObject;
      } bindings = {
          .perFrame = ubPerFrame_[frameIndex]->gpuAddress(),
          .perObject = ubPerObject_[frameIndex]->gpuAddress(i * sizeof(UniformsPerObject)),
      };
      buffer->cmdPushConstants(bindings);
      buffer->cmdDrawIndexed(PrimitiveType::Triangle, 3 * 6 * 2, IndexFormat::UInt16, *ib0_.get(), 0);
    }
    buffer->cmdPopDebugGroupLabel();
  }
  imgui_->endFrame(*device_.get(), *buffer);
  buffer->cmdEndRendering();

  device_->submit(*buffer, CommandQueueType::Graphics, nativeDrawable.get());
}

int main(int argc, char* argv[]) {
  minilog::initialize(nullptr, {.threadNames = false});

  window_ = lvk::initWindow("Vulkan Mesh", width_, height_);
  initIGL();

  initObjects();

  imgui_ = std::make_unique<lvk::ImGuiRenderer>(*device_);

  glfwSetCursorPosCallback(
      window_, [](auto* window, double x, double y) { ImGui::GetIO().MousePos = ImVec2(x, y); });
  glfwSetMouseButtonCallback(window_, [](auto* window, int button, int action, int mods) {
    double xpos, ypos;
    glfwGetCursorPos(window, &xpos, &ypos);
    const ImGuiMouseButton_ imguiButton =
        (button == GLFW_MOUSE_BUTTON_LEFT)
            ? ImGuiMouseButton_Left
            : (button == GLFW_MOUSE_BUTTON_RIGHT ? ImGuiMouseButton_Right
                                                 : ImGuiMouseButton_Middle);
    ImGuiIO& io = ImGui::GetIO();
    io.MousePos = ImVec2((float)xpos, (float)ypos);
    io.MouseDown[imguiButton] = action == GLFW_PRESS;
  });

  glfwSetWindowSizeCallback(window_, [](GLFWwindow*, int width, int height) {
    width_ = width;
    height_ = height;
    auto* vulkanDevice = static_cast<vulkan::Device*>(device_.get());
    vulkanDevice->getVulkanContext().initSwapchain(width_, height_);
  });

  glfwSetKeyCallback(window_, [](GLFWwindow* window, int key, int, int action, int) {
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
      glfwSetWindowShouldClose(window, GLFW_TRUE);
    }
    if (key == GLFW_KEY_T && action == GLFW_PRESS) {
      texture1_.reset();
    }
  });

  double prevTime = glfwGetTime();

  uint32_t frameIndex = 0;

  // Main loop
  while (!glfwWindowShouldClose(window_)) {
    const double newTime = glfwGetTime();
    fps_.tick(newTime - prevTime);
    prevTime = newTime;
    if (width_ && height_) {
      imgui_->beginFrame(framebuffer_);

      ImGui::Begin("Texture Viewer", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
      ImGui::Image(ImTextureID(texture1_.get()), ImVec2(512, 512));
      ImGui::End();
    }

    render(device_->getCurrentSwapchainTexture(), frameIndex);
    glfwPollEvents();
    frameIndex = (frameIndex + 1) % kNumBufferedFrames;
  }

  imgui_ = nullptr;

  // destroy all the Vulkan stuff before closing the window
  vb0_ = nullptr;
  ib0_ = nullptr;
  ubPerFrame_.clear();
  ubPerObject_.clear();
  renderPipelineState_Mesh_ = nullptr;
  texture0_ = nullptr;
  texture1_ = nullptr;
  sampler_ = nullptr;
  framebuffer_ = {};
  device_.reset(nullptr);

  glfwDestroyWindow(window_);
  glfwTerminate();

  return 0;
}
