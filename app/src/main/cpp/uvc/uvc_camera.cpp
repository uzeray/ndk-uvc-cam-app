// uvc_camera_impl.cpp

#include "uvc_camera.h"
#include "../common/logging.h"
#include "../common/time_utils.h"

#include <fcntl.h>
#include <linux/v4l2-controls.h>

#include <android/native_window_jni.h>
#include <android/native_window.h>

#include <linux/videodev2.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <poll.h>
#include <errno.h>

#include <cstdlib>
#include <cstdio>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstring>
#include <string>
#include <sstream>
#include <algorithm>
#include <condition_variable>
#include <dlfcn.h>

#ifndef V4L2_CID_JPEG_COMPRESSION_QUALITY
#define V4L2_CID_JPEG_COMPRESSION_QUALITY 0x009f090d
#endif

// Bazı NDK/header kombinasyonlarında eksik olabiliyor -> güvenli fallback
#ifndef V4L2_CID_AUTOGAIN
#define V4L2_CID_AUTOGAIN 0x00980913
#endif

#ifndef V4L2_CID_POWER_LINE_FREQUENCY
#define V4L2_CID_POWER_LINE_FREQUENCY 0x00980918
#endif

#ifndef V4L2_EXPOSURE_AUTO
#define V4L2_EXPOSURE_AUTO 0
#endif

// Power line frequency enum değerleri bazı header'larda farklı isimlenebiliyor.
// Güvenli fallback:
#ifndef V4L2_CID_POWER_LINE_FREQUENCY_DISABLED
#define V4L2_CID_POWER_LINE_FREQUENCY_DISABLED 0
#endif
#ifndef V4L2_CID_POWER_LINE_FREQUENCY_50HZ
#define V4L2_CID_POWER_LINE_FREQUENCY_50HZ 1
#endif
#ifndef V4L2_CID_POWER_LINE_FREQUENCY_60HZ
#define V4L2_CID_POWER_LINE_FREQUENCY_60HZ 2
#endif
#ifndef V4L2_CID_POWER_LINE_FREQUENCY_AUTO
#define V4L2_CID_POWER_LINE_FREQUENCY_AUTO 3
#endif

// MJPG decode
#include "../third_party/stb_image_impl.h"

namespace uvc {

    static std::mutex gLock;
    static std::string gLastError;

    static std::atomic<long long> gLastFrameTsNs{0};
    static std::atomic<long long> gPrevFrameTsNs{0};
    static std::atomic<int> gFpsX100{0};
    static std::atomic<int> gChosenFps{0};

    static std::atomic<uint32_t> gChosenFourcc{0};
    static std::atomic<int> gChosenW{0};
    static std::atomic<int> gChosenH{0};

    struct UvcBuf {
        void *ptr = nullptr;
        size_t len = 0;
    };

    static int gFd = -1;
    static std::vector<UvcBuf> gBufs;
    static ANativeWindow *gWin = nullptr;

    static std::atomic<bool> gRunning{false};
    static std::thread gCaptureThread;
    static std::thread gDecodeThread;

    static int gW = 0;
    static int gH = 0;

// --- MJPEG latest-frame mailbox (capture hızlı QBUF için) ---
    static std::mutex gFrameLock;
    static std::condition_variable gFrameCv;
    static std::vector<uint8_t> gLatestJpeg;
    static std::atomic<bool> gHasNewFrame{false};

    static void setErrLocked(const std::string &s) { gLastError = s; }

    static void clearErrLocked() { gLastError.clear(); }

    static void trySetFrameRate(ANativeWindow *win, float fps, int32_t compatibility) {
        if (!win) return;

        void *h = dlopen("libandroid.so", RTLD_NOW);
        if (!h) return;

        using Fn = int32_t (*)(ANativeWindow *, float, int32_t);
        auto fn = reinterpret_cast<Fn>(dlsym(h, "ANativeWindow_setFrameRate"));
        if (fn) {
            int32_t r = fn(win, fps, compatibility);
            ALOGI("UVC ANativeWindow_setFrameRate(%.2f) -> %d", fps, r);
        }
        dlclose(h);
    }

