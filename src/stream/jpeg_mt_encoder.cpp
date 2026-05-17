// src/stream/jpeg_mt_encoder.cpp
// Adapted from rpicam-apps encoder/mjpeg_encoder.cpp (BSD-2-Clause).

#include "jpeg_mt_encoder.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>

#include <jpeglib.h>

// ── Constructor ────────────────────────────────────────────────────────────

JpegMtEncoder::JpegMtEncoder(uint32_t width, uint32_t height,
                               uint32_t stride, int quality)
    : width_(width), height_(height), stride_(stride), quality_(quality)
{
    // Start encode threads first, then output thread
    for (int i = 0; i < NUM_THREADS; ++i)
        enc_threads_[i] = std::thread(&JpegMtEncoder::encode_thread_fn, this, i);

    out_thread_ = std::thread(&JpegMtEncoder::output_thread_fn, this);

    std::cout << "JpegMtEncoder: started " << NUM_THREADS << " threads "
              << width_ << "x" << height_ << " q=" << quality_ << "\n";
}

// ── Destructor ─────────────────────────────────────────────────────────────

JpegMtEncoder::~JpegMtEncoder()
{
    // 1. Signal encode threads to drain and exit
    {
        std::lock_guard<std::mutex> lock(enc_mutex_);
        abort_enc_ = true;
    }
    enc_cv_.notify_all();
    for (auto& t : enc_threads_) t.join();

    // 2. Signal output thread to drain remaining items and exit
    {
        std::lock_guard<std::mutex> lock(out_mutex_);
        abort_out_ = true;
    }
    out_cv_.notify_all();
    out_thread_.join();

    std::cout << "JpegMtEncoder: stopped\n";
}

// ── encode() ───────────────────────────────────────────────────────────────

bool JpegMtEncoder::encode(FramePtr frame)
{
    std::lock_guard<std::mutex> lock(enc_mutex_);
    if (static_cast<int>(enc_queue_.size()) >= MAX_QUEUE)
        return false;   // queue full — drop frame

    enc_queue_.push({std::move(frame), next_index_++});
    enc_cv_.notify_one();
    return true;
}

// ── encode_thread_fn() ─────────────────────────────────────────────────────

void JpegMtEncoder::encode_thread_fn(int id)
{
    // Persistent jpeg_compress_struct — created once, reused every frame.
    // Mirrors rpicam-apps: no per-frame jpeg_create_compress overhead.
    jpeg_compress_struct cinfo{};
    jpeg_error_mgr       jerr{};
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);

    while (true) {
        EncodeItem item;
        {
            std::unique_lock<std::mutex> lock(enc_mutex_);
            enc_cv_.wait_for(lock, std::chrono::milliseconds(200), [this] {
                return !enc_queue_.empty() || abort_enc_.load();
            });

            if (abort_enc_ && enc_queue_.empty()) {
                jpeg_destroy_compress(&cinfo);
                return;
            }
            if (enc_queue_.empty())
                continue;

            item = std::move(enc_queue_.front());
            enc_queue_.pop();
        }

        // ── Copy frame data, then release DMA buffer immediately ──────────
        // The camera has only 4 DMA buffers. If we hold the FramePtr for the
        // full 57ms encode duration, all 4 buffers are held and the camera
        // stalls → only ~16fps throughput instead of 30fps.
        // Copying Y+UV takes ~5ms; releasing here keeps camera running at 30fps.

        const uint32_t W   = item.frame->width;
        const uint32_t H   = item.frame->height;
        const uint32_t S   = item.frame->stride;
        const uint32_t uvW = W / 2;
        const uint32_t uvH = H / 2;

        const uint8_t* srcY  = item.frame->data;
        const uint8_t* srcUV = item.frame->data + S * H;

        // Y plane: strip stride padding so libjpeg gets tightly packed rows
        std::vector<uint8_t> y_buf(W * H);
        for (uint32_t r = 0; r < H; ++r)
            std::memcpy(y_buf.data() + r * W, srcY + r * S, W);

        // UV de-interleave: NV12 [Cb Cr ...] → separate Cb, Cr planes
        std::vector<uint8_t> cb_buf(uvW * uvH);
        std::vector<uint8_t> cr_buf(uvW * uvH);
        for (uint32_t r = 0; r < uvH; ++r) {
            const uint8_t* row = srcUV + r * S;
            for (uint32_t c = 0; c < uvW; ++c) {
                cb_buf[r * uvW + c] = row[c * 2];
                cr_buf[r * uvW + c] = row[c * 2 + 1];
            }
        }

        // Release DMA buffer — camera can reuse it for the next frame
        item.frame.reset();

        // Encode from heap copies (no DMA buffer held during encoding)
        uint8_t* jpeg_buf  = nullptr;
        size_t   jpeg_size = 0;
        do_encode(cinfo, W, H, y_buf, cb_buf, cr_buf, jpeg_buf, jpeg_size);

        // Push result to per-thread output queue
        std::vector<uint8_t> jpeg_vec(jpeg_buf, jpeg_buf + jpeg_size);
        free(jpeg_buf);

        {
            std::lock_guard<std::mutex> lock(out_mutex_);
            out_queues_[id].push({std::move(jpeg_vec), item.index, id});
        }
        out_cv_.notify_one();
    }
}

