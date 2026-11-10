/* Native Windows C recorder: one Media Foundation camera + ESP32 serial CSI.
   Acquisition only; pose extraction is an offline step. See README.md. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#define CINTERFACE
#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <objbase.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <limits.h>
#include <math.h>

#include "csi_parse.h"

#define MAX_PORTS 16
#define RECORDER_VERSION "1.1.0"
#define RELEASE(p) do { if (p) { (p)->lpVtbl->Release(p); (p) = NULL; } } while (0)
#define TRY(call) do { hr = (call); if (FAILED(hr)) { \
    fprintf(stderr, "%s failed: HRESULT 0x%08lX\n", #call, (unsigned long)hr); \
    goto done; } } while (0)

typedef struct Session Session;

enum CameraPhase {
    CAMERA_STARTING, CAMERA_READING, CAMERA_PROCESSING,
    CAMERA_ENCODING, CAMERA_FINALIZING, CAMERA_FINISHED
};

typedef struct {
    Session *session;
    char name[24];
    HANDLE serial;
    HANDLE thread;
    HANDLE ready;
    FILE *csv;
    FILE *console;
    volatile LONG rows;
    volatile LONG malformed;
} Port;

typedef struct {
    Session *session;
    IMFMediaSource *source;
    IMFSourceReader *reader;
    IMFSinkWriter *writer;
    DWORD sink_stream;
    HANDLE thread;
    HANDLE ready;
    FILE *csv;
    char name[1024];
    UINT32 width, height, fps_num, fps_den;
    DWORD native_mode;
    volatile LONG frames;
    volatile LONG phase;
    LONGLONG first_source_pts;
    LONGLONG last_source_pts;
    int writing;
} Camera;

struct Session {
    HANDLE stop;
    volatile LONG recording;
    volatile LONG fatal;
    LONGLONG frequency;
    LONGLONG start_qpc;
    double seconds;
    double elapsed_at_stop;
    char start_utc[40];
    char directory[MAX_PATH];
    int nports;
    Port ports[MAX_PORTS];
    Camera camera;
};

typedef struct {
    int camera_index;
    int mode_index;
    int list_cameras;
    int list_modes;
    int camera_only;
    DWORD baud;
    unsigned countdown;
    double seconds;
    const char *output;
    int nports;
    char ports[MAX_PORTS][24];
} Options;

static Session *control_session;

static LONGLONG now_qpc(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

static void utc_now(char out[40])
{
    FILETIME ft;
    SYSTEMTIME st;
    ULARGE_INTEGER ticks;
    GetSystemTimePreciseAsFileTime(&ft);
    FileTimeToSystemTime(&ft, &st);
    ticks.LowPart = ft.dwLowDateTime;
    ticks.HighPart = ft.dwHighDateTime;
    (void)snprintf(out, 40, "%04u-%02u-%02uT%02u:%02u:%02u.%06uZ",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
        (unsigned)((ticks.QuadPart % UINT64_C(10000000)) / 10));
}

static int stopped(Session *s)
{
    return WaitForSingleObject(s->stop, 0) == WAIT_OBJECT_0;
}

static void fail(Session *s, const char *what, unsigned long code)
{
    InterlockedExchange(&s->fatal, 1);
    fprintf(stderr, "ERROR: %s (0x%08lX). Stopping and finalizing files.\n", what, code);
    SetEvent(s->stop);
}

static BOOL WINAPI console_control(DWORD event)
{
    if ((event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT) && control_session) {
        SetEvent(control_session->stop);
        return TRUE;
    }
    return FALSE;
}

static int path_for(Session *s, const char *leaf, char out[MAX_PATH])
{
    int n = snprintf(out, MAX_PATH, "%s\\%s", s->directory, leaf);
    return n > 0 && n < MAX_PATH;
}

static FILE *new_file(Session *s, const char *leaf)
{
    char path[MAX_PATH];
    FILE *f;
    if (!path_for(s, leaf, path)) return NULL;
    f = fopen(path, "wb");
    if (f) (void)setvbuf(f, NULL, _IOFBF, 1024 * 1024);
    return f;
}

static void json_string(FILE *f, const char *p)
{
    const unsigned char *s = (const unsigned char *)p;
    fputc('"', f);
    while (*s) {
        if (*s == '\\' || *s == '"') { fputc('\\', f); fputc(*s, f); }
        else if (*s < 32) fprintf(f, "\\u%04x", (unsigned)*s);
        else fputc(*s, f);
        ++s;
    }
    fputc('"', f);
}

static HRESULT cameras(IMFActivate ***devices, UINT32 *count)
{
    IMFAttributes *a = NULL;
    HRESULT hr;
    *devices = NULL;
    *count = 0;
    TRY(MFCreateAttributes(&a, 1));
    TRY(IMFAttributes_SetGUID(a, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
        &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID));
    TRY(MFEnumDeviceSources(a, devices, count));
done:
    RELEASE(a);
    return hr;
}

static void camera_name(IMFActivate *a, char *out, int size)
{
    WCHAR *wide = NULL;
    UINT32 length;
    (void)snprintf(out, (size_t)size, "Unnamed camera");
    if (SUCCEEDED(IMFActivate_GetAllocatedString(a,
        &MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &wide, &length))) {
        if (!WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, size, NULL, NULL))
            (void)snprintf(out, (size_t)size, "Camera name exceeds buffer");
    }
    CoTaskMemFree(wide);
}

static void dimensions(IMFMediaType *type, UINT32 *w, UINT32 *h,
                       UINT32 *num, UINT32 *den)
{
    UINT64 packed = 0;
    (void)IMFMediaType_GetUINT64(type, &MF_MT_FRAME_SIZE, &packed);
    *w = (UINT32)(packed >> 32); *h = (UINT32)packed;
    packed = 0;
    (void)IMFMediaType_GetUINT64(type, &MF_MT_FRAME_RATE, &packed);
    *num = (UINT32)(packed >> 32); *den = (UINT32)packed;
}

static HRESULT set_pair(IMFMediaType *t, const GUID *key, UINT32 a, UINT32 b)
{
    /* MFSetAttributeSize/Ratio packing, without C++ inline helper dependency. */
    return IMFMediaType_SetUINT64(t, key, ((UINT64)a << 32) | b);
}