    static int xioctl(int fd, unsigned long req, void *arg) {
        int r;
        do { r = ioctl(fd, req, arg); } while (r == -1 && errno == EINTR);
        return r;
    }

    static bool trySetCtrl(int fd, __u32 id, int value) {
        v4l2_control c{};
        c.id = id;
        c.value = value;
        return (ioctl(fd, VIDIOC_S_CTRL, &c) == 0);
    }

    static void trySetJpegQualityMax(int fd) {
        v4l2_queryctrl qc{};
        qc.id = V4L2_CID_JPEG_COMPRESSION_QUALITY;

        if (ioctl(fd, VIDIOC_QUERYCTRL, &qc) == 0) {
            int maxQ = qc.maximum;
            (void) trySetCtrl(fd, V4L2_CID_JPEG_COMPRESSION_QUALITY, maxQ);
        }
    }

    static std::string fourccToStr(uint32_t f) {
        char s[5];
        s[0] = (char) (f & 0xFF);
        s[1] = (char) ((f >> 8) & 0xFF);
        s[2] = (char) ((f >> 16) & 0xFF);
        s[3] = (char) ((f >> 24) & 0xFF);
        s[4] = 0;
        return std::string(s);
    }

    static void logLarge(const std::string &text) {
        const size_t CHUNK = 900;
        for (size_t i = 0; i < text.size(); i += CHUNK) {
            std::string part = text.substr(i, CHUNK);
            ALOGI("%s", part.c_str());
        }
    }

    static bool queryCtrl(int fd, __u32 id, v4l2_queryctrl &qc) {
        std::memset(&qc, 0, sizeof(qc));
        qc.id = id;
        if (xioctl(fd, VIDIOC_QUERYCTRL, &qc) != 0) return false;
        if (qc.flags & V4L2_CTRL_FLAG_DISABLED) return false;
        return true;
    }

    static bool getCtrl(int fd, __u32 id, int &outVal) {
        v4l2_control c{};
        c.id = id;
        if (xioctl(fd, VIDIOC_G_CTRL, &c) != 0) return false;
        outVal = c.value;
        return true;
    }

    static bool setCtrl(int fd, __u32 id, int val) {
        v4l2_control c{};
        c.id = id;
        c.value = val;
        return xioctl(fd, VIDIOC_S_CTRL, &c) == 0;
    }

    static void dumpControlsLocked(int fd) {
        std::ostringstream os;
        os << "UVC CTRLS:\n";

        struct CtrlItem {
            __u32 id;
            const char *name;
        };
        CtrlItem list[] = {
                {V4L2_CID_EXPOSURE_AUTO,             "EXPOSURE_AUTO"},
                {V4L2_CID_EXPOSURE_AUTO_PRIORITY,    "EXPOSURE_AUTO_PRIORITY"},
                {V4L2_CID_EXPOSURE_ABSOLUTE,         "EXPOSURE_ABSOLUTE"},
                {V4L2_CID_GAIN,                      "GAIN"},
                {V4L2_CID_BRIGHTNESS,                "BRIGHTNESS"},
                {V4L2_CID_CONTRAST,                  "CONTRAST"},
                {V4L2_CID_SATURATION,                "SATURATION"},
                {V4L2_CID_SHARPNESS,                 "SHARPNESS"},
                {V4L2_CID_GAMMA,                     "GAMMA"},
                {V4L2_CID_AUTO_WHITE_BALANCE,        "AUTO_WB"},
                {V4L2_CID_WHITE_BALANCE_TEMPERATURE, "WB_TEMP"},
                {V4L2_CID_POWER_LINE_FREQUENCY,      "POWER_LINE_FREQ"},
                {V4L2_CID_AUTOGAIN,                  "AUTOGAIN"},
        };

        for (auto &it: list) {
            v4l2_queryctrl qc{};
            if (!queryCtrl(fd, it.id, qc)) continue;

            int cur = 0;
            bool okCur = getCtrl(fd, it.id, cur);

            os << "  " << it.name
               << " range[" << qc.minimum << ".." << qc.maximum << "]"
               << " step=" << qc.step
               << " def=" << qc.default_value;

            if (okCur) os << " cur=" << cur;
            os << "\n";
        }

        logLarge(os.str());
    }

