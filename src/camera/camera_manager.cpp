// src/camera/camera_manager.cpp
// Steps 3a–3f of the Camera Manager implementation plan.

#include "camera_manager.h"

#include <iostream>
#include <sys/mman.h> // mmap, munmap, PROT_READ, PROT_WRITE, MAP_SHARED

#include <libcamera/formats.h> // libcamera::formats::NV12

// NOTE: libcamera::CameraManager conflicts in name with our own CameraManager
// class, so we do NOT use "using namespace libcamera;" here.
// All libcamera types are referenced with the explicit libcamera:: prefix.

// ── Constructor / Destructor ───────────────────────────────────────────────

CameraManager::CameraManager(CameraFrameQueue &queue)
    : queue_(queue)
{
}

CameraManager::~CameraManager()
{
    if (running_)
        close();
}

// ── open() — orchestrates steps 3a → 3f ───────────────────────────────────

bool CameraManager::open(Resolution res)
{
    resolution_ = res;
    const ResolutionConfig &cfg = config_of(res);

    if (!init_camera_manager())
        return false;
    if (!acquire_camera())
    {
        cam_manager_->stop();
        return false;
    }
    if (!configure_stream(cfg))
    {
        release_camera();
        cam_manager_->stop();
        return false;
    }
    if (!allocate_and_map_buffers())
    {
        release_camera();
        cam_manager_->stop();
        return false;
    }
    if (!create_requests())
    {
        unmap_buffers();
        release_camera();
        cam_manager_->stop();
        return false;
    }

    // Connect the completion signal before starting so no frame is missed.
    camera_->requestCompleted.connect(this, &CameraManager::on_request_completed);

    start_streaming();
    running_ = true;

    std::cout << "CameraManager: streaming at " << cfg.label
              << " (NV12, stride=" << stride_ << ")\n";
    return true;
}

// ── close() ────────────────────────────────────────────────────────────────

void CameraManager::close()
{
    if (!running_)
        return;

    // Signal requeue_request() to stop re-queuing before stopping the camera.
    running_ = false;

    stop_streaming(); // blocks until all in-flight requests are cancelled

    camera_->requestCompleted.disconnect(this, &CameraManager::on_request_completed);

    // Destroy requests before the allocator (which owns the underlying buffers).
    requests_.clear();

    // NOTE: if any consumer still holds a shared_ptr<Frame>, its frame->data
    // pointer will become dangling after unmap_buffers(). Callers must ensure
    // all consumers have dropped their frames before calling close().
    unmap_buffers();
    allocator_.reset();

    release_camera();
    cam_manager_->stop();
}

// ── set_resolution() — tear down and restart ──────────────────────────────

bool CameraManager::set_resolution(Resolution res)
{
    close();
    return open(res);
}

// ═══════════════════════════════════════════════════════════════════════════
// Step 3a — Initialise the libcamera subsystem
// ═══════════════════════════════════════════════════════════════════════════

