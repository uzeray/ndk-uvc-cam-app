// uvc_camera.cpp

#include "uvc_camera.h"
#include "../common/logging.h"
#include "../common/time_utils.h"

#include <android/native_window_jni.h>
#include <android/native_window.h>

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <poll.h>
#include <errno.h>

#include <linux/videodev2.h>
#include <linux/v4l2-controls.h>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <cmath>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include <dlfcn.h>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>

#ifndef V4L2_CID_JPEG_COMPRESSION_QUALITY
#define V4L2_CID_JPEG_COMPRESSION_QUALITY 0x009f090d
#endif
#ifndef V4L2_CID_AUTOGAIN
#define V4L2_CID_AUTOGAIN 0x00980913
#endif

#ifndef V4L2_EXPOSURE_AUTO
#define V4L2_EXPOSURE_AUTO 0
#endif
#ifndef V4L2_EXPOSURE_MANUAL
#define V4L2_EXPOSURE_MANUAL 1
#endif

#ifndef V4L2_CID_POWER_LINE_FREQUENCY_50HZ
#define V4L2_CID_POWER_LINE_FREQUENCY_50HZ 1
#endif

// ---------- AE tuning ----------
#ifndef UVC_AE_TARGET_LUMA
#define UVC_AE_TARGET_LUMA 122
#endif
#ifndef UVC_AE_TOL
#define UVC_AE_TOL 10
#endif
#ifndef UVC_AE_ADJUST_INTERVAL_MS
#define UVC_AE_ADJUST_INTERVAL_MS 120
#endif
#ifndef UVC_AE_MAX_EXPOSURE_US_CAP
#define UVC_AE_MAX_EXPOSURE_US_CAP 8000
#endif
#ifndef UVC_AE_MIN_EXPOSURE_US_CAP
#define UVC_AE_MIN_EXPOSURE_US_CAP 400
#endif

//
// CROP FACTOR: We only want the TOP 30% of the UVC frame.
// This prevents squeezing 720 lines into a small view.
#define UVC_CROP_HEIGHT_RATIO 0.30f

namespace uvc {

    static std::mutex gLock;
    static std::string gLastError;

    static int gFd = -1;
    static ANativeWindow *gWin = nullptr;

    struct MmapBuf {
        void *ptr = nullptr;
        size_t len = 0;
    };
    static std::vector<MmapBuf> gBufs;

    static std::atomic<bool> gRunning{false};
    static std::thread gThCap;
    static std::thread gThDec;

    static std::atomic<long long> gLastFrameTsNs{0};
    static std::atomic<long long> gPrevFrameTsNs{0};
    static std::atomic<int> gFpsX100{0};

    static std::atomic<int> gChosenFps{0};
    static std::atomic<uint32_t> gChosenFourcc{0};
    static std::atomic<int> gChosenW{0};
    static std::atomic<int> gChosenH{0};

    static int gW = 0, gH = 0;

    // mailbox (single-latest)
    static std::mutex gFrameLock;
    static std::condition_variable gFrameCv;
    static std::vector<uint8_t> gFrameBytes;
    static std::atomic<bool> gFrameReady{false};

    // AE state
    struct CtrlRange {
        bool ok = false;
        int minV = 0, maxV = 0, step = 1, defV = 0;
    };
    static std::mutex gCtrlLock;
    static CtrlRange gExpAbs{};
    static CtrlRange gGain{};
    static std::atomic<int> gCurExpAbs{0}; // exposure_absolute (100us)
    static std::atomic<int> gCurGain{0};
    static std::atomic<bool> gAeEnabled{false};
    static std::atomic<long long> gLastAeAdjustNs{0};

    // ---------- helpers ----------
    static void setErrLocked(const std::string &s) { gLastError = s; }

    static void clearErrLocked() { gLastError.clear(); }