    static void dumpModesLocked(int fd) {
        std::ostringstream os;
        os << "UVC MODES BEGIN\n";

        v4l2_fmtdesc f{};
        std::memset(&f, 0, sizeof(f));
        f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

        for (f.index = 0; xioctl(fd, VIDIOC_ENUM_FMT, &f) == 0; f.index++) {
            uint32_t fourcc = f.pixelformat;
            os << "FMT " << fourccToStr(fourcc) << " (" << (char *) f.description << ")\n";

            v4l2_frmsizeenum fs{};
            std::memset(&fs, 0, sizeof(fs));
            fs.pixel_format = fourcc;

            for (fs.index = 0; xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fs) == 0; fs.index++) {
                if (fs.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
                    int w = (int) fs.discrete.width;
                    int h = (int) fs.discrete.height;

                    v4l2_frmivalenum fi{};
                    std::memset(&fi, 0, sizeof(fi));
                    fi.pixel_format = fourcc;
                    fi.width = (uint32_t) w;
                    fi.height = (uint32_t) h;

                    os << "  " << w << "x" << h << " fps:";
                    bool any = false;

                    for (fi.index = 0;
                         xioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &fi) == 0; fi.index++) {
                        if (fi.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
                            int num = (int) fi.discrete.numerator;
                            int den = (int) fi.discrete.denominator;
                            if (num > 0 && den > 0) {
                                int fps = den / num;
                                os << " " << fps;
                                any = true;
                            }
                        } else {
                            os << " (non-discrete)";
                            any = true;
                            break;
                        }
                    }
                    if (!any) os << " (unknown)";
                    os << "\n";
                } else {
                    os << "  (non-discrete framesize)\n";
                }
            }
        }

        os << "UVC MODES END\n";
        logLarge(os.str());
    }

// ------------------- IMAGE TUNING (BRIGHTNESS/EXPOSURE/GAIN) -------------------

    static int valFromPct(const v4l2_queryctrl &qc, int pct) {
        pct = std::clamp(pct, 0, 100);
        long long range = (long long) qc.maximum - (long long) qc.minimum;
        long long v = (long long) qc.minimum + (range * pct) / 100LL;
        v = std::clamp(v, (long long) qc.minimum, (long long) qc.maximum);

        if (qc.step > 1) {
            long long base = qc.minimum;
            v = base + ((v - base) / qc.step) * qc.step;
            v = std::clamp(v, (long long) qc.minimum, (long long) qc.maximum);
        }
        return (int) v;
    }

    static bool setCtrlIfSupported(int fd, __u32 id, int val) {
        v4l2_queryctrl qc{};
        if (!queryCtrl(fd, id, qc)) return false;
        return setCtrl(fd, id, val);
    }

    static bool setCtrlPctIfSupported(int fd, __u32 id, int pct) {
        v4l2_queryctrl qc{};
        if (!queryCtrl(fd, id, qc)) return false;
        int v = valFromPct(qc, pct);
        return setCtrl(fd, id, v);
    }