static HRESULT open_camera(Camera *c, const Options *o)
{
    IMFActivate **devices = NULL;
    UINT32 count = 0, i;
    IMFAttributes *attributes = NULL;
    IMFMediaType *type = NULL, *best = NULL, *output = NULL;
    HRESULT hr;
    double best_score = 1e100;
    DWORD best_index = 0;

    TRY(cameras(&devices, &count));
    if (o->list_cameras) {
        for (i = 0; i < count; ++i) {
            char name[1024];
            camera_name(devices[i], name, sizeof name);
            printf("%u: %s\n", (unsigned)i, name);
        }
        if (!count) fprintf(stderr, "No camera found. Check the privacy shutter and Windows camera permissions.\n");
        hr = count ? S_OK : MF_E_NOT_FOUND;
        goto done;
    }
    if ((UINT32)o->camera_index >= count) {
        fprintf(stderr, "Camera %d does not exist. Run --list-cameras.\n", o->camera_index);
        hr = MF_E_NOT_FOUND;
        goto done;
    }
    camera_name(devices[o->camera_index], c->name, sizeof c->name);
    TRY(IMFActivate_ActivateObject(devices[o->camera_index], &IID_IMFMediaSource,
        (void **)&c->source));
    TRY(MFCreateAttributes(&attributes, 2));
    TRY(IMFAttributes_SetUINT32(attributes, &MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE));
    TRY(MFCreateSourceReaderFromMediaSource(c->source, attributes, &c->reader));
    TRY(IMFSourceReader_SetStreamSelection(c->reader, MF_SOURCE_READER_ALL_STREAMS, FALSE));
    TRY(IMFSourceReader_SetStreamSelection(c->reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE));

    for (i = 0; ; ++i) {
        UINT32 w, h, num, den;
        GUID subtype = {0};
        double fps, score;
        hr = IMFSourceReader_GetNativeMediaType(c->reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &type);
        if (hr == MF_E_NO_MORE_TYPES) { hr = S_OK; break; }
        if (FAILED(hr)) goto done;
        dimensions(type, &w, &h, &num, &den);
        (void)IMFMediaType_GetGUID(type, &MF_MT_SUBTYPE, &subtype);
        fps = den ? (double)num / den : 0;
        if (o->list_modes) {
            char fourcc[5];
            unsigned j;
            for (j = 0; j < 4; ++j) {
                unsigned ch = (subtype.Data1 >> (8 * j)) & 255;
                fourcc[j] = (char)(ch >= 32 && ch < 127 ? ch : '.');
            }
            fourcc[4] = 0;
            printf("%u: %ux%u, %.3f fps, subtype %s (0x%08lX)\n",
                (unsigned)i, (unsigned)w, (unsigned)h, fps, fourcc, (unsigned long)subtype.Data1);
        }
        /* Select a real camera mode. Do not synthesize/resample camera frames. */
        if (w && h && !(w % 2) && !(h % 2) && fps > 0 && fps <= 120) {
            score = fabs((double)w - 1280) + fabs((double)h - 720) + fabs(fps - 30) * 100;
            if (IsEqualGUID(&subtype, &MFVideoFormat_NV12)) score -= 2;
            if (o->mode_index >= 0) score = (int)i == o->mode_index ? -1e10 : 1e100;
            if (score < best_score) {
                RELEASE(best);
                best = type;
                IMFMediaType_AddRef(best);
                best_score = score;
                best_index = i;
            }
        }
        RELEASE(type);
    }
    if (o->list_modes) goto done;
    if (!best) {
        fprintf(stderr, "No usable camera mode. Try --list-modes and select an even-sized mode <=120 fps.\n");
        hr = MF_E_INVALIDMEDIATYPE;
        goto done;
    }
    c->native_mode = best_index;
    dimensions(best, &c->width, &c->height, &c->fps_num, &c->fps_den);
    TRY(IMFSourceReader_SetCurrentMediaType(c->reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, best));
    TRY(MFCreateMediaType(&output));
    TRY(IMFMediaType_SetGUID(output, &MF_MT_MAJOR_TYPE, &MFMediaType_Video));
    TRY(IMFMediaType_SetGUID(output, &MF_MT_SUBTYPE, &MFVideoFormat_NV12));
    /* Set only the pixel layout; retain the selected native size and frame rate. */
    TRY(IMFSourceReader_SetCurrentMediaType(c->reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, output));
    RELEASE(output);
    TRY(IMFSourceReader_GetCurrentMediaType(c->reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM, &output));
    dimensions(output, &c->width, &c->height, &c->fps_num, &c->fps_den);
    if (!c->width || !c->height || !c->fps_num || !c->fps_den) {
        hr = MF_E_INVALIDMEDIATYPE;
        goto done;
    }
    printf("Camera: %s; mode %lu; %ux%u at %.3f fps\n", c->name,
        (unsigned long)c->native_mode, (unsigned)c->width, (unsigned)c->height,
        (double)c->fps_num / c->fps_den);
done:
    RELEASE(type);
    RELEASE(best);
    RELEASE(output);
    RELEASE(attributes);
    for (i = 0; i < count; ++i) RELEASE(devices[i]);
    CoTaskMemFree(devices);
    return hr;
}

