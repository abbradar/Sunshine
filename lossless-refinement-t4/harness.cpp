// T4 harness: runs Sunshine's NVENC encoder (src/nvenc/nvenc_base.cpp) with the lossless refinement layer
// (src/lossless_refinement.cpp) over CUDA on Linux, on a scripted desktop, and checks the decoded stream.
//
//   harness encode <h264|hevc> <444|420> <slices> <ref_frames> <bitrate_kbps> <packet_size> <out>
//     Encodes the scenario, writes <out>.bit (Annex B, lost frames left out) and <out>.frames.
//   harness check <444|420> <out> < decoded.yuv
//     Reads the decoded frames (rawvideo yuv444p or yuv420p) and checks every block the layer asked to encode
//     losslessly against the source.
#include <dlfcn.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "src/config.h"
#include "src/lossless_refinement.h"
#include "src/nvenc/nvenc_base.h"
#include "src/platform/common.h"

using namespace std::literals;

namespace config {
  // Periodic loggers read the log level; print info and above
  sunshine_t sunshine {.min_log_level = 2};
}  // namespace config

namespace {
  constexpr int width = 1920;
  constexpr int height = 1080;
  constexpr int fps = 60;
  constexpr int end_tick = 1440;
  constexpr int pointer_size = 32;
  constexpr platf::damage_rect_t caret_rect {600, 400, 2, 20};
  constexpr platf::damage_rect_t window_rect {300, 500, 400, 300};
  constexpr platf::damage_rect_t video_rect {1200, 150, 320, 180};