// setupLocked içinde çağrıldığı için BURADA (önceden) tanımlı olmalı
    static void resetTouchedControlsToDefaultsLocked(int fd) {
        auto reset = [&](uint32_t id) {
            v4l2_queryctrl qc{};
            if (!queryCtrl(fd, id, qc)) return;
            (void) setCtrl(fd, id, (int) qc.default_value);
        };

        reset(V4L2_CID_EXPOSURE_AUTO);
        reset(V4L2_CID_EXPOSURE_AUTO_PRIORITY);
        reset(V4L2_CID_EXPOSURE_ABSOLUTE);

        reset(V4L2_CID_AUTOGAIN);
        reset(V4L2_CID_GAIN);

        reset(V4L2_CID_BRIGHTNESS);
        reset(V4L2_CID_CONTRAST);
        reset(V4L2_CID_SATURATION);
        reset(V4L2_CID_GAMMA);
        reset(V4L2_CID_SHARPNESS);

        reset(V4L2_CID_AUTO_WHITE_BALANCE);
        reset(V4L2_CID_WHITE_BALANCE_TEMPERATURE);

        reset(V4L2_CID_POWER_LINE_FREQUENCY);
    }

    static void tuneImageControlsLocked(int fd, int /*desiredFps*/) {
        v4l2_queryctrl qc{};

        // 50Hz flicker azaltma (CH/EU)
        if (queryCtrl(fd, V4L2_CID_POWER_LINE_FREQUENCY, qc)) {
            if (!setCtrlIfSupported(fd, V4L2_CID_POWER_LINE_FREQUENCY,
                                    V4L2_CID_POWER_LINE_FREQUENCY_AUTO)) {
                (void) setCtrlIfSupported(fd, V4L2_CID_POWER_LINE_FREQUENCY,
                                          V4L2_CID_POWER_LINE_FREQUENCY_50HZ);
            }
        }

        // FPS düşürmesin: Auto exposure priority kapalı
        if (queryCtrl(fd, V4L2_CID_EXPOSURE_AUTO_PRIORITY, qc)) {
            (void) setCtrlIfSupported(fd, V4L2_CID_EXPOSURE_AUTO_PRIORITY, 0);
        }

        // Exposure AUTO
        if (queryCtrl(fd, V4L2_CID_EXPOSURE_AUTO, qc)) {
            (void) setCtrlIfSupported(fd, V4L2_CID_EXPOSURE_AUTO, V4L2_EXPOSURE_AUTO);
        }

        // Auto gain
        if (queryCtrl(fd, V4L2_CID_AUTOGAIN, qc)) {
            (void) setCtrlIfSupported(fd, V4L2_CID_AUTOGAIN, 1);
        }

        // Auto WB
        (void) setCtrlIfSupported(fd, V4L2_CID_AUTO_WHITE_BALANCE, 1);

        // JPEG quality max
        trySetJpegQualityMax(fd);

        // Eğer bazı cihazlarda auto çok patlatıyorsa, bu iki satırı AÇIP sabitleyebilirsin:
        // (void)setCtrlPctIfSupported(fd, V4L2_CID_BRIGHTNESS, 45);
        // (void)setCtrlPctIfSupported(fd, V4L2_CID_GAMMA, 50);
    }