    static int xioctl(int fd, unsigned long req, void *arg) {
        int r;
        do { r = ioctl(fd, req, arg); } while (r == -1 && errno == EINTR);
        return r;
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

    static CtrlRange readRange(int fd, __u32 id) {
        CtrlRange r{};
        v4l2_queryctrl qc{};
        if (!queryCtrl(fd, id, qc)) return r;
        r.ok = true;
        r.minV = (int) qc.minimum;
        r.maxV = (int) qc.maximum;
        r.step = (int) std::max<__s32>(qc.step, 1);
        r.defV = (int) qc.default_value;
        return r;
    }

    static int clampToRange(const CtrlRange &r, int v) {
        if (!r.ok) return v;
        v = std::clamp(v, r.minV, r.maxV);
        if (r.step > 1) {
            int base = r.minV;
            v = base + ((v - base) / r.step) * r.step;
            v = std::clamp(v, r.minV, r.maxV);
        }
        return v;
    }

    static void trySetJpegQualityMax(int fd) {
        v4l2_queryctrl qc{};
        qc.id = V4L2_CID_JPEG_COMPRESSION_QUALITY;
        if (xioctl(fd, VIDIOC_QUERYCTRL, &qc) == 0) {
            (void) setCtrl(fd, V4L2_CID_JPEG_COMPRESSION_QUALITY, (int) qc.maximum);
        }
    }

    static void trySetFrameRate(ANativeWindow *win, float fps) {
        if (!win) return;
        void *h = dlopen("libandroid.so", RTLD_NOW);
        if (!h) return;
        using Fn = int32_t (*)(ANativeWindow *, float, int32_t);
        auto fn = reinterpret_cast<Fn>(dlsym(h, "ANativeWindow_setFrameRate"));
        if (fn) {
            (void) fn(win, fps, (int32_t) ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE);
        }
        dlclose(h);
    }

    static bool isCaptureNode(int fd, v4l2_capability &cap) {
        if (xioctl(fd, VIDIOC_QUERYCAP, &cap) != 0) return false;
        if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) return false;
        if (!(cap.capabilities & V4L2_CAP_STREAMING)) return false;
        return true;
    }

    static int openBestNode(std::string &dbg) {
        int fallback = -1;
        v4l2_capability fcap{};
        for (int i = 0; i < 64; i++) {
            char path[64];
            std::snprintf(path, sizeof(path), "/dev/video%d", i);
            int fd = open(path, O_RDWR | O_NONBLOCK);
            if (fd < 0) {
                if (errno == ENOENT) continue;
                continue;
            }
            v4l2_capability cap{};
            if (!isCaptureNode(fd, cap)) {
                close(fd);
                continue;
            }
            bool isUvc = (std::strncmp((const char *) cap.driver, "uvcvideo", 7) == 0);
            if (isUvc) {
                dbg += std::string("SELECT ") + path + "\n";
                if (fallback >= 0) {
                    close(fallback);
                    fallback = -1;
                }
                return fd;
            }
            if (fallback < 0) {
                fallback = fd;
                fcap = cap;
            } else {
                close(fd);
            }
        }
        if (fallback >= 0) {
            dbg += std::string("FALLBACK driver=") + (const char *) fcap.driver + "\n";
        }
        return fallback;
    }

    static bool trySetFormat(int fd, int w, int h, uint32_t fourcc, v4l2_format &outFmt) {
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

    static void trySetFps(int fd, int fps) {
        if (fps <= 0) return;
        v4l2_streamparm p{};
        p.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(fd, VIDIOC_G_PARM, &p) == 0) {
            p.parm.capture.timeperframe.numerator = 1;
            p.parm.capture.timeperframe.denominator = (uint32_t) fps;
            (void) xioctl(fd, VIDIOC_S_PARM, &p);
        }
    }