static HRESULT open_video_writer(Camera *c)
{
    Session *s = c->session;
    char path[MAX_PATH];
    WCHAR wide[MAX_PATH];
    IMFAttributes *attributes = NULL;
    IMFMediaType *encoded = NULL, *input = NULL;
    HRESULT hr = E_FAIL;
    if (!path_for(s, "video.mp4", path) ||
        !MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH)) return E_INVALIDARG;
    TRY(MFCreateAttributes(&attributes, 2));
    /* Prefer the software H.264 encoder while validating capture on new hardware. */
    TRY(IMFAttributes_SetUINT32(attributes, &MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, FALSE));
    TRY(IMFAttributes_SetUINT32(attributes, &MF_SINK_WRITER_DISABLE_THROTTLING, TRUE));
    TRY(MFCreateSinkWriterFromURL(wide, NULL, attributes, &c->writer));
    TRY(MFCreateMediaType(&encoded));
    TRY(IMFMediaType_SetGUID(encoded, &MF_MT_MAJOR_TYPE, &MFMediaType_Video));
    TRY(IMFMediaType_SetGUID(encoded, &MF_MT_SUBTYPE, &MFVideoFormat_H264));
    TRY(IMFMediaType_SetUINT32(encoded, &MF_MT_AVG_BITRATE, 4000000));
    TRY(IMFMediaType_SetUINT32(encoded, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    TRY(set_pair(encoded, &MF_MT_FRAME_SIZE, c->width, c->height));
    TRY(set_pair(encoded, &MF_MT_FRAME_RATE, c->fps_num, c->fps_den));
    TRY(set_pair(encoded, &MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
    TRY(IMFSinkWriter_AddStream(c->writer, encoded, &c->sink_stream));
    TRY(IMFSourceReader_GetCurrentMediaType(c->reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM, &input));
    TRY(IMFSinkWriter_SetInputMediaType(c->writer, c->sink_stream, input, NULL));
    TRY(IMFSinkWriter_BeginWriting(c->writer));
    c->writing = 1;
done:
    RELEASE(attributes);
    RELEASE(encoded);
    RELEASE(input);
    return hr;
}

static HANDLE open_serial(const char *port_name, DWORD baud)
{
    char device[40];
    HANDLE h;
    DCB dcb = {0};
    COMMTIMEOUTS timeout = {0};
    (void)snprintf(device, sizeof device, "\\\\.\\%s", port_name);
    h = CreateFileA(device, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return h;
    dcb.DCBlength = sizeof dcb;
    if (!GetCommState(h, &dcb)) goto error;
    dcb.BaudRate = baud;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_DISABLE;
    dcb.fRtsControl = RTS_CONTROL_DISABLE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fErrorChar = FALSE;
    dcb.fNull = FALSE;
    dcb.fAbortOnError = FALSE;
    if (!SetCommState(h, &dcb)) goto error;
    timeout.ReadIntervalTimeout = MAXDWORD;
    timeout.ReadTotalTimeoutConstant = 20;
    if (!SetCommTimeouts(h, &timeout) || !SetupComm(h, 65536, 1024)) goto error;
    return h;
error:
    {
        DWORD code = GetLastError();
        CloseHandle(h);
        SetLastError(code);
        return INVALID_HANDLE_VALUE;
    }
}

static DWORD WINAPI serial_worker(void *argument)
{
    Port *p = (Port *)argument;
    Session *s = p->session;
    char bytes[8192], line[CSI_LINE_CAP], scratch[CSI_LINE_CAP];
    size_t used = 0;
    int discard = 0;
    LONGLONG last_flush = now_qpc();
    while (!stopped(s)) {
        DWORD errors, received = 0, wanted, j;
        COMSTAT status;
        LONGLONG stamp;
        char utc[40];
        if (!ClearCommError(p->serial, &errors, &status)) {
            fail(s, p->name, GetLastError()); break;
        }
        if (errors) { fail(s, "Serial driver reported a communication error", errors); break; }
        wanted = status.cbInQue ? status.cbInQue : 1;
        if (wanted > sizeof bytes) wanted = sizeof bytes;
        if (!ReadFile(p->serial, bytes, wanted, &received, NULL)) {
            if (!stopped(s)) fail(s, p->name, GetLastError());
            break;
        }
        stamp = now_qpc();
        utc_now(utc);
        for (j = 0; j < received; ++j) {
            unsigned char ch = (unsigned char)bytes[j];
            if (ch == '\r') continue;
            if (ch == '\n') {
                int valid;
                if (discard) { discard = 0; used = 0; continue; }
                line[used] = '\0';
                valid = csi_validate(line, scratch, sizeof scratch);
                if (valid == 1) {
                    SetEvent(p->ready);
                    if (InterlockedCompareExchange(&s->recording, 0, 0)) {
                        double elapsed = csi_elapsed(stamp, s->start_qpc, s->frequency);
                        if (elapsed >= 0 && elapsed < s->seconds) {
                            fprintf(p->csv, "%s,%" PRId64 ",%.9f,%s,%s\n",
                                utc, (int64_t)stamp, elapsed, p->name, line);
                            if (ferror(p->csv)) { fail(s, "Writing CSI CSV", ERROR_WRITE_FAULT); break; }
                            InterlockedIncrement(&p->rows);
                        }
                    }
                } else if (valid < 0) {
                    InterlockedIncrement(&p->malformed);
                    fprintf(p->console, "%s QPC=%" PRId64 " MALFORMED %s\n", utc, (int64_t)stamp, line);
                } else if (used) {
                    fprintf(p->console, "%s QPC=%" PRId64 " %s\n", utc, (int64_t)stamp, line);
                }
                used = 0;
            } else if (!discard) {
                if (ch < 32 || ch > 126 || used + 1 >= sizeof line) {
                    discard = 1;
                    used = 0;
                    InterlockedIncrement(&p->malformed);
                } else line[used++] = (char)ch;
            }
        }
        if (stamp - last_flush >= s->frequency) {
            if (fflush(p->csv) || fflush(p->console)) {
                fail(s, "Flushing serial files", ERROR_WRITE_FAULT); break;
            }
            last_flush = stamp;
        }
    }
    if (fflush(p->csv) || fflush(p->console)) fail(s, "Flushing serial files", ERROR_WRITE_FAULT);
    return 0;
}

static HRESULT encoder_sample(IMFSample *source, LONGLONG pts,
                              LONGLONG duration, int first, IMFSample **result)
{
    IMFMediaBuffer *buffer = NULL;
    IMFSample *copy = NULL;
    DWORD length = 0;
    HRESULT hr;
    *result = NULL;
    /* The encoder must not retain a camera-owned sample or its buffer pool.
       Do not copy device-specific attributes/timestamps into the MP4 sample. */
    TRY(IMFSample_GetTotalLength(source, &length));
    if (!length) { hr = MF_E_BUFFERTOOSMALL; goto done; }
    TRY(MFCreateMemoryBuffer(length, &buffer));
    TRY(IMFSample_CopyToBuffer(source, buffer));
    TRY(IMFMediaBuffer_SetCurrentLength(buffer, length));
    TRY(MFCreateSample(&copy));
    TRY(IMFSample_AddBuffer(copy, buffer));
    TRY(IMFSample_SetSampleTime(copy, pts));
    TRY(IMFSample_SetSampleDuration(copy, duration));
    if (first) TRY(IMFSample_SetUINT32(copy, &MFSampleExtension_Discontinuity, TRUE));
    *result = copy;
    copy = NULL;
done:
    RELEASE(copy);
    RELEASE(buffer);
    return hr;
}

static DWORD WINAPI camera_worker(void *argument)
{
    Camera *c = (Camera *)argument;
    Session *s = c->session;
    HRESULT initialized = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IMFSample *sample = NULL;
    LONGLONG last_flush = now_qpc();
    if (FAILED(initialized)) { fail(s, "Camera thread COM initialization", (unsigned long)initialized); return 1; }
    while (!stopped(s)) {
        DWORD flags = 0, stream;
        LONGLONG pts = 0, duration, stamp;
        char utc[40];
        HRESULT hr;
        InterlockedExchange(&c->phase, CAMERA_READING);
        hr = IMFSourceReader_ReadSample(c->reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM,
            0, &stream, &flags, &pts, &sample);
        stamp = now_qpc();
        InterlockedExchange(&c->phase, CAMERA_PROCESSING);
        utc_now(utc);
        if (stopped(s)) { RELEASE(sample); break; }
        if (FAILED(hr)) { fail(s, "Reading camera", (unsigned long)hr); break; }
        if (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM)) {
            fail(s, "Camera stream ended or reported an error", flags);
            RELEASE(sample); break;
        }
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            IMFMediaType *changed = NULL;
            UINT32 w = 0, h = 0, num = 0, den = 0;
            GUID subtype = {0};
            hr = IMFSourceReader_GetCurrentMediaType(c->reader,
                MF_SOURCE_READER_FIRST_VIDEO_STREAM, &changed);
            if (SUCCEEDED(hr)) {
                dimensions(changed, &w, &h, &num, &den);
                hr = IMFMediaType_GetGUID(changed, &MF_MT_SUBTYPE, &subtype);
            }
            RELEASE(changed);
            if (FAILED(hr) || !IsEqualGUID(&subtype, &MFVideoFormat_NV12) ||
                w != c->width || h != c->height || num != c->fps_num || den != c->fps_den) {
                fail(s, "Camera changed its recording format", flags);
                RELEASE(sample); break;
            }
        }
        if (!sample) continue;
        SetEvent(c->ready);
        if (InterlockedCompareExchange(&s->recording, 0, 0)) {
            double elapsed = csi_elapsed(stamp, s->start_qpc, s->frequency);
            if (elapsed >= 0 && elapsed < s->seconds) {
                LONGLONG video_pts;
                if (!c->frames) c->first_source_pts = pts;
                else if (pts <= c->last_source_pts) {
                    fail(s, "Non-increasing camera timestamps", ERROR_INVALID_DATA);
                    RELEASE(sample); break;
                }
                video_pts = pts - c->first_source_pts;
                duration = (LONGLONG)((UINT64)10000000 * c->fps_den / c->fps_num);
                {
                    LONGLONG reported;
                    if (SUCCEEDED(IMFSample_GetSampleDuration(sample, &reported)) && reported > 0)
                        duration = reported;
                }
                {
                    IMFSample *owned = NULL;
                    hr = encoder_sample(sample, video_pts, duration, !c->frames, &owned);
                    /* Release camera ownership before any encoder call can block. */
                    RELEASE(sample);
                    InterlockedExchange(&c->phase, CAMERA_ENCODING);
                    if (SUCCEEDED(hr)) hr = IMFSinkWriter_WriteSample(c->writer, c->sink_stream, owned);
                    RELEASE(owned);
                    InterlockedExchange(&c->phase, CAMERA_PROCESSING);
                }
                if (FAILED(hr)) {
                    fail(s, "Encoding camera video", (unsigned long)hr);
                    RELEASE(sample); break;
                }
                fprintf(c->csv, "%ld,%s,%" PRId64 ",%.9f,%" PRId64 ",%" PRId64 ",%" PRId64 "\n",
                    (long)c->frames, utc, (int64_t)stamp, elapsed, (int64_t)pts,
                    (int64_t)video_pts, (int64_t)duration);
                if (ferror(c->csv)) { fail(s, "Writing frame timestamps", ERROR_WRITE_FAULT); RELEASE(sample); break; }
                c->last_source_pts = pts;
                InterlockedIncrement(&c->frames);
                if (stamp - last_flush >= s->frequency) {
                    if (fflush(c->csv)) {
                        fail(s, "Flushing frame timestamps", ERROR_WRITE_FAULT); break;
                    }
                    last_flush = stamp;
                }
            }
        }
        RELEASE(sample);
    }
    RELEASE(sample);
    if (c->writing) {
        HRESULT hr;
        InterlockedExchange(&c->phase, CAMERA_FINALIZING);
        printf("Finalizing MP4 (%ld frames)...\n", (long)c->frames);
        fflush(stdout);
        if (fflush(c->csv)) fail(s, "Flushing frame timestamps", ERROR_WRITE_FAULT);
        hr = IMFSinkWriter_Finalize(c->writer);
        c->writing = 0;
        if (FAILED(hr)) fail(s, "Finalizing MP4", (unsigned long)hr);
        else { puts("MP4 finalized successfully."); fflush(stdout); }
    }
    if (fflush(c->csv)) fail(s, "Flushing frame timestamps", ERROR_WRITE_FAULT);
    InterlockedExchange(&c->phase, CAMERA_FINISHED);
    CoUninitialize();
    return 0;
}

static int write_metadata(Session *s, const Options *o, const char *state)
{
    FILE *f = new_file(s, "metadata.json");
    int i, bad;
    if (!f) return 0;
    fprintf(f, "{\n  \"schema\": \"csi-camera-c/1\",\n  \"recorder_version\": \"" RECORDER_VERSION "\",\n  \"status\": ");
    json_string(f, state);
    fprintf(f, ",\n  \"sync_status\": \"uncalibrated_host_receipt_timestamps\",\n");
    fprintf(f, "  \"encoder\": \"software_h264\",\n");
    fprintf(f, "  \"qpc_frequency_hz\": %" PRId64 ",\n  \"start_qpc_ticks\": %" PRId64 ",\n",
        (int64_t)s->frequency, (int64_t)s->start_qpc);
    fprintf(f, "  \"start_utc\": "); json_string(f, s->start_utc);
    fprintf(f, ",\n  \"requested_seconds\": %.6f,\n  \"elapsed_seconds\": %.6f,\n",
        s->seconds, s->elapsed_at_stop);
    fprintf(f, "  \"baud\": %lu,\n  \"camera_name\": ", (unsigned long)o->baud);
    json_string(f, s->camera.name);
    fprintf(f, ",\n  \"camera_index\": %d,\n  \"native_camera_mode\": %lu,\n",
        o->camera_index, (unsigned long)s->camera.native_mode);
    fprintf(f, "  \"width\": %u,\n  \"height\": %u,\n  \"fps_numerator\": %u,\n  \"fps_denominator\": %u,\n",
        (unsigned)s->camera.width, (unsigned)s->camera.height,
        (unsigned)s->camera.fps_num, (unsigned)s->camera.fps_den);
    fprintf(f, "  \"camera_pts_timebase\": \"1/10000000\",\n  \"video_frames\": %ld,\n  \"ports\": [",
        (long)s->camera.frames);
    for (i = 0; i < s->nports; ++i) {
        fprintf(f, "%s\n    {\"port\": ", i ? "," : "");
        json_string(f, s->ports[i].name);
        fprintf(f, ", \"rows\": %ld, \"malformed_lines_including_warmup\": %ld}",
            (long)s->ports[i].rows, (long)s->ports[i].malformed);
    }
    fprintf(f, "\n  ]\n}\n");
    bad = ferror(f);
    if (fclose(f)) bad = 1;
    return !bad;
}

static void usage(void)
{
    puts("CSI + webcam recorder " RECORDER_VERSION " (native Windows C)\n"
         "  capture.exe --list-cameras\n"
         "  capture.exe --camera 0 --list-modes\n"
         "  capture.exe --camera-only --seconds 10 --output camera_test_001\n"
         "  capture.exe --ports COM3 COM5 COM7 --camera 0 --seconds 30 --output session_001\n\n"
         "Options: --camera N --mode N --ports COM... --baud 460800\n"
         "         --seconds 300 --countdown 10 --output NEW_FOLDER\n"
         "         --camera-only --list-cameras --list-modes --version --help\n"
         "Ctrl+C stops and finalizes. Existing output folders are never overwritten.");
}

static int integer_option(const char *text, long minimum, long maximum, long *result)
{
    char *end;
    long v;
    if (!*text) return 0;
    v = strtol(text, &end, 10);
    if (*end || v < minimum || v > maximum) return 0;
    *result = v;
    return 1;
}

static int parse_options(int argc, char **argv, Options *o)
{
    int i;
    memset(o, 0, sizeof *o);
    o->mode_index = -1;
    o->baud = 460800;
    o->countdown = 10;
    o->seconds = 300;
    o->output = "session_001";
    for (i = 1; i < argc; ++i) {
        const char *a = argv[i];
        long v;
        if (!strcmp(a, "--version")) { puts(RECORDER_VERSION); return 0; }
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(); return 0; }
        if (!strcmp(a, "--list-cameras")) o->list_cameras = 1;
        else if (!strcmp(a, "--list-modes")) o->list_modes = 1;
        else if (!strcmp(a, "--camera-only")) o->camera_only = 1;
        else if (!strcmp(a, "--ports")) {
            int first = o->nports;
            while (i + 1 < argc && strncmp(argv[i + 1], "--", 2)) {
                const char *name = argv[++i];
                int j;
                if (o->nports >= MAX_PORTS || _strnicmp(name, "COM", 3) ||
                    !integer_option(name + 3, 1, 65535, &v)) goto error;
                (void)snprintf(o->ports[o->nports], sizeof o->ports[0], "COM%ld", v);
                for (j = 0; j < o->nports; ++j)
                    if (!strcmp(o->ports[j], o->ports[o->nports])) goto error;
                ++o->nports;
            }
            if (o->nports == first) goto error;
        } else {
            const char *value;
            if (i + 1 >= argc) goto error;
            value = argv[++i];
            if (!strcmp(a, "--camera")) {
                if (!integer_option(value, 0, 1000, &v)) goto error;
                o->camera_index = (int)v;
            } else if (!strcmp(a, "--mode")) {
                if (!integer_option(value, 0, 10000, &v)) goto error;
                o->mode_index = (int)v;
            } else if (!strcmp(a, "--baud")) {
                if (!integer_option(value, 1, 4000000, &v)) goto error;
                o->baud = (DWORD)v;
            } else if (!strcmp(a, "--countdown")) {
                if (!integer_option(value, 0, 3600, &v)) goto error;
                o->countdown = (unsigned)v;
            } else if (!strcmp(a, "--seconds")) {
                char *end;
                o->seconds = strtod(value, &end);
                if (end == value || *end || !isfinite(o->seconds) || o->seconds <= 0 || o->seconds > 86400) goto error;
            } else if (!strcmp(a, "--output")) o->output = value;
            else goto error;
        }
    }
    if (o->camera_only && o->nports) goto error;
    if (!o->camera_only && !o->nports) {
        strcpy(o->ports[0], "COM3"); strcpy(o->ports[1], "COM5"); strcpy(o->ports[2], "COM7");
        o->nports = 3;
    }
    return 1;
error:
    fprintf(stderr, "Invalid or incomplete option near %s. Run --help.\n", argv[i]);
    return -1;
}

static void close_file(Session *s, FILE **f)
{
    if (*f) {
        if (fclose(*f)) fail(s, "Closing output file", ERROR_WRITE_FAULT);
        *f = NULL;
    }
}

int main(int argc, char **argv)
{
    Options o;
    Session s;
    LARGE_INTEGER frequency;
    HRESULT hr;
    HANDLE workers[MAX_PORTS + 1];
    int nworkers = 0, i, parsed, directory_created = 0, recorded = 0;
    int co_started = 0, mf_started = 0;
    DWORD n;
    LONGLONG warmup_started;
    const char *result_state = "failed";

    parsed = parse_options(argc, argv, &o);
    if (parsed <= 0) return parsed < 0 ? 2 : 0;
    puts("CSI camera recorder v" RECORDER_VERSION);
    memset(&s, 0, sizeof s);
    s.seconds = o.seconds;
    s.camera.session = &s;
    for (i = 0; i < MAX_PORTS; ++i) s.ports[i].serial = INVALID_HANDLE_VALUE;
    QueryPerformanceFrequency(&frequency);
    s.frequency = frequency.QuadPart;
    if (!s.frequency) { fputs("QPC clock unavailable.\n", stderr); return 2; }
    s.stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!s.stop) return 2;
    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) { fail(&s, "COM initialization", (unsigned long)hr); goto cleanup; }
    co_started = 1;
    hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(hr)) { fail(&s, "Media Foundation startup", (unsigned long)hr); goto cleanup; }
    mf_started = 1;
    hr = open_camera(&s.camera, &o);
    if (FAILED(hr)) {
        fail(&s, "Opening camera; check Windows camera permissions / camera mode", (unsigned long)hr);
        goto cleanup;
    }
    if (o.list_cameras || o.list_modes) { result_state = "listed"; goto cleanup; }

    n = GetFullPathNameA(o.output, MAX_PATH, s.directory, NULL);
    if (!n || n >= MAX_PATH - 32) { fail(&s, "Output path is too long", ERROR_BAD_PATHNAME); goto cleanup; }
    if (!CreateDirectoryA(s.directory, NULL)) {
        fail(&s, "Cannot create output folder; use a NEW name and an existing parent folder", GetLastError());
        goto cleanup;
    }
    directory_created = 1;
    s.camera.csv = new_file(&s, "frames.csv");
    s.camera.ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!s.camera.csv || !s.camera.ready) { fail(&s, "Opening frame log", ERROR_OPEN_FAILED); goto cleanup; }
    fputs("frame_index,host_time_utc,host_qpc_ticks,host_elapsed_s,source_pts_100ns,video_pts_100ns,duration_100ns\n", s.camera.csv);
    hr = open_video_writer(&s.camera);
    if (FAILED(hr)) { fail(&s, "Opening H.264 MP4 encoder", (unsigned long)hr); goto cleanup; }

    s.nports = o.camera_only ? 0 : o.nports;
    for (i = 0; i < s.nports; ++i) {
        Port *p = &s.ports[i];
        char leaf[80];
        p->session = &s;
        strcpy(p->name, o.ports[i]);
        p->ready = CreateEventW(NULL, TRUE, FALSE, NULL);
        p->serial = open_serial(p->name, o.baud);
        if (!p->ready || p->serial == INVALID_HANDLE_VALUE) { fail(&s, p->name, GetLastError()); goto cleanup; }
        (void)snprintf(leaf, sizeof leaf, "csi_%s.csv", p->name);
        p->csv = new_file(&s, leaf);
        (void)snprintf(leaf, sizeof leaf, "%s_console.log", p->name);
        p->console = new_file(&s, leaf);
        if (!p->csv || !p->console) { fail(&s, "Opening serial output files", ERROR_OPEN_FAILED); goto cleanup; }
        fputs("host_time_utc,host_qpc_ticks,host_elapsed_s,serial_port,type,rx_board,tx_board,kind,session,sequence,rx_boot,radio_time_us,uptime_us,rssi,channel,len,first_word_invalid,data\n", p->csv);
    }
    if (!write_metadata(&s, &o, "warming_up")) { fail(&s, "Writing metadata", ERROR_WRITE_FAULT); goto cleanup; }
    control_session = &s;
    SetConsoleCtrlHandler(console_control, TRUE);
    for (i = 0; i < s.nports; ++i) {
        s.ports[i].thread = CreateThread(NULL, 0, serial_worker, &s.ports[i], 0, NULL);
        if (!s.ports[i].thread) { fail(&s, "Starting serial thread", GetLastError()); goto stop_workers; }
        workers[nworkers++] = s.ports[i].thread;
    }
    s.camera.thread = CreateThread(NULL, 0, camera_worker, &s.camera, 0, NULL);
    if (!s.camera.thread) { fail(&s, "Starting camera thread", GetLastError()); goto stop_workers; }
    workers[nworkers++] = s.camera.thread;
    puts("Warming up. Waiting for camera frames and valid CSI on every requested port...");
    warmup_started = now_qpc();
    while (!stopped(&s)) {
        int all = WaitForSingleObject(s.camera.ready, 0) == WAIT_OBJECT_0;
        for (i = 0; i < s.nports; ++i)
            if (WaitForSingleObject(s.ports[i].ready, 0) != WAIT_OBJECT_0) all = 0;
        if (all) break;
        if (csi_elapsed(now_qpc(), warmup_started, s.frequency) > 30) {
            for (i = 0; i < s.nports; ++i)
                if (WaitForSingleObject(s.ports[i].ready, 0) != WAIT_OBJECT_0)
                    fprintf(stderr, "No valid CSI from %s.\n", s.ports[i].name);
            fail(&s, "Warmup timed out after 30 seconds", WAIT_TIMEOUT);
            break;
        }
        WaitForSingleObject(s.stop, 100);
    }
    if (stopped(&s)) goto stop_workers;
    for (n = o.countdown; n > 0 && !stopped(&s); --n) {
        printf("Recording starts in %lu...\n", (unsigned long)n);
        WaitForSingleObject(s.stop, 1000);
    }
    if (stopped(&s)) goto stop_workers;
    utc_now(s.start_utc);
    s.start_qpc = now_qpc();
    InterlockedExchange(&s.recording, 1);
    recorded = 1;
    if (!write_metadata(&s, &o, "recording")) { fail(&s, "Writing start metadata", ERROR_WRITE_FAULT); goto stop_workers; }
    printf("RECORDING for %.3f seconds. Output: %s\n", s.seconds, s.directory);
    {
        double next_report = 2;
        while (!stopped(&s)) {
            double elapsed = csi_elapsed(now_qpc(), s.start_qpc, s.frequency);
            if (elapsed >= s.seconds) break;
            if (elapsed >= next_report) {
                printf("%.1f s: %ld video frames", elapsed, (long)s.camera.frames);
                for (i = 0; i < s.nports; ++i)
                    printf("; %s=%ld CSI rows", s.ports[i].name, (long)s.ports[i].rows);
                putchar('\n');
                next_report = elapsed + 2;
            }
            WaitForSingleObject(s.stop, 50);
        }
    }
    s.elapsed_at_stop = csi_elapsed(now_qpc(), s.start_qpc, s.frequency);
    printf("Capture window ended at %.3f seconds; %ld video frames submitted.\n",
        s.elapsed_at_stop, (long)s.camera.frames);
    result_state = stopped(&s) ? "stopped_early" : "complete";