// ------------------------------------------------------------------------------

    static bool trySetFormatLocked(int fd, int w, int h, uint32_t fourcc, v4l2_format &outFmt) {
        v4l2_format fmt{};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = (uint32_t) w;
        fmt.fmt.pix.height = (uint32_t) h;
        fmt.fmt.pix.pixelformat = fourcc;
        fmt.fmt.pix.field = V4L2_FIELD_ANY;

        if (xioctl(fd, VIDIOC_S_FMT, &fmt) != 0) return false;
        outFmt = fmt;
        return true;
    }

    static bool v4l2IsCaptureNode(int fd, v4l2_capability *outCap) {
        v4l2_capability cap{};
        if (xioctl(fd, VIDIOC_QUERYCAP, &cap) != 0) return false;
        if (outCap) *outCap = cap;

        if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) return false;
        if (!(cap.capabilities & V4L2_CAP_STREAMING)) return false;
        return true;
    }

    static int openBestNodeLocked(std::string &dbg) {
        int fallbackFd = -1;
        v4l2_capability fallbackCap{};

        for (int i = 0; i < 64; i++) {
            char path[64];
            std::snprintf(path, sizeof(path), "/dev/video%d", i);

            int fd = open(path, O_RDWR | O_NONBLOCK);
            if (fd < 0) {
                int e = errno;
                if (e == ENOENT) continue;
                dbg += std::string(path) + ": open errno=" + std::to_string(e) + " (" +
                       std::strerror(e) + ")\n";
                continue;
            }

            v4l2_capability cap{};
            if (!v4l2IsCaptureNode(fd, &cap)) {
                close(fd);
                continue;
            }

            bool isUvc = (std::strncmp((const char *) cap.driver, "uvcvideo", 7) == 0);
            if (isUvc) {
                // LEAK FIX: daha önce tuttuğumuz fallback fd varsa kapat
                if (fallbackFd >= 0) {
                    close(fallbackFd);
                    fallbackFd = -1;
                }
                dbg += std::string("SELECT ") + path + " driver=" + (const char *) cap.driver +
                       " card=" + (const char *) cap.card + "\n";
                return fd;
            }

            if (fallbackFd < 0) {
                fallbackFd = fd;
                fallbackCap = cap;
            } else {
                close(fd);
            }
        }

        if (fallbackFd >= 0) {
            dbg += std::string("FALLBACK driver=") + (const char *) fallbackCap.driver +
                   " card=" + (const char *) fallbackCap.card + "\n";
        }
        return fallbackFd;
    }

    static void tearDownLocked() {
        if (gFd >= 0) {
            v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            (void) xioctl(gFd, VIDIOC_STREAMOFF, &type);
        }

        for (auto &b: gBufs) {
            if (b.ptr && b.len) munmap(b.ptr, b.len);
        }
        gBufs.clear();

        if (gFd >= 0) {
            close(gFd);
            gFd = -1;
        }

        if (gWin) {
            ANativeWindow_release(gWin);
            gWin = nullptr;
        }

        {
            std::lock_guard<std::mutex> lk(gFrameLock);
            gLatestJpeg.clear();
            gHasNewFrame.store(false, std::memory_order_relaxed);
        }

        gLastFrameTsNs.store(0, std::memory_order_relaxed);
        gPrevFrameTsNs.store(0, std::memory_order_relaxed);
        gFpsX100.store(0, std::memory_order_relaxed);
        gChosenFps.store(0, std::memory_order_relaxed);
        gChosenFourcc.store(0, std::memory_order_relaxed);
        gChosenW.store(0, std::memory_order_relaxed);
        gChosenH.store(0, std::memory_order_relaxed);
        gW = 0;
        gH = 0;
    }

    static void renderRgbaToWindow(const uint8_t *rgba, int w, int h) {
        if (!gWin) return;

        ANativeWindow_Buffer out{};
        if (ANativeWindow_lock(gWin, &out, nullptr) != 0) return;

        uint8_t *dst = (uint8_t *) out.bits;
        int dstStrideBytes = out.stride * 4;
        int srcStrideBytes = w * 4;

        int copyH = std::min(h, out.height);
        int copyWBytes = std::min(w, out.width) * 4;

        for (int y = 0; y < copyH; y++) {
            std::memcpy(dst + y * dstStrideBytes, rgba + y * srcStrideBytes, copyWBytes);
        }

        ANativeWindow_unlockAndPost(gWin);
    }

    static void trySetFpsLocked(int fd, int fps) {
        if (fps <= 0) return;

        v4l2_streamparm parm{};
        parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

        if (xioctl(fd, VIDIOC_G_PARM, &parm) == 0) {
            parm.parm.capture.timeperframe.numerator = 1;
            parm.parm.capture.timeperframe.denominator = (uint32_t) fps;
            (void) xioctl(fd, VIDIOC_S_PARM, &parm);
        }

        trySetJpegQualityMax(fd);
    }

    static bool setupLocked(int w, int h, int fps, std::string &dbgOut) {
        gFd = openBestNodeLocked(dbgOut);
        if (gFd < 0) {
            setErrLocked("UVC node bulunamadi / open olmadi.\n" + dbgOut);
            return false;
        }

        dumpModesLocked(gFd);
        dumpControlsLocked(gFd);

        // FPS ve quality hint
        trySetFpsLocked(gFd, fps);

        v4l2_format fmt{};
        bool okFmt = trySetFormatLocked(gFd, w, h, V4L2_PIX_FMT_MJPEG, fmt);
        if (!okFmt) {
            int e = errno;
            setErrLocked(std::string("VIDIOC_S_FMT(MJPG) failed errno=") + std::to_string(e) +
                         " (" + std::strerror(e) + ")");
            return false;
        }

        // S_FMT sonrası bazı cihazlar ctrl resetler -> tekrar uygula
        trySetFpsLocked(gFd, fps);

        // ÖNCE reset (bozuk state temizliği)
        resetTouchedControlsToDefaultsLocked(gFd);

        // SONRA güvenli auto tuning
        tuneImageControlsLocked(gFd, fps);

        gChosenFourcc.store(fmt.fmt.pix.pixelformat, std::memory_order_relaxed);
        gChosenW.store((int) fmt.fmt.pix.width, std::memory_order_relaxed);
        gChosenH.store((int) fmt.fmt.pix.height, std::memory_order_relaxed);

        v4l2_streamparm parm2{};
        parm2.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        int readFps = fps;
        if (xioctl(gFd, VIDIOC_G_PARM, &parm2) == 0) {
            int num = (int) parm2.parm.capture.timeperframe.numerator;
            int den = (int) parm2.parm.capture.timeperframe.denominator;
            if (num > 0 && den > 0) readFps = den / num;
        }
        gChosenFps.store(readFps, std::memory_order_relaxed);

        if (fps >= 60 && readFps < 55) {
            setErrLocked("Kamera/driver 60fps kabul etmedi. G_PARM sonucu fps=" +
                         std::to_string(readFps));
            return false;
        }

        v4l2_requestbuffers req{};
        req.count = 16;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;

        if (xioctl(gFd, VIDIOC_REQBUFS, &req) != 0 || req.count < 4) {
            int e = errno;
            setErrLocked(std::string("VIDIOC_REQBUFS failed errno=") + std::to_string(e) +
                         " (" + std::strerror(e) + ")");
            return false;
        }

        gBufs.clear();
        gBufs.resize(req.count);

        for (uint32_t i = 0; i < req.count; i++) {
            v4l2_buffer buf{};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;

            if (xioctl(gFd, VIDIOC_QUERYBUF, &buf) != 0) {
                int e = errno;
                setErrLocked(std::string("VIDIOC_QUERYBUF failed errno=") + std::to_string(e) +
                             " (" + std::strerror(e) + ")");
                return false;
            }

            void *p = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, gFd,
                           buf.m.offset);
            if (p == MAP_FAILED) {
                int e = errno;
                setErrLocked(std::string("mmap failed errno=") + std::to_string(e) +
                             " (" + std::strerror(e) + ")");
                return false;
            }

            gBufs[i].ptr = p;
            gBufs[i].len = buf.length;

            if (xioctl(gFd, VIDIOC_QBUF, &buf) != 0) {
                int e = errno;
                setErrLocked(std::string("VIDIOC_QBUF failed errno=") + std::to_string(e) +
                             " (" + std::strerror(e) + ")");
                return false;
            }
        }

        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(gFd, VIDIOC_STREAMON, &type) != 0) {
            int e = errno;
            setErrLocked(std::string("VIDIOC_STREAMON failed errno=") + std::to_string(e) +
                         " (" + std::strerror(e) + ")");
            return false;
        }

        if (gWin) {
            (void) ANativeWindow_setBuffersGeometry(
                    gWin,
                    (int) fmt.fmt.pix.width,
                    (int) fmt.fmt.pix.height,
                    WINDOW_FORMAT_RGBA_8888
            );

            trySetFrameRate(gWin, (float) fps,
                            (int32_t) ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE);
        }

        gW = (int) fmt.fmt.pix.width;
        gH = (int) fmt.fmt.pix.height;

        return true;
    }

    static void captureLoop() {
        while (gRunning.load(std::memory_order_relaxed)) {
            pollfd pfd{};
            pfd.fd = gFd;
            pfd.events = POLLIN;

            int pr = poll(&pfd, 1, 2000);
            if (pr <= 0) continue;

            v4l2_buffer buf{};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;

            if (xioctl(gFd, VIDIOC_DQBUF, &buf) != 0) continue;

            long long ts = nowBoottimeNs();
            gLastFrameTsNs.store(ts, std::memory_order_relaxed);

            long long prev = gPrevFrameTsNs.exchange(ts, std::memory_order_relaxed);
            if (prev != 0 && ts > prev) {
                double fps = 1e9 / (double) (ts - prev);
                if (fps > 0.0 && fps < 10000.0) {
                    gFpsX100.store((int) (fps * 100.0), std::memory_order_relaxed);
                }
            }

            if (buf.index < gBufs.size() && buf.bytesused > 0) {
                const uint8_t *src = (const uint8_t *) gBufs[buf.index].ptr;
                int used = (int) buf.bytesused;

                if (gChosenFourcc.load(std::memory_order_relaxed) == V4L2_PIX_FMT_MJPEG) {
                    {
                        std::lock_guard<std::mutex> lk(gFrameLock);
                        gLatestJpeg.resize((size_t) used);
                        std::memcpy(gLatestJpeg.data(), src, (size_t) used);
                        gHasNewFrame.store(true, std::memory_order_relaxed);
                    }
                    gFrameCv.notify_one();
                }
            }

            (void) xioctl(gFd, VIDIOC_QBUF, &buf);
        }

        gFrameCv.notify_all();
    }

    static void decodeLoop() {
        std::vector<uint8_t> local;

        while (gRunning.load(std::memory_order_relaxed)) {
            {
                std::unique_lock<std::mutex> lk(gFrameLock);
                gFrameCv.wait(lk, [] {
                    return !gRunning.load(std::memory_order_relaxed) ||
                           gHasNewFrame.load(std::memory_order_relaxed);
                });

                if (!gRunning.load(std::memory_order_relaxed)) break;

                gLatestJpeg.swap(local); // KOPYA YOK
                gHasNewFrame.store(false, std::memory_order_relaxed);
            }

            if (!gWin || local.empty()) continue;

            int outW = 0, outH = 0, outComp = 0;
            uint8_t *rgba = stbi_load_from_memory(local.data(), (int) local.size(),
                                                  &outW, &outH, &outComp, 4);
            if (rgba && outW > 0 && outH > 0) {
                renderRgbaToWindow(rgba, outW, outH);
            }
            if (rgba) stbi_image_free(rgba);
        }
    }

    bool start(JNIEnv *env, jobject surface, int desiredFps) {
        std::lock_guard<std::mutex> lk(gLock);
        clearErrLocked();

        if (gRunning.load(std::memory_order_relaxed)) {
            gRunning.store(false, std::memory_order_relaxed);
            gFrameCv.notify_all();
            if (gCaptureThread.joinable()) gCaptureThread.join();
            if (gDecodeThread.joinable()) gDecodeThread.join();
            tearDownLocked();
        }

        gWin = ANativeWindow_fromSurface(env, surface);
        if (!gWin) {
            setErrLocked("ANativeWindow_fromSurface(UVC) failed");
            return false;
        }

        int fps = (desiredFps > 0 ? desiredFps : 60);

        std::string dbg;
        bool ok = setupLocked(1280, 720, fps, dbg);
        if (!ok) {
            std::string e1 = gLastError;
            tearDownLocked();
            setErrLocked(std::string("setup(1280x720 MJPG) fail:\n") + e1 + "\nDBG:\n" + dbg);
            return false;
        }

        {
            std::lock_guard<std::mutex> fl(gFrameLock);
            gLatestJpeg.reserve(512 * 1024);
            gHasNewFrame.store(false, std::memory_order_relaxed);
        }

        gRunning.store(true, std::memory_order_relaxed);
        gCaptureThread = std::thread(captureLoop);
        gDecodeThread = std::thread(decodeLoop);
        return true;
    }

    void stop() {
        std::lock_guard<std::mutex> lk(gLock);
        if (!gRunning.load(std::memory_order_relaxed)) return;

        gRunning.store(false, std::memory_order_relaxed);
        gFrameCv.notify_all();

        if (gCaptureThread.joinable()) gCaptureThread.join();
        if (gDecodeThread.joinable()) gDecodeThread.join();

        tearDownLocked();
    }

    long long lastFrameTimestampNs() { return gLastFrameTsNs.load(std::memory_order_relaxed); }

    int estimatedFpsX100() { return gFpsX100.load(std::memory_order_relaxed); }

    int chosenFps() { return gChosenFps.load(std::memory_order_relaxed); }

    std::string lastError() {
        std::lock_guard<std::mutex> lk(gLock);
        return gLastError;
    }

    std::string chosenMode() {
        uint32_t f = gChosenFourcc.load(std::memory_order_relaxed);
        int w = gChosenW.load(std::memory_order_relaxed);
        int h = gChosenH.load(std::memory_order_relaxed);
        if (f == 0 || w == 0 || h == 0) return "n/a";
        return fourccToStr(f) + " " + std::to_string(w) + "x" + std::to_string(h);
    }

} // namespace uvc