  using clock = std::chrono::steady_clock;
  const auto tick_interval = std::chrono::duration_cast<clock::duration>(std::chrono::nanoseconds {1'000'000'000 / fps});

  struct rng_t {
    std::uint64_t s;

    std::uint64_t next() {
      std::uint64_t z = (s += 0x9e3779b97f4a7c15ULL);
      z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
      z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
      return z ^ (z >> 31);
    }

    std::uint32_t below(std::uint32_t n) {
      return static_cast<std::uint32_t>(next() % n);
    }
  };

  // What the scripted desktop shows; a pure function of the tick
  struct scene_t {
    std::uint32_t background = 1;
    bool noise = false;
    int pointer_x = 100;
    int pointer_y = 100;
    bool caret = false;
    std::uint32_t window = 0;
    std::uint32_t video = 0;

    bool operator==(const scene_t &) const = default;
  };

  scene_t scene_at(int tick) {
    scene_t s;
    s.background = tick < 720 ? 1 : 2;
    s.noise = tick >= 1080;
    // Pointer: diagonal sweep, then occasional moves over the video, then a sweep over noise
    const int sweep = std::clamp(tick - 179, 0, 60);
    s.pointer_x = 100 + 10 * sweep;
    s.pointer_y = 100 + 6 * sweep;
    if (tick >= 480) {
      s.pointer_x += 4 * (std::min(tick, 600) - 480) / 5;
    }
    if (tick >= 1300) {
      s.pointer_x = 900 + 8 * (std::min(tick, 1380) - 1300);
      s.pointer_y = 300;
    }
    s.caret = tick >= 240 && tick < 420 && (tick - 240) / 30 % 2 == 0;
    s.window = tick >= 420 ? 1 : 0;
    s.video = tick < 480 ? 0 : std::min(tick, 600) - 479;
    return s;
  }

  struct yuv_t {
    std::vector<std::uint8_t> y, u, v;  // 4:4:4 planes

    yuv_t():
        y(width * height),
        u(width * height),
        v(width * height) {
    }

    void set(int x, int yy, std::uint8_t ly, std::uint8_t cu, std::uint8_t cv) {
      if (x < 0 || yy < 0 || x >= width || yy >= height) {
        return;
      }
      const auto i = static_cast<std::size_t>(yy) * width + x;
      y[i] = ly;
      u[i] = cu;
      v[i] = cv;
    }

    void fill(const platf::damage_rect_t &r, std::uint8_t ly, std::uint8_t cu, std::uint8_t cv) {
      for (int yy = r.y; yy < r.y + r.height; ++yy) {
        for (int x = r.x; x < r.x + r.width; ++x) {
          set(x, yy, ly, cu, cv);
        }
      }
    }
  };

  void draw_text(yuv_t &img, const platf::damage_rect_t &area, rng_t &rng, std::uint8_t ink) {
    for (int line = area.y + 6; line + 14 <= area.y + area.height - 4; line += 20) {
      const int end = area.x + 8 + static_cast<int>(rng.below(static_cast<std::uint32_t>(area.width - 16)));
      for (int x = area.x + 8; x + 8 <= end; x += 8) {
        if (rng.below(100) < 15) {
          continue;
        }
        for (int gy = 0; gy < 12; ++gy) {
          for (int gx = 0; gx < 6; ++gx) {
            const auto r = rng.below(100);
            if (r < 30) {
              img.set(x + gx, line + gy, ink, 128, 128);
            } else if (r < 38) {
              // Anti-aliased edge
              img.set(x + gx, line + gy, static_cast<std::uint8_t>((ink + 200) / 2), 126, 130);
            }
          }
        }
      }
    }
  }

  void draw_background(yuv_t &img, std::uint32_t seed, bool noise) {
    rng_t rng {seed * 7919ULL};
    if (noise) {
      for (std::size_t i = 0; i < img.y.size(); ++i) {
        const auto r = rng.next();
        img.y[i] = static_cast<std::uint8_t>(r);
        img.u[i] = static_cast<std::uint8_t>(r >> 8);
        img.v[i] = static_cast<std::uint8_t>(r >> 16);
      }
      return;
    }
    img.fill({0, 0, width, height}, 225, 128, 128);
    img.fill({0, 0, width, 40}, 60, 150, 110);
    img.fill({0, height - 48, width, 48}, 50, 132, 124);
    draw_text(img, {20, 50, 1200, height - 110}, rng, 30);
    for (int i = 0; i < 20; ++i) {
      const int x = 1250 + static_cast<int>(rng.below(560));
      const int y = 60 + static_cast<int>(rng.below(480));
      img.fill({x, y, 80, 24}, static_cast<std::uint8_t>(80 + rng.below(140)), static_cast<std::uint8_t>(90 + rng.below(80)), static_cast<std::uint8_t>(90 + rng.below(80)));
    }
    // Gradient and a photo-like area with grain
    for (int y = 600; y < 800; ++y) {
      for (int x = 1300; x < 1800; ++x) {
        img.set(x, y, static_cast<std::uint8_t>(16 + (x - 1300) * 219 / 500), static_cast<std::uint8_t>(64 + (y - 600) * 128 / 200), static_cast<std::uint8_t>(192 - (x - 1300) * 128 / 500));
      }
    }
    const double phase = seed * 0.37;
    for (int y = 820; y < 1020; ++y) {
      for (int x = 1300; x < 1800; ++x) {
        const double s = std::sin(x / 31.0 + phase) * std::cos(y / 23.0 - phase) + 0.5 * std::sin((x + y) / 11.0);
        const auto grain = static_cast<int>(rng.below(7)) - 3;
        img.set(x, y, static_cast<std::uint8_t>(std::clamp(128 + static_cast<int>(s * 70) + grain, 0, 255)), static_cast<std::uint8_t>(118 + static_cast<int>(s * 10)), static_cast<std::uint8_t>(140 - static_cast<int>(s * 12)));
      }
    }
  }

  void draw_pointer(yuv_t &img, int px, int py) {
    // Arrow: inside when the column is within the slanted edge
    const auto inside = [](int c, int r) {
      return r >= 0 && r < pointer_size && c >= 0 && ((r < 22 && c <= r * 2 / 3) || (r >= 18 && c >= 4 && c <= 9 && r - c < 22));
    };
    for (int r = 0; r < pointer_size; ++r) {
      for (int c = 0; c < pointer_size; ++c) {
        if (!inside(c, r)) {
          continue;
        }
        const bool edge = !inside(c - 1, r) || !inside(c + 1, r) || !inside(c, r - 1) || !inside(c, r + 1);
        img.set(px + c, py + r, edge ? 16 : 235, 128, 128);
      }
    }
  }

  yuv_t render(const scene_t &s) {
    static std::map<std::pair<std::uint32_t, bool>, yuv_t> backgrounds;
    auto key = std::pair {s.background, s.noise};
    auto it = backgrounds.find(key);
    if (it == backgrounds.end()) {
      yuv_t bg;
      draw_background(bg, s.background, s.noise);
      it = backgrounds.emplace(key, std::move(bg)).first;
    }
    yuv_t img = it->second;
    if (s.window) {
      img.fill(window_rect, 80, 128, 128);
      img.fill({window_rect.x + 1, window_rect.y + 1, window_rect.width - 2, window_rect.height - 2}, 245, 128, 128);
      rng_t rng {1000 + s.window};
      draw_text(img, {window_rect.x + 1, window_rect.y + 24, window_rect.width - 2, window_rect.height - 26}, rng, 20);
    }
    if (s.video) {
      rng_t rng {5000 + s.video};
      for (int y = 0; y < video_rect.height; ++y) {
        for (int x = 0; x < video_rect.width; ++x) {
          const double v = std::sin((x + 3.0 * s.video) / 17.0) * std::cos((y - 2.0 * s.video) / 23.0);
          img.set(video_rect.x + x, video_rect.y + y, static_cast<std::uint8_t>(std::clamp(128 + static_cast<int>(v * 90) + static_cast<int>(rng.below(9)) - 4, 0, 255)), static_cast<std::uint8_t>(128 + v * 30), static_cast<std::uint8_t>(128 - v * 30));
        }
      }
    }
    if (s.caret) {
      img.fill(caret_rect, 16, 128, 128);
    }
    draw_pointer(img, s.pointer_x, s.pointer_y);
    return img;
  }

  std::optional<std::vector<platf::damage_rect_t>> damage_between(const scene_t &a, const scene_t &b) {
    if (a.background != b.background || a.noise != b.noise) {
      return std::nullopt;
    }
    std::vector<platf::damage_rect_t> damage;
    if (a.pointer_x != b.pointer_x || a.pointer_y != b.pointer_y) {
      damage.push_back({a.pointer_x, a.pointer_y, pointer_size, pointer_size});
      damage.push_back({b.pointer_x, b.pointer_y, pointer_size, pointer_size});
    }
    if (a.caret != b.caret) {
      damage.push_back(caret_rect);
    }
    if (a.window != b.window) {
      damage.push_back(window_rect);
    }
    if (a.video != b.video) {
      damage.push_back(video_rect);
    }
    return damage;
  }

  int max_fec_frame_size(int packet_size, int fec_percentage) {
    // See src/stream.cpp: 4 FEC blocks of 255 shards, 8-byte short frame header, 16-byte packet header
    return 4 * (255 * 100 / (100 + fec_percentage)) * (packet_size - 16) - 8;
  }
}  // namespace

namespace NVENC_NAMESPACE {

  class nvenc_cuda final: public nvenc_base {
  public:
    nvenc_cuda():
        nvenc_base(NV_ENC_DEVICE_TYPE_CUDA) {
    }

    ~nvenc_cuda() override {
      if (encoder) {
        destroy_encoder();
      }
      if (surface) {
        cu.cuMemFree(surface);
      }
      if (context) {
        cu.cuCtxDestroy(context);
      }
    }

    bool init_cuda() {
      cuda_lib = dlopen("libcuda.so.1", RTLD_NOW);
      if (!cuda_lib) {
        std::cerr << "dlopen libcuda: " << dlerror() << '\n';
        return false;
      }
#define LOAD(name, sym) cu.name = reinterpret_cast<decltype(cu.name)>(dlsym(cuda_lib, sym))
      LOAD(cuInit, "cuInit");
      LOAD(cuDeviceGet, "cuDeviceGet");
      LOAD(cuCtxCreate, "cuCtxCreate_v2");
      LOAD(cuCtxDestroy, "cuCtxDestroy_v2");
      LOAD(cuMemAllocPitch, "cuMemAllocPitch_v2");
      LOAD(cuMemFree, "cuMemFree_v2");
      LOAD(cuMemcpy2D, "cuMemcpy2D_v2");
#undef LOAD
      CUdevice dev;
      if (cu.cuInit(0) || cu.cuDeviceGet(&dev, 0) || cu.cuCtxCreate(&context, 0, dev)) {
        std::cerr << "CUDA init failed\n";
        return false;
      }
      device = context;
      return true;
    }

    bool upload(const yuv_t &img) {
      const bool ayuv = encoder_params.buffer_format == NV_ENC_BUFFER_FORMAT_AYUV;
      const std::size_t row_bytes = ayuv ? width * 4 : width;
      const std::size_t rows = ayuv ? height : height * 3 / 2;
      host.resize(row_bytes * rows);
      if (ayuv) {
        for (std::size_t i = 0; i < img.y.size(); ++i) {
          host[i * 4 + 0] = img.v[i];
          host[i * 4 + 1] = img.u[i];
          host[i * 4 + 2] = img.y[i];
          host[i * 4 + 3] = 255;
        }
      } else {
        std::copy(img.y.begin(), img.y.end(), host.begin());
        auto *uv = host.data() + static_cast<std::size_t>(width) * height;
        for (int y = 0; y < height / 2; ++y) {
          for (int x = 0; x < width / 2; ++x) {
            const auto avg = [&](const std::vector<std::uint8_t> &p) {
              const auto i = static_cast<std::size_t>(2 * y) * width + 2 * x;
              return static_cast<std::uint8_t>((p[i] + p[i + 1] + p[i + width] + p[i + width + 1] + 2) / 4);
            };
            uv[static_cast<std::size_t>(y) * width + 2 * x] = avg(img.u);
            uv[static_cast<std::size_t>(y) * width + 2 * x + 1] = avg(img.v);
          }
        }
      }
      CUDA_MEMCPY2D copy {};
      copy.srcMemoryType = CU_MEMORYTYPE_HOST;
      copy.srcHost = host.data();
      copy.srcPitch = row_bytes;
      copy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
      copy.dstDevice = surface;
      copy.dstPitch = pitch;
      copy.WidthInBytes = row_bytes;
      copy.Height = rows;
      return cu.cuMemcpy2D(&copy) == CUDA_SUCCESS;
    }

  private:
    bool init_library() override {
      if (nvenc) {
        return true;
      }
      auto lib = dlopen("libnvidia-encode.so.1", RTLD_NOW);
      if (!lib) {
        std::cerr << "dlopen libnvidia-encode: " << dlerror() << '\n';
        return false;
      }
      auto create_instance = reinterpret_cast<decltype(NvEncodeAPICreateInstance) *>(dlsym(lib, "NvEncodeAPICreateInstance"));
      auto list = std::make_shared<NV_ENCODE_API_FUNCTION_LIST>();
      list->version = NV_ENCODE_API_FUNCTION_LIST_VER;
      if (!create_instance || nvenc_failed(create_instance(list.get()))) {
        return false;
      }
      nvenc = list;
      return true;
    }

    bool create_and_register_input_buffer() override {
      const bool ayuv = encoder_params.buffer_format == NV_ENC_BUFFER_FORMAT_AYUV;
      if (!surface && cu.cuMemAllocPitch(&surface, &pitch, ayuv ? width * 4 : width, ayuv ? height : height * 3 / 2, 16)) {
        return false;
      }
      if (!registered_input_buffer) {
        NV_ENC_REGISTER_RESOURCE reg = {.version = NV_ENC_REGISTER_RESOURCE_VER};
        reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;
        reg.width = encoder_params.width;
        reg.height = encoder_params.height;
        reg.pitch = static_cast<std::uint32_t>(pitch);
        reg.resourceToRegister = reinterpret_cast<void *>(surface);
        reg.bufferFormat = encoder_params.buffer_format;
        reg.bufferUsage = NV_ENC_INPUT_IMAGE;
        if (nvenc_failed(nvenc->nvEncRegisterResource(encoder, &reg))) {
          std::cerr << "nvEncRegisterResource: " << last_nvenc_error_string << '\n';
          return false;
        }
        registered_input_buffer = reg.registeredResource;
      }
      return true;
    }

    struct {
      tcuInit *cuInit;
      tcuDeviceGet *cuDeviceGet;
      tcuCtxCreate_v2 *cuCtxCreate;
      tcuCtxDestroy_v2 *cuCtxDestroy;
      tcuMemAllocPitch_v2 *cuMemAllocPitch;
      tcuMemFree_v2 *cuMemFree;
      tcuMemcpy2D_v2 *cuMemcpy2D;
    } cu {};

    void *cuda_lib = nullptr;
    CUcontext context = nullptr;
    CUdeviceptr surface = 0;
    std::size_t pitch = 0;
    std::vector<std::uint8_t> host;
  };

}  // namespace NVENC_NAMESPACE

namespace {

  int encode(int argc, char **argv) {
    if (argc != 9) {
      std::cerr << "usage: harness encode <h264|hevc> <444|420> <slices> <ref_frames> <bitrate_kbps> <packet_size> <out>\n";
      return 2;
    }
    const bool hevc = argv[2] == "hevc"sv;
    const bool yuv444 = argv[3] == "444"sv;
    const std::string out = argv[8];
    const int fec_limit = max_fec_frame_size(std::stoi(argv[7]), 20);

    video::config_t client {};
    client.width = width;
    client.height = height;
    client.framerate = fps;
    client.bitrate = std::stoi(argv[6]);
    client.slicesPerFrame = std::stoi(argv[4]);
    client.numRefFrames = std::stoi(argv[5]);
    client.videoFormat = hevc ? 1 : 0;
    client.chromaSamplingType = yuv444 ? 1 : 0;
    client.maxFecFrameSize = fec_limit;

    nvenc::nvenc_config config;
    // LOSSLESS=0 runs the same scenario without refinement, to compare streams
    config.lossless_refinement = !std::getenv("LOSSLESS") || std::getenv("LOSSLESS") != "0"sv;
    if (const char *preset = std::getenv("NVENC_PRESET")) {
      config.quality_preset = std::stoi(preset);
    }
    // Sunshine's other NVENC options
    config.adaptive_quantization = std::getenv("AQ") != nullptr;
    config.h264_cavlc = std::getenv("CAVLC") != nullptr;
    config.weighted_prediction = std::getenv("WP") != nullptr;
    config.enable_min_qp = std::getenv("MIN_QP") != nullptr;
    if (std::getenv("FULL_TWO_PASS")) {
      config.two_pass = nvenc::nvenc_two_pass::full_resolution;
    }

    NVENC_NAMESPACE::nvenc_cuda enc;
    if (!enc.init_cuda()) {
      return 1;
    }
    const video::sunshine_colorspace_t colorspace {video::colorspace_e::rec709, false, 8};
    if (!enc.create_encoder(config, client, colorspace, yuv444 ? platf::pix_fmt_e::ayuv : platf::pix_fmt_e::nv12)) {
      std::cerr << "create_encoder failed\n";
      return 1;
    }
    const auto block_size = enc.lossless_block_size();
    std::printf("lossless block size %u, FEC limit %d bytes\n", block_size, fec_limit);
    if (block_size == 0 && config.lossless_refinement) {
      return 1;
    }

    const auto t0 = clock::time_point {} + 1h;
    const auto frame_interval = std::chrono::duration_cast<clock::duration>(video::capture_frame_interval(client));
    const auto max_wait = std::chrono::duration_cast<clock::duration>(2 * frame_interval);
    video::lossless_refinement_t layer {
      {width, height, width, height, block_size, yuv444, static_cast<std::uint64_t>(fec_limit)},
      100ms,
      frame_interval,
      client.bitrate,
      t0,
    };
    std::printf("bands %u\n", layer.band_count());

    std::ofstream bit(out + ".bit", std::ios::binary);
    std::ofstream full_bit(out + ".full.bit", std::ios::binary);
    std::ofstream frames(out + ".frames");
    frames << "B " << block_size << ' ' << layer.band_count() << ' ' << fec_limit << '\n';

    // Scripted network events
    const auto at = [&](int tick) {
      return t0 + tick * tick_interval;
    };
    struct loss_t {
      clock::time_point time;
      int frames;
      bool started = false;
    };
    std::vector<loss_t> losses {{at(760), 3}, {at(1340), 2}};
    std::optional<clock::time_point> idr_request = at(900);
    int lost_remaining = 0;
    std::int64_t last_idr = 0;
    std::int64_t first_lost = 0;

    std::optional<scene_t> shown;
    std::uint64_t sequence = 0;
    int next_tick = 0;
    auto now = t0;
    std::int64_t frame_index = 0;
    std::int64_t written = 0;
    bool force_idr = false;
    int frames_at_same_time = 0;
    auto last_time = now;

    std::size_t max_lossless_size = 0, max_size = 0, over_fec = 0, band_frames = 0;
    double max_band_ratio = 0, max_noise_ratio = 0;

    while (true) {
      // The next capture is the next tick that shows something new
      int capture_tick = next_tick;
      while (capture_tick < end_tick && shown && scene_at(capture_tick) == *shown) {
        ++capture_tick;
      }
      const auto wake = now + layer.wait_time(now, max_wait);
      if (capture_tick < end_tick && at(capture_tick) <= wake) {
        now = std::max(now, at(capture_tick));
        next_tick = capture_tick + 1;
        const auto scene = scene_at(capture_tick);
        platf::img_t img;
        img.capture_sequence = ++sequence;
        if (shown) {
          img.damage = damage_between(*shown, scene);
        }
        layer.image_captured(img, now);
        shown = scene;
        if (!enc.upload(render(scene))) {
          std::cerr << "upload failed\n";
          return 1;
        }
      } else if (capture_tick >= end_tick && wake >= at(end_tick)) {
        break;
      } else {
        now = wake;
      }

      if (now == last_time) {
        if (++frames_at_same_time > 3) {
          std::cerr << "layer keeps asking for frames at the same time\n";
          return 1;
        }
      } else {
        frames_at_same_time = 0;
        last_time = now;
      }

      if (idr_request && now >= *idr_request) {
        idr_request.reset();
        force_idr = true;
        layer.frames_lost(now);
      }
      // LOSS_FRAME moves the first loss to a frame index
      static const std::int64_t loss_frame = std::getenv("LOSS_FRAME") ? std::stoll(std::getenv("LOSS_FRAME")) : -1;
      if (loss_frame >= 0) {
        losses.front().time = frame_index == loss_frame ? now : clock::time_point::max();
      }
      // FFmpeg can't decode H.264 after losing frames that include frame_num 0, with or without refinement,
      // so losses avoid them unless LOSS_FRAME is set
      const auto frame_num = (frame_index - last_idr) % 256;
      const bool across_wrap = !hevc && loss_frame < 0 && (frame_num == 0 || frame_num > 256 - 4);
      for (auto &loss : losses) {
        if (!loss.started && now >= loss.time && !across_wrap) {
          loss.started = true;
          lost_remaining = loss.frames;
          first_lost = frame_index;
        }
      }

      const auto lossless = layer.next_frame(now, frame_index);
      auto encoded = enc.encode_frame(static_cast<std::uint64_t>(frame_index), std::exchange(force_idr, false), lossless.blocks);
      if (encoded.data.empty()) {
        std::cerr << "encode_frame failed at frame " << frame_index << '\n';
        return 1;
      }
      layer.frame_encoded(encoded.data.size());
      if (encoded.idr) {
        last_idr = frame_index;
      }

      const auto size = encoded.data.size();
      full_bit.write(reinterpret_cast<const char *>(encoded.data.data()), static_cast<std::streamsize>(size));
      max_size = std::max(max_size, size);
      const bool has_lossless_area = lossless.refined_band || lossless.blocks.size() > 0;
      if (lossless.refined_band || (lossless.blocks.size() > 0 && size > max_lossless_size)) {
        max_lossless_size = std::max(max_lossless_size, size);
      }
      if (has_lossless_area && size > static_cast<std::size_t>(fec_limit)) {
        ++over_fec;
      }
      if (lossless.refined_band) {
        ++band_frames;
        const auto [first_row, last_row] = video::lossless_band_rows(video::lossless_block_count(height, block_size), *lossless.refined_band, layer.band_count());
        const auto rows = std::min<std::uint64_t>(last_row * block_size, height) - first_row * block_size;
        const double raw = static_cast<double>(rows) * width * (yuv444 ? 3 : 1.5);
        max_band_ratio = std::max(max_band_ratio, size / raw);
        if (shown && shown->noise) {
          max_noise_ratio = std::max(max_noise_ratio, size / raw);
        }
      }

      const auto ms = std::chrono::duration_cast<std::chrono::microseconds>(now - t0).count() / 1000.0;
      if (lost_remaining > 0) {
        frames << "L " << frame_index << ' ' << ms << ' ' << size << '\n';
        if (--lost_remaining == 0) {
          const auto result = enc.invalidate_ref_frames(static_cast<std::uint64_t>(first_lost), static_cast<std::uint64_t>(frame_index));
          frames << "I " << first_lost << ' ' << frame_index << ' ' << static_cast<int>(result) << '\n';
          if (result == video::ref_frames_invalidation_e::invalidated) {
            layer.frames_invalidated(first_lost, now);
          } else if (result == video::ref_frames_invalidation_e::idr) {
            force_idr = true;
            layer.frames_lost(now);
          }
        }
      } else {
        bit.write(reinterpret_cast<const char *>(encoded.data.data()), static_cast<std::streamsize>(size));
        const auto &s = *shown;
        frames << "F " << written++ << ' ' << frame_index << ' ' << ms << ' ' << size << ' ' << encoded.idr << ' '
               << (lossless.refined_band ? static_cast<int>(*lossless.refined_band) : -1) << ' '
               << s.background << ' ' << s.noise << ' ' << s.pointer_x << ' ' << s.pointer_y << ' ' << s.caret << ' ' << s.window << ' ' << s.video << ' '
               << lossless.blocks.size();
        for (const auto &r : lossless.blocks) {
          frames << ' ' << r.x << ' ' << r.y << ' ' << r.width << ' ' << r.height;
        }
        frames << '\n';
      }
      ++frame_index;
    }

    std::printf("frames %lld (written %lld), band frames %zu, max frame %zu B, max lossless frame %zu B, lossless frames over FEC limit %zu\n", static_cast<long long>(frame_index), static_cast<long long>(written), band_frames, max_size, max_lossless_size, over_fec);
    std::printf("max band size / raw size %.3f, on noise %.3f\n", max_band_ratio, max_noise_ratio);
    return 0;
  }

  int check(int argc, char **argv) {
    if (argc != 4) {
      std::cerr << "usage: harness check <444|420> <out> < decoded.yuv\n";
      return 2;
    }
    const bool yuv444 = argv[2] == "444"sv;
    std::ifstream frames(std::string {argv[3]} + ".frames");
    const int cw = yuv444 ? width : width / 2;
    const int ch = yuv444 ? height : height / 2;
    const std::size_t luma = static_cast<std::size_t>(width) * height;
    const std::size_t chroma = static_cast<std::size_t>(cw) * ch;
    std::vector<std::uint8_t> dec(luma + 2 * chroma);

    std::uint32_t block_size = 0;
    std::string line;
    std::int64_t checked = 0, exact_rects = 0, bad_rects = 0, bad_frames = 0, full_exact = 0, decoded = 0, lossless_frames = 0;
    std::optional<scene_t> cached_scene;
    yuv_t src;
    std::vector<std::uint8_t> src_u, src_v;
    int reported = 0;
    while (std::getline(frames, line)) {
      if (line.starts_with("B ")) {
        block_size = static_cast<std::uint32_t>(std::stoul(line.substr(2)));
      }
      if (line.empty() || line[0] != 'F') {
        continue;
      }
      std::istringstream in(line.substr(2));
      std::int64_t index, frame_index, size;
      double ms;
      int idr, band, noise, caret;
      scene_t s;
      std::size_t rect_count;
      in >> index >> frame_index >> ms >> size >> idr >> band >> s.background >> noise >> s.pointer_x >> s.pointer_y >> caret >> s.window >> s.video >> rect_count;
      s.noise = noise;
      s.caret = caret;
      std::vector<video::lossless_block_rect_t> rects(rect_count);
      for (auto &r : rects) {
        in >> r.x >> r.y >> r.width >> r.height;
      }
      if (!std::cin.read(reinterpret_cast<char *>(dec.data()), static_cast<std::streamsize>(dec.size()))) {
        std::printf("decoded stream ended after %lld frames, expected more\n", static_cast<long long>(decoded));
        return 1;
      }
      ++decoded;
      if (!cached_scene || !(*cached_scene == s)) {
        src = render(s);
        cached_scene = s;
        if (!yuv444) {
          src_u.assign(chroma, 0);
          src_v.assign(chroma, 0);
          for (int y = 0; y < ch; ++y) {
            for (int x = 0; x < cw; ++x) {
              const auto i = static_cast<std::size_t>(2 * y) * width + 2 * x;
              src_u[static_cast<std::size_t>(y) * cw + x] = static_cast<std::uint8_t>((src.u[i] + src.u[i + 1] + src.u[i + width] + src.u[i + width + 1] + 2) / 4);
              src_v[static_cast<std::size_t>(y) * cw + x] = static_cast<std::uint8_t>((src.v[i] + src.v[i + 1] + src.v[i + width] + src.v[i + width + 1] + 2) / 4);
            }
          }
        }
      }
      const auto &su = yuv444 ? src.u : src_u;
      const auto &sv = yuv444 ? src.v : src_v;
      bool frame_bad = false;
      if (!rects.empty()) {
        ++lossless_frames;
      }
      for (const auto &r : rects) {
        const int x0 = static_cast<int>(r.x * block_size), y0 = static_cast<int>(r.y * block_size);
        const int x1 = std::min<int>(static_cast<int>((r.x + r.width) * block_size), width);
        const int y1 = std::min<int>(static_cast<int>((r.y + r.height) * block_size), height);
        int max_diff = 0;
        std::int64_t diffs = 0;
        const auto cmp = [&](const std::uint8_t *a, const std::uint8_t *b, int stride, int ax0, int ay0, int ax1, int ay1) {
          for (int y = ay0; y < ay1; ++y) {
            for (int x = ax0; x < ax1; ++x) {
              const int d = std::abs(a[static_cast<std::size_t>(y) * stride + x] - b[static_cast<std::size_t>(y) * stride + x]);
              if (d) {
                ++diffs;
                max_diff = std::max(max_diff, d);
              }
            }
          }
        };
        cmp(src.y.data(), dec.data(), width, x0, y0, x1, y1);
        const int sx = yuv444 ? 1 : 2;
        cmp(su.data(), dec.data() + luma, cw, x0 / sx, y0 / sx, (x1 + sx - 1) / sx, (y1 + sx - 1) / sx);
        cmp(sv.data(), dec.data() + luma + chroma, cw, x0 / sx, y0 / sx, (x1 + sx - 1) / sx, (y1 + sx - 1) / sx);
        ++checked;
        if (diffs) {
          ++bad_rects;
          frame_bad = true;
          if (reported++ < 20) {
            std::printf("MISMATCH frame %lld (encoder frame %lld, %.1f ms, band %d, idr %d): blocks %u,%u %ux%u: %lld samples differ, max %d\n", static_cast<long long>(index), static_cast<long long>(frame_index), ms, band, idr, r.x, r.y, r.width, r.height, static_cast<long long>(diffs), max_diff);
          }
        } else {
          ++exact_rects;
        }
      }
      if (frame_bad) {
        ++bad_frames;
      }
      if (std::equal(src.y.begin(), src.y.end(), dec.begin())) {
        ++full_exact;
      }
    }
    char extra;
    const bool trailing = static_cast<bool>(std::cin.read(&extra, 1));
    std::printf("%lld frames decoded, %lld with lossless areas, %lld rects checked: %lld exact, %lld mismatched in %lld frames; %lld frames with exact luma everywhere%s\n", static_cast<long long>(decoded), static_cast<long long>(lossless_frames), static_cast<long long>(checked), static_cast<long long>(exact_rects), static_cast<long long>(bad_rects), static_cast<long long>(bad_frames), static_cast<long long>(full_exact), trailing ? "; decoder produced extra frames" : "");
    return bad_rects || trailing ? 1 : 0;
  }
}  // namespace

int main(int argc, char **argv) {
  if (argc > 1 && argv[1] == "encode"sv) {
    return encode(argc, argv);
  }
  if (argc > 1 && argv[1] == "check"sv) {
    return check(argc, argv);
  }
  std::cerr << "usage: harness encode|check ...\n";
  return 2;
}