// ── do_encode() — libjpeg raw_data_in path ─────────────────────────────────
//
// Uses jpeg_write_raw_data() which accepts pre-subsampled YCbCr row arrays
// directly, bypassing libjpeg's internal colour conversion and subsampling.
// For NV12 input:
//   Y  rows → direct pointer into DMA buffer (no copy)
//   Cb rows → pointer into de-interleaved Cb buffer
//   Cr rows → pointer into de-interleaved Cr buffer
// Matched exactly to rpicam-apps MjpegEncoder::encodeJPEG().

// Takes pre-copied heap buffers (no DMA access during encoding).
// Y is packed (stride already stripped). Cb/Cr are de-interleaved.
void JpegMtEncoder::do_encode(jpeg_compress_struct&        cinfo,
                               uint32_t                    W,
                               uint32_t                    H,
                               const std::vector<uint8_t>& y_buf,
                               const std::vector<uint8_t>& cb_buf,
                               const std::vector<uint8_t>& cr_buf,
                               uint8_t*&                   out_buf,
                               size_t&                     out_size)
{
    const uint32_t uv_w = W / 2;
    const uint32_t uv_h = H / 2;

    // ── libjpeg raw_data_in setup (same as rpicam-apps encodeJPEG) ────────
    cinfo.image_width      = W;
    cinfo.image_height     = H;
    cinfo.input_components = 3;
    cinfo.in_color_space   = JCS_YCbCr;
    cinfo.restart_interval = 0;

    jpeg_set_defaults(&cinfo);
    cinfo.raw_data_in = TRUE;   // skip libjpeg colour conversion — use raw MCU rows
    jpeg_set_quality(&cinfo, quality_, TRUE);

    unsigned char* jpeg_mem = nullptr;
    unsigned long  jpeg_len = 0;
    jpeg_mem_dest(&cinfo, &jpeg_mem, &jpeg_len);
    jpeg_start_compress(&cinfo, TRUE);

    // ── Feed 16-row MCU blocks ────────────────────────────────────────────
    JSAMPROW y_rows[16], cb_rows[8], cr_rows[8];

    const uint8_t* Y_last  = y_buf.data()  + (H   - 1) * W;
    const uint8_t* cb_last = cb_buf.data() + (uv_h - 1) * uv_w;
    const uint8_t* cr_last = cr_buf.data() + (uv_h - 1) * uv_w;

    for (uint32_t row = 0; row < H; row += 16) {
        for (int i = 0; i < 16; ++i) {
            const uint8_t* p = y_buf.data() + std::min(row + (uint32_t)i, H - 1) * W;
            y_rows[i] = const_cast<JSAMPROW>(std::min(p, Y_last));
        }
        for (int i = 0; i < 8; ++i) {
            uint32_t uvr = row / 2 + (uint32_t)i;
            const uint8_t* pc = cb_buf.data() + std::min(uvr, uv_h - 1) * uv_w;
            const uint8_t* pv = cr_buf.data() + std::min(uvr, uv_h - 1) * uv_w;
            cb_rows[i] = const_cast<JSAMPROW>(std::min(pc, cb_last));
            cr_rows[i] = const_cast<JSAMPROW>(std::min(pv, cr_last));
        }
        JSAMPARRAY planes[] = { y_rows, cb_rows, cr_rows };
        jpeg_write_raw_data(&cinfo, planes, 16);
    }

    jpeg_finish_compress(&cinfo);
    out_buf  = jpeg_mem;
    out_size = jpeg_len;
}

// ── output_thread_fn() ─────────────────────────────────────────────────────
//
// Reorders JPEG frames from per-thread queues by `index` and fires
// output_cb_ in sequence. Mirrors rpicam-apps' outputThread().

void JpegMtEncoder::output_thread_fn()
{
    uint64_t expected = 0;   // next index to deliver

    while (true) {
        OutputItem item{};
        bool found = false;

        {
            std::unique_lock<std::mutex> lock(out_mutex_);
            out_cv_.wait_for(lock, std::chrono::milliseconds(200), [&] {
                // Look for the frame with index == expected in any queue
                for (auto& q : out_queues_)
                    if (!q.empty() && q.front().index == expected)
                        return true;
                // Also wake on abort (after all queues drain)
                if (abort_out_) {
                    for (auto& q : out_queues_)
                        if (!q.empty()) return true;
                    return true;
                }
                return false;
            });

            // Check for abort with all queues empty
            if (abort_out_) {
                bool all_empty = true;
                for (auto& q : out_queues_)
                    if (!q.empty()) { all_empty = false; break; }
                if (all_empty) return;
            }

            // Find the queue that has `expected`
            for (auto& q : out_queues_) {
                if (!q.empty() && q.front().index == expected) {
                    item  = std::move(q.front());
                    q.pop();
                    found = true;
                    break;
                }
            }
        }

        if (found) {
            if (output_cb_ && !item.jpeg.empty())
                output_cb_(item.jpeg.data(), item.jpeg.size());
            ++expected;
        }
    }
}
