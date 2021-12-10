#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <VapourSynth.h>
#include <VSHelper.h>

#include <cuda_runtime.h>
#include <NvInferRuntime.h>
#ifdef USE_NVINFER_PLUGIN
#include <NvInferPlugin.h>
#endif

#include "inference_helper.h"
#include "trt_utils.h"
#include "utils.h"

using namespace std::string_literals;

struct TicketSemaphore {
    std::atomic<intptr_t> ticket {};
    std::atomic<intptr_t> current {};

    void init(intptr_t num) noexcept {
        current.store(num, std::memory_order::seq_cst);
    }

    void acquire() noexcept {
        intptr_t tk { ticket.fetch_add(1, std::memory_order::acquire) };
        while (true) {
            intptr_t curr { current.load(std::memory_order::acquire) };
            if (tk < curr) {
                return;
            }
            current.wait(curr, std::memory_order::relaxed);
        }
    }

    void release() noexcept {
        current.fetch_add(1, std::memory_order::release);
        current.notify_all();
    }
};

struct vsTrtData {
    std::vector<VSNodeRef *> nodes;
    std::unique_ptr<VSVideoInfo> out_vi;

    int device_id;
    int num_streams;
    bool use_cuda_graph;
    int pad;

    Logger logger;
    std::unique_ptr<nvinfer1::IRuntime> runtime;
    std::unique_ptr<nvinfer1::ICudaEngine> engine;

    TicketSemaphore semaphore;
    std::vector<int> tickets;
    std::mutex instances_lock;
    std::vector<InferenceInstance> instances;

    [[nodiscard]]
    int acquire() noexcept {
        semaphore.acquire();
        int ticket;
        {
            std::lock_guard<std::mutex> lock { instances_lock };
            ticket = tickets.back();
            tickets.pop_back();
        }
        return ticket;
    }

    void release(int ticket) noexcept {
        {
            std::lock_guard<std::mutex> lock { instances_lock };
            tickets.push_back(ticket);
        }
        semaphore.release();
    }
};

static void VS_CC vsTrtInit(
    VSMap *in,
    VSMap *out,
    void **instanceData,
    VSNode *node,
    VSCore *core,
    const VSAPI *vsapi
) noexcept {

    auto d = static_cast<vsTrtData *>(*instanceData);
    vsapi->setVideoInfo(d->out_vi.get(), 1, node);
}