bool CameraManager::init_camera_manager()
{
    cam_manager_ = std::make_unique<libcamera::CameraManager>();

    int ret = cam_manager_->start();
    if (ret != 0)
    {
        std::cerr << "CameraManager: libcamera CameraManager::start() failed: "
                  << ret << "\n";
        cam_manager_.reset();
        return false;
    }
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// Step 3a — Acquire the first available camera (Pi Camera v1 = index 0)
// ═══════════════════════════════════════════════════════════════════════════

bool CameraManager::acquire_camera()
{
    const auto &cameras = cam_manager_->cameras();
    if (cameras.empty())
    {
        std::cerr << "CameraManager: no cameras detected on this system\n";
        return false;
    }

    // grab a shared_ptr from the manager by ID so lifetime is correct
    camera_ = cam_manager_->get(cameras[0]->id());
    if (!camera_)
    {
        std::cerr << "CameraManager: failed to retrieve camera by id\n";
        return false;
    }

    if (camera_->acquire() != 0)
    {
        std::cerr << "CameraManager: failed to acquire exclusive access to camera\n";
        camera_.reset();
        return false;
    }

    std::cout << "CameraManager: acquired camera [" << camera_->id() << "]\n";
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// Step 3b — Configure the stream
//
// generateConfiguration() returns a configuration seeded with hardware
// defaults for the requested role.  We override size, format, and buffer
// count, then call validate() which may adjust values the hardware cannot
// satisfy exactly. configure() programs the hardware.
// ═══════════════════════════════════════════════════════════════════════════

bool CameraManager::configure_stream(const ResolutionConfig &cfg)
{
    cam_config_ = camera_->generateConfiguration(
        {libcamera::StreamRole::VideoRecording});

    if (!cam_config_)
    {
        std::cerr << "CameraManager: generateConfiguration() failed\n";
        return false;
    }

    libcamera::StreamConfiguration &sc = cam_config_->at(0);
    sc.size = {cfg.width, cfg.height};
    sc.pixelFormat = libcamera::formats::NV12;
    sc.bufferCount = 4; // 4 DMA buffers in flight — low latency, no starvation

    libcamera::CameraConfiguration::Status status = cam_config_->validate();
    if (status == libcamera::CameraConfiguration::Invalid)
    {
        std::cerr << "CameraManager: configuration is invalid even after validate()\n";
        return false;
    }
    if (status == libcamera::CameraConfiguration::Adjusted)
    {
        // Hardware adjusted size or format — report the actual values
        std::cout << "CameraManager: configuration adjusted to "
                  << sc.size.width << "x" << sc.size.height
                  << " fmt=" << sc.pixelFormat << "\n";
    }

    if (camera_->configure(cam_config_.get()) != 0)
    {
        std::cerr << "CameraManager: camera->configure() failed\n";
        return false;
    }

    stream_ = sc.stream();
    stride_ = sc.stride; // actual bytes per row set by the driver after configure()

    std::cout << "CameraManager: stream configured "
              << sc.size.width << "x" << sc.size.height
              << " stride=" << stride_
              << " buffers=" << sc.bufferCount << "\n";
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// Step 3c — Allocate DMA buffers and mmap them
//
// The FrameBufferAllocator asks the kernel to allocate DMA memory for each
// buffer slot. We then mmap each buffer into process address space so that
// on_request_completed() can hand a raw pointer to consumers without any copy.
//
// NV12 layout: plane[0] = Y  (stride * height bytes)
//              plane[1] = UV (stride * height/2 bytes)
// On the Pi, both planes share the same DMA fd and are contiguous in memory —
// we mmap the first plane's fd once for the total size.
// ═══════════════════════════════════════════════════════════════════════════

bool CameraManager::allocate_and_map_buffers()
{
    allocator_ = std::make_unique<libcamera::FrameBufferAllocator>(camera_);

    if (allocator_->allocate(stream_) < 0)
    {
        std::cerr << "CameraManager: FrameBufferAllocator::allocate() failed\n";
        return false;
    }

    for (const auto &buffer : allocator_->buffers(stream_))
    {
        // Sum plane lengths to get the total DMA buffer size
        size_t total = 0;
        for (const auto &plane : buffer->planes())
            total += plane.length;

        // mmap the contiguous DMA region from the first plane's file descriptor
        int fd = buffer->planes()[0].fd.get();
        void *ptr = mmap(nullptr, total,
                         PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (ptr == MAP_FAILED)
        {
            std::cerr << "CameraManager: mmap failed for DMA buffer (fd=" << fd << ")\n";
            unmap_buffers();
            return false;
        }

        mapped_[buffer.get()] = {static_cast<uint8_t *>(ptr), total};
    }

    std::cout << "CameraManager: mapped " << mapped_.size() << " DMA buffers\n";
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// Step 3d — Create one Request per buffer
//
// Each Request wraps one DMA buffer. When submitted to the camera, the
// hardware fills the buffer and fires requestCompleted. The Request is then
// re-used (Request::ReuseBuffers) so it can be queued again without
// re-adding the buffer.
// ═══════════════════════════════════════════════════════════════════════════

bool CameraManager::create_requests()
{
    for (const auto &buffer : allocator_->buffers(stream_))
    {
        auto request = camera_->createRequest();
        if (!request)
        {
            std::cerr << "CameraManager: createRequest() failed\n";
            return false;
        }

        if (request->addBuffer(stream_, buffer.get()) < 0)
        {
            std::cerr << "CameraManager: request->addBuffer() failed\n";
            return false;
        }

        requests_.push_back(std::move(request));
    }

    std::cout << "CameraManager: created " << requests_.size() << " requests\n";
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// Step 3f — Start / stop streaming
// ═══════════════════════════════════════════════════════════════════════════

void CameraManager::start_streaming()
{
    camera_->start();
    for (auto &req : requests_)
        camera_->queueRequest(req.get());
}

void CameraManager::stop_streaming()
{
    // camera_->stop() blocks until all in-flight requests are cancelled
    // and requestCompleted has been called for each with RequestCancelled.
    camera_->stop();
}

// ═══════════════════════════════════════════════════════════════════════════
// Step 3e — requestCompleted callback
//
// Fired on libcamera's internal event thread for every captured frame.
// We build a Frame that points directly into the DMA buffer (zero copy),
// attach a release() lambda that re-queues the Request, and push the
// shared_ptr into the FrameQueue.
//
// If the queue is full (downstream too slow), the frame is dropped and the
// buffer is immediately returned to the camera.
// ═══════════════════════════════════════════════════════════════════════════

void CameraManager::on_request_completed(libcamera::Request *request)
{
    // Requests cancelled during stop() must not be re-queued
    if (request->status() == libcamera::Request::RequestCancelled)
        return;

    auto *buf = request->buffers().at(stream_);

    auto it = mapped_.find(buf);
    if (it == mapped_.end())
    {
        std::cerr << "CameraManager: completed buffer not in mmap table — dropping\n";
        requeue_request(request);
        return;
    }

    const libcamera::FrameMetadata &meta = buf->metadata();
    const MappedBuffer &mb = it->second;

    auto frame = std::make_shared<Frame>();
    frame->data = mb.ptr;
    frame->data_size = mb.length;
    frame->width = config_of(resolution_).width;
    frame->height = config_of(resolution_).height;
    frame->stride = stride_;
    frame->format = PixelFormat::NV12;
    frame->sequence = meta.sequence;

    // libcamera timestamp is nanoseconds from CLOCK_MONOTONIC
    frame->timestamp = std::chrono::steady_clock::time_point(
        std::chrono::nanoseconds(meta.timestamp));

    // Expose the DMA-BUF fd so the Recorder can pass it directly to
    // v4l2h264enc via V4L2_MEMORY_DMABUF — true zero-copy hardware encoding.
    // The fd is owned by the FrameBufferAllocator and remains valid until close().
    frame->dma_fd = buf->planes()[0].fd.get();

    // release() is invoked when the last shared_ptr<Frame> is destroyed.
    // It returns the DMA buffer to the camera for the next capture cycle.
    frame->release = [this, request]()
    {
        requeue_request(request);
    };

    // Phase C snapshot: if armed, copy the shared_ptr into still_queue_
    // BEFORE moving it into the video queue so both hold a reference.
    // Phase D: this block is bypassed once the real still stream is active.
    if (snapshot_armed_.exchange(false, std::memory_order_acq_rel))
    {
        FramePtr still = frame; // shared_ptr copy — refcount +1
        still->stream_type = StreamType::Still;
        still_queue_.push(std::move(still));
    }

    if (!queue_.push(std::move(frame)))
    {
        // Queue full — drop this frame and immediately recycle the buffer
        requeue_request(request);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Snapshot trigger — Phase C implementation
// ═══════════════════════════════════════════════════════════════════════════

bool CameraManager::request_snapshot()
{
    if (!running_)
        return false;
    // Arm the flag. on_request_completed() will copy the next video frame
    // into still_queue_ (same DMA pointer, refcount bumped by 1).
    // In Phase D this flag is not used; the still stream fires independently.
    bool expected = false;
    return snapshot_armed_.compare_exchange_strong(expected, true,
                                                   std::memory_order_release);
}

FramePtr CameraManager::try_get_snapshot()
{
    return still_queue_.pop();
}

bool CameraManager::set_still_resolution(StillResolution res)
{
    still_res_ = res;
    // Phase D: if the still stream is active, reconfigure it here.
    // Phase C: no-op — the video frame is always at the video resolution.
    return true;
}

// ── Phase D helper stubs ───────────────────────────────────────────────────
// Bodies are intentionally minimal. Phase D replaces these with full
// implementations mirroring configure_stream() / allocate_and_map_buffers()
// / create_requests() but for the StillCapture role.

bool CameraManager::configure_still_stream(StillResolution /*res*/)
{
    // TODO Phase D: append StreamRole::StillCapture to cam_config_,
    //               set still_stream_ = cam_config_->at(1).stream().
    return false;
}

bool CameraManager::allocate_and_map_still_buffers()
{
    // TODO Phase D: allocate 1 DMA buffer for the still stream,
    //               mmap it into still_mapped_.
    return false;
}

bool CameraManager::create_still_request()
{
    // TODO Phase D: create one libcamera::Request, add the still buffer,
    //               push into still_requests_.
    return false;
}

void CameraManager::handle_still_completed(libcamera::Request * /*request*/)
{
    // TODO Phase D: mirror on_request_completed() for the still stream.
    //               Build a Frame with stream_type = StreamType::Still,
    //               push into still_queue_, do NOT re-queue the request
    //               (still requests are single-shot).
}

// ── Re-queue a completed request for the next capture ─────────────────────

void CameraManager::requeue_request(libcamera::Request *request)
{
    // Guard against requeueing after close() has stopped the camera.
    // The running_ flag is set to false before camera_->stop(), so any
    // release() lambda fired after close() returns will safely no-op here.
    if (!running_)
        return;

    request->reuse(libcamera::Request::ReuseBuffers);
    camera_->queueRequest(request);
}

// ── Cleanup helpers ────────────────────────────────────────────────────────

void CameraManager::unmap_buffers()
{
    for (auto &[buf, mb] : mapped_)
    {
        if (mb.ptr)
            munmap(mb.ptr, mb.length);
    }
    mapped_.clear();
}

void CameraManager::release_camera()
{
    if (camera_)
    {
        camera_->release();
        camera_.reset();
    }
}