stop_workers:
    InterlockedExchange(&s.recording, 0);
    SetEvent(s.stop);
    /* Keep the source alive until the reader/encoder thread has exited.
       A healthy pending camera read completes on the next arriving frame.
       Shutting the source down concurrently here could strand that read or
       invalidate camera-backed encoder input while Finalize is draining it. */
    puts("Stopping capture and waiting for MP4 finalization...");
    fflush(stdout);
    if (nworkers && WaitForMultipleObjects((DWORD)nworkers, workers, TRUE, 30000) != WAIT_OBJECT_0) {
        static const char *phases[] = {"starting", "reading camera", "processing frame",
            "encoding frame", "finalizing MP4", "finished"};
        LONG phase = InterlockedCompareExchange(&s.camera.phase, 0, 0);
        fprintf(stderr, "Worker shutdown timed out. Camera phase: %s. MP4 may be incomplete.\n",
            phase >= 0 && phase <= CAMERA_FINISHED ? phases[phase] : "unknown");
        InterlockedExchange(&s.fatal, 1);
        (void)write_metadata(&s, &o, "shutdown_timeout");
        /* Do not free COM objects or files still owned by running workers. */
        ExitProcess(2);
    }
    if (recorded) {
        if (!s.camera.frames) fail(&s, "No video frames were saved", ERROR_NO_DATA);
        for (i = 0; i < s.nports; ++i)
            if (!s.ports[i].rows) fail(&s, "A serial stream saved no CSI rows", ERROR_NO_DATA);
    }