static const VSFrameRef *VS_CC vsTrtGetFrame(
    int n,
    int activationReason,
    void **instanceData,
    void **frameData,
    VSFrameContext *frameCtx,
    VSCore *core,
    const VSAPI *vsapi
) noexcept {

    auto d = static_cast<vsTrtData *>(*instanceData);

    if (activationReason == arInitial) {
        for (const auto & node : d->nodes) {
            vsapi->requestFrameFilter(n, node, frameCtx);
        }
    } else if (activationReason == arAllFramesReady) {
        const std::vector<const VSVideoInfo *> in_vis {
            getVideoInfo(vsapi, d->nodes)
        };

        const std::vector<const VSFrameRef *> src_frames {
            getFrames(n, vsapi, frameCtx, d->nodes)
        };

        const int src_planes { d->engine->getBindingDimensions(0).d[1] };

        std::vector<const uint8_t *> src_ptrs;
        src_ptrs.reserve(src_planes);
        for (int i = 0; i < std::size(d->nodes); ++i) {
            for (int j = 0; j < in_vis[i]->format->numPlanes; ++j) {
                src_ptrs.emplace_back(vsapi->getReadPtr(src_frames[i], j));
            }
        }

        VSFrameRef * const dst_frame { vsapi->newVideoFrame(
            d->out_vi->format, d->out_vi->width, d->out_vi->height,
            src_frames[0], core
        )};

        const int dst_planes { d->engine->getBindingDimensions(1).d[1] };
        std::vector<uint8_t *> dst_ptrs;
        dst_ptrs.reserve(dst_planes);
        for (int i = 0; i < dst_planes; ++i) {
            dst_ptrs.emplace_back(vsapi->getWritePtr(dst_frame, i));
        }

        const int ticket { d->acquire() };
        InferenceInstance & instance { d->instances[ticket] };

        const nvinfer1::Dims src_dim { instance.exec_context->getBindingDimensions(0) };
        const int src_patch_h { src_dim.d[2] };
        const int src_patch_w { src_dim.d[3] };

        const nvinfer1::Dims dst_dim { instance.exec_context->getBindingDimensions(1) };
        const int dst_patch_h { dst_dim.d[2] };
        const int dst_patch_w { dst_dim.d[3] };

        const int h_scale = dst_patch_h / src_patch_h;
        const int w_scale = dst_patch_w / src_patch_w;

        const IOInfo info {
            .in = InputInfo {
                .width = vsapi->getFrameWidth(src_frames[0], 0),
                .height = vsapi->getFrameHeight(src_frames[0], 0),
                .pitch = vsapi->getStride(src_frames[0], 0),
                .bytes_per_sample = vsapi->getFrameFormat(src_frames[0])->bytesPerSample,
                .patch_w = src_patch_w,
                .patch_h = src_patch_h
            },
            .out = OutputInfo {
                .pitch = vsapi->getStride(dst_frame, 0),
                .bytes_per_sample = vsapi->getFrameFormat(dst_frame)->bytesPerSample
            },
            .w_scale = w_scale,
            .h_scale = h_scale,
            .pad = d->pad
        };

        const auto inference_result = inference(
            instance,
            d->device_id, d->use_cuda_graph,
            info, src_ptrs, dst_ptrs
        );

        d->release(ticket);

        for (const auto & frame : src_frames) {
            vsapi->freeFrame(frame);
        }

        if (inference_result.has_value()) {
            vsapi->setFilterError(
                (__func__ + ": "s + inference_result.value()).c_str(),
                frameCtx
            );

            vsapi->freeFrame(dst_frame);

            return nullptr;
        }

        return dst_frame;
    }

    return nullptr;
}

static void VS_CC vsTrtFree(
    void *instanceData, VSCore *core, const VSAPI *vsapi
) noexcept {

    auto d = static_cast<vsTrtData *>(instanceData);

    for (const auto & node : d->nodes) {
        vsapi->freeNode(node);
    }

    cudaSetDevice(d->device_id);

    delete d;
}