    static int readFps(int fd, int fallback) {
        v4l2_streamparm p{};
        p.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(fd, VIDIOC_G_PARM, &p) == 0) {
            int num = (int) p.parm.capture.timeperframe.numerator;
            int den = (int) p.parm.capture.timeperframe.denominator;
            if (num > 0 && den > 0) return den / num;
        }
        return fallback;
    }

    static inline uint8_t clamp8(int v) {
        return (uint8_t) (v < 0 ? 0 : (v > 255 ? 255 : v));
    }

    static int avgLumaYuyvSample(const uint8_t *yuyv, int w, int h) {
        if (!yuyv || w <= 0 || h <= 0) return 0;
        const int stepX = std::max(1, w / 64);
        const int stepY = std::max(1, h / 36);
        const int stride = w * 2;
        long long sum = 0;
        int cnt = 0;
        for (int y = 0; y < h; y += stepY) {
            const uint8_t *row = yuyv + y * stride;
            for (int x = 0; x < w; x += stepX) {
                sum += (int) row[2 * x];
                cnt++;
            }
        }
        return cnt ? (int) (sum / cnt) : 0;
    }

    static int avgLumaRgbaSample(const uint8_t *rgba, int w, int h) {
        if (!rgba || w <= 0 || h <= 0) return 0;
        const int stepX = std::max(1, w / 64);
        const int stepY = std::max(1, h / 36);
        const int stride = w * 4;
        long long sum = 0;
        int cnt = 0;
        for (int y = 0; y < h; y += stepY) {
            const uint8_t *row = rgba + y * stride;
            for (int x = 0; x < w; x += stepX) {
                const uint8_t *p = row + x * 4;
                int R = p[0], G = p[1], B = p[2];
                int Y = (77 * R + 150 * G + 29 * B) >> 8;
                sum += Y;
                cnt++;
            }
        }
        return cnt ? (int) (sum / cnt) : 0;
    }

    static int exposureCapAbsForFps(int fps, const CtrlRange &exp) {
        if (!exp.ok) return 0;
        fps = std::max(1, fps);
        double frameUs = 1000000.0 / (double) fps;
        int capUs = (int) std::floor(frameUs * 0.80);
        capUs = std::clamp(capUs, UVC_AE_MIN_EXPOSURE_US_CAP, UVC_AE_MAX_EXPOSURE_US_CAP);
        int capAbs = capUs / 100;
        capAbs = std::clamp(capAbs, exp.minV, exp.maxV);
        capAbs = clampToRange(exp, capAbs);
        return capAbs;
    }

    static void autoExposureMaybeAdjust(int avgLuma) {
        if (!gAeEnabled.load(std::memory_order_relaxed)) return;
        if (avgLuma <= 0) return;

        long long now = nowBoottimeNs();
        long long last = gLastAeAdjustNs.load(std::memory_order_relaxed);
        const long long interval = (long long) UVC_AE_ADJUST_INTERVAL_MS * 1000000LL;
        if (last != 0 && (now - last) < interval) return;
        if (!gLastAeAdjustNs.compare_exchange_strong(last, now, std::memory_order_relaxed)) return;

        const int target = UVC_AE_TARGET_LUMA;
        const int tol = UVC_AE_TOL;
        if (std::abs(avgLuma - target) <= tol) return;

        std::lock_guard<std::mutex> lk(gCtrlLock);
        if (gFd < 0) return;

        const int fps = std::max(gChosenFps.load(std::memory_order_relaxed), 30);
        const int expCap = exposureCapAbsForFps(fps, gExpAbs);

        int curExp = gCurExpAbs.load(std::memory_order_relaxed);
        int curGain = gCurGain.load(std::memory_order_relaxed);

        if (gExpAbs.ok && curExp == 0) {
            int v = 0;
            if (getCtrl(gFd, V4L2_CID_EXPOSURE_ABSOLUTE, v)) curExp = v;
        }
        if (gGain.ok && curGain == 0) {
            int v = 0;
            if (getCtrl(gFd, V4L2_CID_GAIN, v)) curGain = v;
        }

        bool changed = false;

        if (avgLuma > target + tol) {
            if (gGain.ok && curGain > gGain.minV) {
                int next = clampToRange(gGain, curGain - gGain.step);
                if (next != curGain && setCtrl(gFd, V4L2_CID_GAIN, next)) {
                    curGain = next;
                    changed = true;
                }
            } else if (gExpAbs.ok && curExp > gExpAbs.minV) {
                int next = clampToRange(gExpAbs, curExp - gExpAbs.step);
                if (next != curExp && setCtrl(gFd, V4L2_CID_EXPOSURE_ABSOLUTE, next)) {
                    curExp = next;
                    changed = true;
                }
            }
        } else if (avgLuma < target - tol) {
            if (gExpAbs.ok && expCap > 0 && curExp < expCap) {
                int next = std::min(curExp + gExpAbs.step, expCap);
                next = clampToRange(gExpAbs, next);
                if (next != curExp && setCtrl(gFd, V4L2_CID_EXPOSURE_ABSOLUTE, next)) {
                    curExp = next;
                    changed = true;
                }
            } else if (gGain.ok && curGain < gGain.maxV) {
                int next = clampToRange(gGain, curGain + gGain.step);
                if (next != curGain && setCtrl(gFd, V4L2_CID_GAIN, next)) {
                    curGain = next;
                    changed = true;
                }
            }
        }

        if (changed) {
            gCurExpAbs.store(curExp, std::memory_order_relaxed);
            gCurGain.store(curGain, std::memory_order_relaxed);
        }
    }

    static void renderRgbaToWindow(const uint8_t *rgba, int w, int h) {
        if (!gWin) return;
        ANativeWindow_Buffer out{};
        if (ANativeWindow_lock(gWin, &out, nullptr) != 0) return;

        uint8_t *dst = (uint8_t *) out.bits;
        int dstStride = out.stride * 4;
        int srcStride = w * 4;

        int copyH = std::min(h, out.height);
        int copyWBytes = std::min(w, out.width) * 4;

        for (int y = 0; y < copyH; y++) {
            std::memcpy(dst + y * dstStride, rgba + y * srcStride, copyWBytes);
            if (copyWBytes < dstStride) {
                std::memset(dst + y * dstStride + copyWBytes, 0, (size_t) (dstStride - copyWBytes));
            }
        }
        for (int y = copyH; y < out.height; y++) {
            std::memset(dst + y * dstStride, 0, (size_t) dstStride);
        }

        ANativeWindow_unlockAndPost(gWin);
    }

    static void applyControls(int fd, int chosenFps) {
        // 50Hz flicker (EU/TR)
        {
            v4l2_queryctrl qc{};
            if (queryCtrl(fd, V4L2_CID_POWER_LINE_FREQUENCY, qc)) {
                (void) setCtrl(fd, V4L2_CID_POWER_LINE_FREQUENCY,
                               V4L2_CID_POWER_LINE_FREQUENCY_50HZ);
            }
        }

        // AE priority off (fps sabit kalsın)
        {
            v4l2_queryctrl qc{};
            if (queryCtrl(fd, V4L2_CID_EXPOSURE_AUTO_PRIORITY, qc)) {
                (void) setCtrl(fd, V4L2_CID_EXPOSURE_AUTO_PRIORITY, 0);
            }
        }

        // Auto WB on
        (void) setCtrl(fd, V4L2_CID_AUTO_WHITE_BALANCE, 1);

        // MJPEG quality max
        trySetJpegQualityMax(fd);

        // Manual exp + autogain off (varsa) + yazılımsal AE
        {
            v4l2_queryctrl qc{};
            bool expAutoOk = queryCtrl(fd, V4L2_CID_EXPOSURE_AUTO, qc);
            bool expAbsOk = queryCtrl(fd, V4L2_CID_EXPOSURE_ABSOLUTE, qc);
            bool gainOk = queryCtrl(fd, V4L2_CID_GAIN, qc);
            bool autogOk = queryCtrl(fd, V4L2_CID_AUTOGAIN, qc);

            gExpAbs = readRange(fd, V4L2_CID_EXPOSURE_ABSOLUTE);
            gGain = readRange(fd, V4L2_CID_GAIN);

            if (expAutoOk && expAbsOk) {
                (void) setCtrl(fd, V4L2_CID_EXPOSURE_AUTO, V4L2_EXPOSURE_MANUAL);
            }
            if (autogOk) {
                (void) setCtrl(fd, V4L2_CID_AUTOGAIN, 0);
            }

            const int fps = chosenFps > 0 ? chosenFps : 60;
            const int expCap = exposureCapAbsForFps(fps, gExpAbs);

            if (gExpAbs.ok && expCap > 0) {
                int initExp = gExpAbs.minV + (expCap - gExpAbs.minV) / 2;
                initExp = clampToRange(gExpAbs, initExp);
                if (setCtrl(fd, V4L2_CID_EXPOSURE_ABSOLUTE, initExp)) {
                    gCurExpAbs.store(initExp, std::memory_order_relaxed);
                }
            }
            if (gGain.ok) {
                int initGain = gGain.minV + (gGain.maxV - gGain.minV) / 4;
                initGain = clampToRange(gGain, initGain);
                if (setCtrl(fd, V4L2_CID_GAIN, initGain)) {
                    gCurGain.store(initGain, std::memory_order_relaxed);
                }
            }

            gAeEnabled.store(gExpAbs.ok || gGain.ok, std::memory_order_relaxed);
            (void) gainOk;
        }
    }

    // ---- Enumerate supported sizes/fps to choose best stable mode ----
    static std::vector<std::pair<int, int>> enumFrameSizes(int fd, uint32_t pixfmt) {
        std::vector<std::pair<int, int>> out;

        v4l2_frmsizeenum fse{};
        fse.pixel_format = pixfmt;

        for (fse.index = 0; xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fse) == 0; fse.index++) {
            if (fse.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
                out.emplace_back((int) fse.discrete.width, (int) fse.discrete.height);
            } else if (fse.type == V4L2_FRMSIZE_TYPE_STEPWISE ||
                       fse.type == V4L2_FRMSIZE_TYPE_CONTINUOUS) {
                int minW = (int) fse.stepwise.min_width;
                int maxW = (int) fse.stepwise.max_width;
                int minH = (int) fse.stepwise.min_height;
                int maxH = (int) fse.stepwise.max_height;

                // En büyük
                out.emplace_back(maxW, maxH);

                // Yaygın 16:9 seçeneklerini aralığa uyarsa ekle
                const std::pair<int, int> common[] = {
                        {3840, 2160},
                        {2560, 1440},
                        {1920, 1080},
                        {1600, 900},
                        {1280, 720},
                        {960,  540},
                        {640,  360},
                        {640,  480}
                };
                for (auto &c: common) {
                    if (c.first >= minW && c.first <= maxW && c.second >= minH &&
                        c.second <= maxH) {
                        out.emplace_back(c.first, c.second);
                    }
                }
                break;
            }
        }

        // uniq
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

    static int enumMaxFpsFor(int fd, uint32_t pixfmt, int w, int h) {
        int best = 0;
        v4l2_frmivalenum fie{};
        fie.pixel_format = pixfmt;
        fie.width = (uint32_t) w;
        fie.height = (uint32_t) h;

        for (fie.index = 0; xioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &fie) == 0; fie.index++) {
            if (fie.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
                int num = (int) fie.discrete.numerator;
                int den = (int) fie.discrete.denominator;
                if (num > 0 && den > 0) {
                    int fps = den / num;
                    best = std::max(best, fps);
                }
            } else if (fie.type == V4L2_FRMIVAL_TYPE_STEPWISE ||
                       fie.type == V4L2_FRMIVAL_TYPE_CONTINUOUS) {
                int num = (int) fie.stepwise.min.numerator;
                int den = (int) fie.stepwise.min.denominator;
                if (num > 0 && den > 0) {
                    int fps = den / num;
                    best = std::max(best, fps);
                }
                break;
            }
        }
        return best;
    }

    struct ModeCand {
        int w = 0;
        int h = 0;
        uint32_t f = 0;
        int maxFps = 0;
        int scoreMeet = 0; // 1 if can meet desired
    };

    static std::vector<ModeCand> buildCandidates(int fd, int desiredFps) {
        std::vector<ModeCand> out;
        const uint32_t fmts[] = {V4L2_PIX_FMT_MJPEG, V4L2_PIX_FMT_YUYV};

        for (uint32_t f: fmts) {
            auto sizes = enumFrameSizes(fd, f);

            // Eğer ENUM başarısızsa fallback liste:
            if (sizes.empty()) {
                const std::pair<int, int> fallback[] = {
                        {1920, 1080},
                        {1600, 900},
                        {1280, 720},
                        {1024, 576},
                        {960,  540},
                        {800,  600},
                        {640,  480}
                };
                sizes.assign(std::begin(fallback), std::end(fallback));
            }

            for (auto &s: sizes) {
                int w = s.first;
                int h = s.second;
                if (w <= 0 || h <= 0) continue;

                int m = enumMaxFpsFor(fd, f, w, h);
                // ENUM interval yoksa 0 gelir; yine de denenecek ama düşük öncelik.
                ModeCand c{};
                c.w = w;
                c.h = h;
                c.f = f;
                c.maxFps = m;
                c.scoreMeet = (m >= desiredFps - 2) ? 1 : 0;
                out.push_back(c);
            }
        }

        // uniq by (f,w,h)
        std::sort(out.begin(), out.end(), [](const ModeCand &a, const ModeCand &b) {
            if (a.f != b.f) return a.f < b.f;
            if (a.w != b.w) return a.w < b.w;
            return a.h < b.h;
        });
        out.erase(std::unique(out.begin(), out.end(), [](const ModeCand &a, const ModeCand &b) {
            return a.f == b.f && a.w == b.w && a.h == b.h;
        }), out.end());

        // sort best-first:
        std::sort(out.begin(), out.end(), [desiredFps](const ModeCand &a, const ModeCand &b) {
            // 1) meets desired fps (by enum) first
            if (a.scoreMeet != b.scoreMeet) return a.scoreMeet > b.scoreMeet;

            // 2) prefer higher resolution area
            long long aa = (long long) a.w * (long long) a.h;
            long long bb = (long long) b.w * (long long) b.h;
            if (aa != bb) return aa > bb;

            // 3) prefer higher maxFps (if known)
            if (a.maxFps != b.maxFps) return a.maxFps > b.maxFps;

            // 4) prefer MJPEG over YUYV for stability/bandwidth
            if (a.f != b.f) return (a.f == V4L2_PIX_FMT_MJPEG);

            return false;
        });

        return out;
    }

    static void teardownLocked() {
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
            gFrameBytes.clear();
            gFrameReady.store(false, std::memory_order_relaxed);
        }

        gLastFrameTsNs.store(0, std::memory_order_relaxed);
        gPrevFrameTsNs.store(0, std::memory_order_relaxed);
        gFpsX100.store(0, std::memory_order_relaxed);

        gChosenFps.store(0, std::memory_order_relaxed);
        gChosenFourcc.store(0, std::memory_order_relaxed);
        gChosenW.store(0, std::memory_order_relaxed);
        gChosenH.store(0, std::memory_order_relaxed);

        gAeEnabled.store(false, std::memory_order_relaxed);
        gExpAbs = {};
        gGain = {};
        gCurExpAbs.store(0, std::memory_order_relaxed);
        gCurGain.store(0, std::memory_order_relaxed);
        gLastAeAdjustNs.store(0, std::memory_order_relaxed);

        gW = 0;
        gH = 0;

        std::lock_guard<std::mutex> lk(gLock);
        gLastError.clear();
    }

    static bool setupLocked(int desiredFps, std::string &dbg) {
        gFd = openBestNode(dbg);
        if (gFd < 0) {
            setErrLocked("UVC device open failed.\n" + dbg);
            return false;
        }

        const int want = (desiredFps > 0 ? desiredFps : 60);

        auto cands = buildCandidates(gFd, want);

        v4l2_format fmt{};
        bool ok = false;

        int bestGotFps = 0;
        v4l2_format bestFmt{};
        int bestW = 0, bestH = 0;
        uint32_t bestFourcc = 0;

        // Deneme sırası: en iyi adaydan en kötüye
        for (const auto &c: cands) {
            if (!trySetFormat(gFd, c.w, c.h, c.f, fmt)) continue;

            // fps ayarla
            int tryFps = want;
            if (c.maxFps > 0) tryFps = std::min(tryFps, c.maxFps);
            trySetFps(gFd, tryFps);
            trySetJpegQualityMax(gFd);
            int got = readFps(gFd, tryFps);

            // kontrol uygula (fps sabit + kalite)
            applyControls(gFd, got);

            // en iyiyi sakla
            if (!ok || (got >= want - 2 && bestGotFps < want - 2) ||
                ((got >= want - 2) == (bestGotFps >= want - 2) &&
                 ((long long) fmt.fmt.pix.width * (long long) fmt.fmt.pix.height >
                  (long long) bestW * (long long) bestH)) ||
                ((long long) fmt.fmt.pix.width * (long long) fmt.fmt.pix.height ==
                 (long long) bestW * (long long) bestH && got > bestGotFps)
                    ) {
                ok = true;
                bestGotFps = got;
                bestFmt = fmt;
                bestW = (int) fmt.fmt.pix.width;
                bestH = (int) fmt.fmt.pix.height;
                bestFourcc = fmt.fmt.pix.pixelformat;

                // hedef fps'e yeterince yakınsa ve zaten en yüksek çözünürlüklere gidiyorsak erken çık
                if (bestGotFps >= want - 2 && c.scoreMeet == 1) {
                    break;
                }
            }
        }

        if (!ok) {
            setErrLocked("VIDIOC_S_FMT failed (no candidate worked)");
            return false;
        }

        // Seçilen en iyi fmt'i tekrar setleyelim (bazı driverlar arada değişebiliyor)
        (void) trySetFormat(gFd, bestW, bestH, bestFourcc, fmt);

        gChosenFps.store(bestGotFps > 0 ? bestGotFps : want, std::memory_order_relaxed);

        gW = (int) fmt.fmt.pix.width;
        gH = (int) fmt.fmt.pix.height;
        gChosenFourcc.store(fmt.fmt.pix.pixelformat, std::memory_order_relaxed);

        // =========================================================================
        // [MODIFICATION START]: Force buffer geometry to CROPPED height
        // =========================================================================
        // We only want the top 30% of the image.
        // We trick Kotlin into thinking the resolution is small (e.g. 1280x216),
        // so it creates a matching surface buffer.
        int cropH = (int) (gH * UVC_CROP_HEIGHT_RATIO);
        gChosenW.store(gW, std::memory_order_relaxed);
        gChosenH.store(cropH, std::memory_order_relaxed); // Notify Kotlin of the smaller height

        // reqbufs + mmap
        v4l2_requestbuffers req{};
        req.count = 12;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        if (xioctl(gFd, VIDIOC_REQBUFS, &req) != 0 || req.count < 4) {
            setErrLocked("VIDIOC_REQBUFS failed");
            return false;
        }

        gBufs.assign(req.count, {});
        for (uint32_t i = 0; i < req.count; i++) {
            v4l2_buffer b{};
            b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            b.memory = V4L2_MEMORY_MMAP;
            b.index = i;

            if (xioctl(gFd, VIDIOC_QUERYBUF, &b) != 0) {
                setErrLocked("VIDIOC_QUERYBUF failed");
                return false;
            }

            void *p = mmap(nullptr, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, gFd, b.m.offset);
            if (p == MAP_FAILED) {
                setErrLocked("mmap failed");
                return false;
            }
            gBufs[i].ptr = p;
            gBufs[i].len = b.length;

            if (xioctl(gFd, VIDIOC_QBUF, &b) != 0) {
                setErrLocked("VIDIOC_QBUF failed");
                return false;
            }
        }

        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(gFd, VIDIOC_STREAMON, &type) != 0) {
            setErrLocked("VIDIOC_STREAMON failed");
            return false;
        }

        if (gWin) {
            // [MODIFICATION START]: Use cropH instead of gH for the window geometry
            (void) ANativeWindow_setBuffersGeometry(gWin, gW, cropH, WINDOW_FORMAT_RGBA_8888);
            // [MODIFICATION END]
            int fps = gChosenFps.load(std::memory_order_relaxed);
            trySetFrameRate(gWin, (float) (fps > 0 ? fps : want));
        }

        // reserve mailbox
        {
            std::lock_guard<std::mutex> lk(gFrameLock);
            size_t cap = 512 * 1024;
            if (gChosenFourcc.load(std::memory_order_relaxed) == V4L2_PIX_FMT_YUYV && gW > 0 &&
                gH > 0) {
                cap = (size_t) gW * (size_t) gH * 2;
            } else if (gW > 0 && gH > 0) {
                // MJPEG için de kaba bir üst sınır
                cap = (size_t) gW * (size_t) gH; // sıkışık veri genelde bundan küçük olur
            }
            gFrameBytes.clear();
            gFrameBytes.reserve(cap);
            gFrameReady.store(false, std::memory_order_relaxed);
        }

        return true;
    }

    // ---------- threads ----------
    static void capLoop() {
        while (gRunning.load(std::memory_order_relaxed)) {
            pollfd pfd{};
            pfd.fd = gFd;
            pfd.events = POLLIN;
            int pr = poll(&pfd, 1, 2000);
            if (pr <= 0) continue;

            v4l2_buffer b{};
            b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            b.memory = V4L2_MEMORY_MMAP;
            if (xioctl(gFd, VIDIOC_DQBUF, &b) != 0) continue;

            long long ts = nowBoottimeNs();
            gLastFrameTsNs.store(ts, std::memory_order_relaxed);

            long long prev = gPrevFrameTsNs.exchange(ts, std::memory_order_relaxed);
            if (prev != 0 && ts > prev) {
                double fps = 1e9 / (double) (ts - prev);
                if (fps > 0.0 && fps < 10000.0)
                    gFpsX100.store((int) (fps * 100.0), std::memory_order_relaxed);
            }

            if (b.index < gBufs.size() && b.bytesused > 0) {
                const uint8_t *src = (const uint8_t *) gBufs[b.index].ptr;
                int used = (int) b.bytesused;

                // hızlı AE ölçümü (YUYV için decode beklemeden)
                if (gChosenFourcc.load(std::memory_order_relaxed) == V4L2_PIX_FMT_YUYV && gW > 0 &&
                    gH > 0) {
                    size_t need = (size_t) gW * (size_t) gH * 2;
                    if ((size_t) used >= need) {
                        int avg = avgLumaYuyvSample(src, gW, gH);
                        autoExposureMaybeAdjust(avg);
                    }
                }

                {
                    std::lock_guard<std::mutex> lk(gFrameLock);
                    gFrameBytes.resize((size_t) used);
                    std::memcpy(gFrameBytes.data(), src, (size_t) used);
                    gFrameReady.store(true, std::memory_order_relaxed);
                }
                gFrameCv.notify_one();
            }

            (void) xioctl(gFd, VIDIOC_QBUF, &b);
        }
        gFrameCv.notify_all();
    }

    static void decLoop() {
        std::vector<uint8_t> local;

        // Reuse mats where possible to reduce allocations
        cv::Mat rgbaReuse;

        while (gRunning.load(std::memory_order_relaxed)) {
            {
                std::unique_lock<std::mutex> lk(gFrameLock);
                gFrameCv.wait(lk, [] {
                    return !gRunning.load(std::memory_order_relaxed) ||
                           gFrameReady.load(std::memory_order_relaxed);
                });
                if (!gRunning.load(std::memory_order_relaxed)) break;

                local.swap(gFrameBytes);
                gFrameReady.store(false, std::memory_order_relaxed);
            }

            if (!gWin || local.empty()) continue;

            uint32_t f = gChosenFourcc.load(std::memory_order_relaxed);

            // [MODIFICATION START]: Calculate CROP height based on the ratio
            int cropH = (int) (gH * UVC_CROP_HEIGHT_RATIO);
            if (cropH <= 0) cropH = 1;
            // [MODIFICATION END]

            if (f == V4L2_PIX_FMT_YUYV) {
                if (gW > 0 && gH > 0) {
                    size_t need = (size_t) gW * (size_t) gH * 2;
                    if (local.size() >= need) {
                        cv::Mat yuyv(gH, gW, CV_8UC2, local.data());

                        if (rgbaReuse.empty() || rgbaReuse.cols != gW || rgbaReuse.rows != gH) {
                            rgbaReuse = cv::Mat(gH, gW, CV_8UC4);
                        }
                        cv::cvtColor(yuyv, rgbaReuse, cv::COLOR_YUV2RGBA_YUY2);

                        int avg = avgLumaRgbaSample(rgbaReuse.data, rgbaReuse.cols, rgbaReuse.rows);
                        autoExposureMaybeAdjust(avg);

                        // [MODIFICATION START]: Crop top 30% and render
                        cv::Rect roi(0, 0, gW, cropH);
                        cv::Mat cropped = rgbaReuse(roi);
                        renderRgbaToWindow(cropped.data, cropped.cols, cropped.rows);
                        // [MODIFICATION END]
                    }
                }
                continue;
            }

            if (f == V4L2_PIX_FMT_MJPEG) {
                try {
                    cv::Mat buf(1, (int) local.size(), CV_8UC1, local.data());
                    cv::Mat bgr = cv::imdecode(buf, cv::IMREAD_COLOR);
                    if (!bgr.empty()) {
                        if (rgbaReuse.empty() || rgbaReuse.cols != bgr.cols ||
                            rgbaReuse.rows != bgr.rows) {
                            rgbaReuse = cv::Mat(bgr.rows, bgr.cols, CV_8UC4);
                        }
                        cv::cvtColor(bgr, rgbaReuse, cv::COLOR_BGR2RGBA);

                        int avg = avgLumaRgbaSample(rgbaReuse.data, rgbaReuse.cols, rgbaReuse.rows);
                        autoExposureMaybeAdjust(avg);

                        // [MODIFICATION START]: Crop top 30% and render
                        cv::Rect roi(0, 0, bgr.cols, cropH);
                        // Ensure roi is within bounds (in case decoding gave different size)
                        if (roi.height > rgbaReuse.rows) roi.height = rgbaReuse.rows;

                        cv::Mat cropped = rgbaReuse(roi);
                        renderRgbaToWindow(cropped.data, cropped.cols, cropped.rows);
                        // [MODIFICATION END]
                    }
                } catch (...) {
                    // ignore decode errors
                }
            }
        }
    }

    // ---------- public API ----------
    bool start(JNIEnv *env, jobject surface, int desiredFps) {
        std::lock_guard<std::mutex> lk(gLock);
        clearErrLocked();

        if (gRunning.load(std::memory_order_relaxed)) {
            gRunning.store(false, std::memory_order_relaxed);
            gFrameCv.notify_all();
            if (gThCap.joinable()) gThCap.join();
            if (gThDec.joinable()) gThDec.join();
            teardownLocked();
        }

        gWin = ANativeWindow_fromSurface(env, surface);
        if (!gWin) {
            setErrLocked("ANativeWindow_fromSurface failed");
            return false;
        }

        std::string dbg;
        if (!setupLocked(desiredFps > 0 ? desiredFps : 60, dbg)) {
            std::string e = gLastError;
            teardownLocked();
            setErrLocked("setup failed:\n" + e + "\n" + dbg);
            return false;
        }

        gRunning.store(true, std::memory_order_relaxed);
        gThCap = std::thread(capLoop);
        gThDec = std::thread(decLoop);
        return true;
    }

    void stop() {
        std::lock_guard<std::mutex> lk(gLock);
        if (!gRunning.load(std::memory_order_relaxed)) return;

        gRunning.store(false, std::memory_order_relaxed);
        gFrameCv.notify_all();

        if (gThCap.joinable()) gThCap.join();
        if (gThDec.joinable()) gThDec.join();

        teardownLocked();
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
        if (!f || !w || !h) return "n/a";
        return fourccToStr(f) + " " + std::to_string(w) + "x" + std::to_string(h);
    }

} // namespace uvc