cleanup:
    if (s.stop) SetEvent(s.stop);
    if (control_session) {
        SetConsoleCtrlHandler(console_control, FALSE);
        control_session = NULL;
    }
    /* Init failures can arrive here before any worker has been started. */
    if (s.camera.writing) {
        hr = IMFSinkWriter_Finalize(s.camera.writer);
        s.camera.writing = 0;
        if (FAILED(hr) && recorded) InterlockedExchange(&s.fatal, 1);
    }
    close_file(&s, &s.camera.csv);
    for (i = 0; i < MAX_PORTS; ++i) {
        close_file(&s, &s.ports[i].csv);
        close_file(&s, &s.ports[i].console);
        if (s.ports[i].serial != INVALID_HANDLE_VALUE) CloseHandle(s.ports[i].serial);
        if (s.ports[i].thread) CloseHandle(s.ports[i].thread);
        if (s.ports[i].ready) CloseHandle(s.ports[i].ready);
    }
    if (s.camera.thread) CloseHandle(s.camera.thread);
    if (s.camera.ready) CloseHandle(s.camera.ready);
    RELEASE(s.camera.writer);
    RELEASE(s.camera.reader);
    if (s.camera.source) (void)IMFMediaSource_Shutdown(s.camera.source);
    RELEASE(s.camera.source);
    if (directory_created) {
        if (s.fatal) result_state = "failed";
        if (!write_metadata(&s, &o, result_state)) {
            fprintf(stderr, "Could not save final metadata. Treat this session as incomplete.\n");
            InterlockedExchange(&s.fatal, 1);
        }
        printf("Session %s: %ld frames saved in %s\n", result_state, (long)s.camera.frames, s.directory);
        puts("Timing is based on host receipt; camera/serial capture latency is not calibrated yet.");
    }
    if (mf_started) MFShutdown();
    if (co_started) CoUninitialize();
    CloseHandle(s.stop);
    return s.fatal ? 1 : 0;
}