static void VS_CC vsTrtCreate(
    const VSMap *in, VSMap *out, void *userData,
    VSCore *core, const VSAPI *vsapi
) noexcept {

    auto d { std::make_unique<vsTrtData>() };

    int num_nodes = vsapi->propNumElements(in, "clips");
    d->nodes.reserve(num_nodes);
    for (int i = 0; i < num_nodes; ++i) {
        d->nodes.emplace_back(vsapi->propGetNode(in, "clips", i, nullptr));
    }

    auto set_error = [&](const std::string & error_message) {
        vsapi->setError(out, (__func__ + ": "s + error_message).c_str());
        for (const auto & node : d->nodes) {
            vsapi->freeNode(node);
        }
    };

    const char * engine_path = vsapi->propGetData(in, "engine_path", 0, nullptr);

    std::vector<const VSVideoInfo *> in_vis;
    in_vis.reserve(std::size(d->nodes));
    for (const auto & node : d->nodes) {
        in_vis.emplace_back(vsapi->getVideoInfo(node));
    }
    if (auto err = checkNodes(in_vis); err.has_value()) {
        return set_error(err.value());
    }

    int error;

    d->pad = int64ToIntS(vsapi->propGetInt(in, "pad", 0, &error));
    if (error) {
        d->pad = 0;
    }
    if (d->pad < 0) {
        return set_error("\"pad\" should be non-negative");
    }

    int error1, error2;
    int block_w = int64ToIntS(vsapi->propGetInt(in, "block_w", 0, &error1));
    int block_h = int64ToIntS(vsapi->propGetInt(in, "block_h", 0, &error2));

    BlockSize block_size;
    if (!error1) { // manual specification triggered
        if (error2) {
            block_h = block_w;
        }

        if (block_w - 2 * d->pad <= 0 || block_h - 2 * d->pad <= 0) {
            return set_error("\"pad\" too large");
        }

        block_size = RequestedBlockSize {
            .block_w = block_w,
            .block_h = block_h
        };
    } else {
        if (d->pad != 0) {
            return set_error("\"block_w\" must be specified");
        }

        int width = in_vis[0]->width;
        int height = in_vis[0]->height;

        if (width - 2 * d->pad <= 0 || height - 2 * d->pad <= 0) {
            return set_error("\"pad\" too large");
        }

        block_size = VideoSize {
            .width = width,
            .height = height
        };
    }

    int device_id = int64ToIntS(vsapi->propGetInt(in, "device_id", 0, &error));
    if (error) {
        device_id = 0;
    }

    int device_count;
    checkError(cudaGetDeviceCount(&device_count));
    if (0 <= device_id && device_id < device_count) {
        checkError(cudaSetDevice(device_id));
    } else {
        return set_error("invalid device ID (" + std::to_string(device_id) + ")");
    }
    d->device_id = device_id;

    d->use_cuda_graph = !!vsapi->propGetInt(in, "use_cuda_graph", 0, &error);
    if (error) {
        d->use_cuda_graph = false;
    }

    d->num_streams = int64ToIntS(vsapi->propGetInt(in, "num_streams", 0, &error));
    if (error) {
        d->num_streams = 1;
    }

    int verbosity = int64ToIntS(vsapi->propGetInt(in, "verbosity", 0, &error));
    if (error) {
        verbosity = int(nvinfer1::ILogger::Severity::kWARNING);
    }
    d->logger.set_verbosity(static_cast<nvinfer1::ILogger::Severity>(verbosity));

#ifdef USE_NVINFER_PLUGIN
    if (!initLibNvInferPlugins(&d->logger, "")) {
        vsapi->logMessage(mtWarning, "vsTrt: Initialize TensorRT plugins failed");
    }
#endif

    d->runtime.reset(nvinfer1::createInferRuntime(d->logger));
    auto maybe_engine = initEngine(engine_path, d->runtime);
    if (std::holds_alternative<std::unique_ptr<nvinfer1::ICudaEngine>>(maybe_engine)) {
        d->engine = std::move(std::get<std::unique_ptr<nvinfer1::ICudaEngine>>(maybe_engine));
    } else {
        return set_error(std::get<ErrorMessage>(maybe_engine));
    }

    auto maybe_profile_index = selectProfile(d->engine, block_size);

    d->instances.reserve(d->num_streams);
    for (int i = 0; i < d->num_streams; ++i) {
        auto maybe_resource = getResource(
            d->engine,
            maybe_profile_index,
            block_size,
            d->use_cuda_graph
        );

        if (std::holds_alternative<InferenceInstance>(maybe_resource)) {
            d->instances.emplace_back(std::move(std::get<InferenceInstance>(maybe_resource)));
        } else {
            return set_error(std::get<ErrorMessage>(maybe_resource));
        }
    }

    d->semaphore.init(d->num_streams);
    d->tickets.reserve(d->num_streams);
    for (int i = 0; i < d->num_streams; ++i) {
        d->tickets.push_back(i);
    }

    d->out_vi = std::make_unique<VSVideoInfo>(*in_vis[0]);
    setDimensions(d->out_vi, d->instances[0].exec_context);

    vsapi->createFilter(
        in, out, "Model",
        vsTrtInit, vsTrtGetFrame, vsTrtFree,
        fmParallel, 0, d.release(), core
    );
}

VS_EXTERNAL_API(void) VapourSynthPluginInit(
    VSConfigPlugin configFunc,
    VSRegisterFunction registerFunc,
    VSPlugin *plugin
) noexcept {

    configFunc(
        "io.github.amusementclub.vs_tensorrt", "trt",
        "TensorRT ML Filter Runtime",
        VAPOURSYNTH_API_VERSION, 1, plugin
    );

    registerFunc("Model",
        "clips:clip[];"
        "engine_path:data;"
        "pad:int:opt;"
        "block_w:int:opt;"
        "block_h:int:opt;"
        "device_id:int:opt;"
        "use_cuda_graph:int:opt;"
        "num_streams:int:opt;"
        "verbosity:int:opt;",
        vsTrtCreate,
        nullptr,
        plugin
    );
}