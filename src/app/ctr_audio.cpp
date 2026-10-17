// Sound on the 3DS: the platform side of WinVita's DirectSound emulation
// (ds_emul). Supplies the two hooks it leaves to a console port
// (audio_sink.h): a realtime sink, here the DSP through libctru's ndsp, and
// the thread that runs the mixer, here on the New 3DS's second application
// core (or the system core), so mixing never costs the game's core.
//
// The sink is self-paced: write() blocks until the DSP has finished one of
// its buffers, so the DSP's clock paces the mixer. Four buffers of one grain
// (512 frames, 23 ms at 22050 Hz) keep ~70-90 ms queued.
//
// ndsp needs the DSP firmware at sdmc:/3ds/dspfirm.cdc (dumped once with the
// DSP1 homebrew on a console; Azahar's HLE DSP does not need it). Without it
// the sink fails to open and ds_emul falls back to silence, saying so.
#include "runtime/audio_sink.h"
#include "runtime/rt_host.h"

#include <3ds.h>

#include <cstring>

namespace d2rt {
namespace audio {
namespace {

constexpr int kBuffers = 4;

class NdspSink : public Sink {
public:
    bool open(int freq, int channels, int grain) override
    {
        if (channels != 2 || grain <= 0)
            return false;
        const Result rc = ndspInit();
        if (R_FAILED(rc)) {
            rt::logf("sound: ndspInit failed (%08lx): is sdmc:/3ds/dspfirm.cdc there?", (unsigned long)rc);
            return false;
        }
        grain_ = grain;
        const size_t bytes = (size_t)grain * 2 * sizeof(int16_t);
        pcm_ = (int16_t *)linearAlloc(bytes * kBuffers);
        if (!pcm_) {
            ndspExit();
            return false;
        }
        std::memset(pcm_, 0, bytes * kBuffers);
        ndspSetOutputMode(NDSP_OUTPUT_STEREO);
        ndspChnReset(0);
        ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
        ndspChnSetRate(0, (float)freq);
        ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
        float mix[12] = {1.0f, 1.0f};   // front left/right at full volume
        ndspChnSetMix(0, mix);
        std::memset(bufs_, 0, sizeof bufs_);
        for (int i = 0; i < kBuffers; i++) {
            bufs_[i].data_pcm16 = pcm_ + (size_t)i * grain * 2;
            bufs_[i].nsamples = (u32)grain;
            bufs_[i].status = NDSP_WBUF_DONE;   // free
        }
        rt::logf("sound: ndsp at %d Hz, %d x %d frames", freq, kBuffers, grain);
        open_ = true;
        return true;
    }

    void write(const int16_t *interleaved, int frames) override
    {
        if (!open_ || frames <= 0)
            return;
        if (frames > grain_)
            frames = grain_;
        // The DSP's pace: wait for the next buffer to come back.
        ndspWaveBuf &b = bufs_[next_];
        while (b.status != NDSP_WBUF_DONE && b.status != NDSP_WBUF_FREE)
            svcSleepThread(2 * 1000 * 1000);
        std::memcpy(b.data_pcm16, interleaved, (size_t)frames * 2 * sizeof(int16_t));
        b.nsamples = (u32)frames;
        DSP_FlushDataCache(b.data_pcm16, (u32)frames * 2 * sizeof(int16_t));
        ndspChnWaveBufAdd(0, &b);
        next_ = (next_ + 1) % kBuffers;
    }

    void close() override
    {
        if (!open_)
            return;
        open_ = false;
        ndspChnWaveBufClear(0);
        ndspExit();
        linearFree(pcm_);
        pcm_ = nullptr;
    }

    const char *name() const override { return "ndsp"; }
    bool self_paced() const override { return true; }

    int rest_samples() override
    {
        int queued = 0;
        for (const ndspWaveBuf &b : bufs_)
            if (b.status == NDSP_WBUF_QUEUED || b.status == NDSP_WBUF_PLAYING)
                queued += (int)b.nsamples;
        return queued;
    }

private:
    bool open_ = false;
    int grain_ = 0, next_ = 0;
    int16_t *pcm_ = nullptr;
    ndspWaveBuf bufs_[kBuffers];
};

Thread g_thread = nullptr;
void (*g_body)(void) = nullptr;

void thread_entry(void *) { g_body(); }

}  // namespace

Sink *make_vita_sink() { return new NdspSink(); }   // ds_emul's name for "the console's realtime sink"

bool thread_start(void (*body)(void))
{
    if (g_thread)
        return false;
    g_body = body;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    // Above the game and the frame drawing: a late grain is an audible gap.
    for (int core : {2, 1}) {
        if (core == 1 && R_FAILED(APT_SetAppCpuTimeLimit(30)))
            continue;
        g_thread = threadCreate(thread_entry, nullptr, 32 * 1024, prio - 2, core, false);
        if (g_thread) {
            rt::logf("sound: mixer thread on core %d", core);
            return true;
        }
    }
    rt::logf("sound: no core for the mixer thread");
    return false;
}

bool thread_retry(void) { return false; }

bool thread_stop(void)
{
    if (!g_thread)
        return true;
    threadJoin(g_thread, U64_MAX);   // ds_emul has cleared its run flag before calling this
    threadFree(g_thread);
    g_thread = nullptr;
    return true;
}

}  // namespace audio
}  // namespace d2rt
