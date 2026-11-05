// Meta Quest immersive host (docs/quest/README.md, milestones 1 and 2).
//
// One render thread owns an EGL/GLES context and an OpenXR session. projectM draws into the
// swapchain texture through an FBO, and that swapchain is submitted every frame as an equirect
// composition layer (a dome around the viewer). PCM from the headset microphone arrives from the
// Java AudioRecord thread through nativePushPcm and is handed to projectM_pcm_add_float on the
// render thread, so projectM is only ever touched there.
//
// Compiled only with -DPROJECTMTV_QUEST=ON (core/build.gradle, -PprojectmQuest=true); the TV and
// phone core never contains it.
#include <jni.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "projectM-4/projectM.h"

#define LOG_TAG "ProjectMQuest"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define QUEST_FN(name) Java_com_example_projectm_visualizer_quest_QuestActivity_##name

namespace {

constexpr int kTextureSize = 2048;           // square projectM target; adaptive scaling is a later milestone
constexpr float kHalfSpan = 1.0471976f;      // dome spans +-60 degrees: 120x120, so projectM's pixels stay square
constexpr float kDomeRadius = 10.f;          // metres; far enough that head movement shows no parallax
constexpr double kMaxFps = 72.0;             // the compositor repeats the last image above this
constexpr double kPresetSeconds = 30.0;
constexpr size_t kPcmCapFrames = 16384;      // ~340 ms at 48 kHz; older audio is dropped, never queued up

#define XR_TRY(call)                                                              \
    do {                                                                          \
        XrResult xrResult_ = (call);                                              \
        if (XR_FAILED(xrResult_)) {                                               \
            LOGE("%s failed: XrResult %d", #call, static_cast<int>(xrResult_));   \
            error_ = #call " failed";                                             \
            return false;                                                         \
        }                                                                         \
    } while (0)

double Now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::string ReadAsset(AAssetManager* assets, const std::string& path) {
    AAsset* asset = AAssetManager_open(assets, path.c_str(), AASSET_MODE_BUFFER);
    if (!asset) return std::string();
    std::string data(static_cast<const char*>(AAsset_getBuffer(asset)), static_cast<size_t>(AAsset_getLength(asset)));
    AAsset_close(asset);
    return data;
}

// projectM only loads textures from real folders: unpack the bundled texture pack once.
void InstallTextures(AAssetManager* assets, const std::string& directory) {
    mkdir(directory.c_str(), 0700);
    AAssetDir* dir = AAssetManager_openDir(assets, "textures");
    if (!dir) return;
    while (const char* name = AAssetDir_getNextFileName(dir)) {
        const std::string target = directory + "/" + name;
        struct stat info;
        AAsset* asset = AAssetManager_open(assets, (std::string("textures/") + name).c_str(), AASSET_MODE_BUFFER);
        if (!asset) continue;
        if (stat(target.c_str(), &info) != 0 || info.st_size != AAsset_getLength(asset)) {
            if (FILE* out = fopen(target.c_str(), "wb")) {
                fwrite(AAsset_getBuffer(asset), 1, static_cast<size_t>(AAsset_getLength(asset)), out);
                fclose(out);
            }
        }
        AAsset_close(asset);
    }
    AAssetDir_close(dir);
}

class QuestHost {
public:
    // activity and assetManager are global references, owned by the host until Stop() returns.
    QuestHost(JavaVM* vm, jobject activity, jobject assetManager, AAssetManager* assets, std::string textureDir)
        : vm_(vm), activity_(activity), assetManager_(assetManager), assets_(assets),
          textureDir_(std::move(textureDir)) {}

    void Start() { thread_ = std::thread([this] { Run(); }); }
    void SetResumed(bool resumed) { resumed_ = resumed; }
    void Stop(JNIEnv* env) {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        env->DeleteGlobalRef(activity_);
        env->DeleteGlobalRef(assetManager_);
    }

    // Any thread: mono float PCM from the headset microphone, -1..1.
    void PushPcm(const float* samples, size_t count) {
        std::lock_guard<std::mutex> lock(pcmMutex_);
        pcm_.insert(pcm_.end(), samples, samples + count);
        if (pcm_.size() > kPcmCapFrames) pcm_.erase(pcm_.begin(), pcm_.end() - kPcmCapFrames);
    }

private:
    void Run() {
        JNIEnv* env = nullptr;
        vm_->AttachCurrentThread(&env, nullptr);
        if (InitEgl() && InitXr() && InitProjectM()) {
            Loop();
        }
        const bool requested = stop_;  // the activity is already finishing when it asked us to stop
        Shutdown();
        if (!requested) NotifyExit(env);
        vm_->DetachCurrentThread();
    }

    // --- EGL: a context for the OpenXR session; frames never go to a window surface ---
    bool InitEgl() {
        display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (display_ == EGL_NO_DISPLAY || !eglInitialize(display_, nullptr, nullptr)) {
            error_ = "EGL unavailable";
            return false;
        }
        const EGLint configAttributes[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                                           EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                           EGL_NONE};
        EGLint configs = 0;
        if (!eglChooseConfig(display_, configAttributes, &config_, 1, &configs) || configs < 1) {
            error_ = "No OpenGL ES 3 EGL config";
            return false;
        }
        const EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        context_ = eglCreateContext(display_, config_, EGL_NO_CONTEXT, contextAttributes);
        const EGLint surfaceAttributes[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
        surface_ = eglCreatePbufferSurface(display_, config_, surfaceAttributes);
        if (context_ == EGL_NO_CONTEXT || surface_ == EGL_NO_SURFACE ||
            !eglMakeCurrent(display_, surface_, surface_, context_)) {
            error_ = "OpenGL ES 3 context unavailable";
            return false;
        }
        return true;
    }

    // --- OpenXR: instance, session, local space and one equirect swapchain ---
    bool InitXr() {
        PFN_xrInitializeLoaderKHR initializeLoader = nullptr;
        XR_TRY(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                                     reinterpret_cast<PFN_xrVoidFunction*>(&initializeLoader)));
        XrLoaderInitInfoAndroidKHR loaderInfo{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
        loaderInfo.applicationVM = vm_;
        loaderInfo.applicationContext = activity_;
        XR_TRY(initializeLoader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&loaderInfo)));

        const char* extensions[] = {XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
                                    XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
                                    XR_KHR_COMPOSITION_LAYER_EQUIRECT2_EXTENSION_NAME};
        uint32_t count = 0;
        XR_TRY(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr));
        const XrExtensionProperties emptyProperties{XR_TYPE_EXTENSION_PROPERTIES};
        std::vector<XrExtensionProperties> available(count, emptyProperties);
        XR_TRY(xrEnumerateInstanceExtensionProperties(nullptr, count, &count, available.data()));
        for (const char* wanted : extensions) {
            if (std::none_of(available.begin(), available.end(),
                             [&](const XrExtensionProperties& p) { return !strcmp(p.extensionName, wanted); })) {
                LOGE("OpenXR runtime lacks %s", wanted);
                error_ = std::string("OpenXR runtime lacks ") + wanted;
                return false;
            }
        }

        XrInstanceCreateInfoAndroidKHR androidInfo{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
        androidInfo.applicationVM = vm_;
        androidInfo.applicationActivity = activity_;
        XrInstanceCreateInfo instanceInfo{XR_TYPE_INSTANCE_CREATE_INFO};
        instanceInfo.next = &androidInfo;
        strncpy(instanceInfo.applicationInfo.applicationName, "ProjectM Quest", XR_MAX_APPLICATION_NAME_SIZE - 1);
        strncpy(instanceInfo.applicationInfo.engineName, "projectM", XR_MAX_ENGINE_NAME_SIZE - 1);
        instanceInfo.applicationInfo.applicationVersion = 1;
        instanceInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
        instanceInfo.enabledExtensionCount = sizeof(extensions) / sizeof(extensions[0]);
        instanceInfo.enabledExtensionNames = extensions;
        XR_TRY(xrCreateInstance(&instanceInfo, &instance_));

        XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
        systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        XR_TRY(xrGetSystem(instance_, &systemInfo, &systemId_));

        // The runtime requires this query before it will create a GLES session.
        PFN_xrGetOpenGLESGraphicsRequirementsKHR getRequirements = nullptr;
        XR_TRY(xrGetInstanceProcAddr(instance_, "xrGetOpenGLESGraphicsRequirementsKHR",
                                     reinterpret_cast<PFN_xrVoidFunction*>(&getRequirements)));
        XrGraphicsRequirementsOpenGLESKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
        XR_TRY(getRequirements(instance_, systemId_, &requirements));

        XrGraphicsBindingOpenGLESAndroidKHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
        binding.display = display_;
        binding.config = config_;
        binding.context = context_;
        XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
        sessionInfo.next = &binding;
        sessionInfo.systemId = systemId_;
        XR_TRY(xrCreateSession(instance_, &sessionInfo, &session_));

        XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        spaceInfo.poseInReferenceSpace.orientation.w = 1.f;
        XR_TRY(xrCreateReferenceSpace(session_, &spaceInfo, &space_));

        // RGBA8 (not sRGB): projectM already writes display-encoded colours, which the compositor
        // must not encode a second time.
        uint32_t formatCount = 0;
        XR_TRY(xrEnumerateSwapchainFormats(session_, 0, &formatCount, nullptr));
        std::vector<int64_t> formats(formatCount);
        XR_TRY(xrEnumerateSwapchainFormats(session_, formatCount, &formatCount, formats.data()));
        if (formats.empty()) {
            error_ = "No swapchain formats";
            return false;
        }
        const bool rgba8 = std::find(formats.begin(), formats.end(), GL_RGBA8) != formats.end();
        XrSwapchainCreateInfo swapchainInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        swapchainInfo.format = rgba8 ? GL_RGBA8 : formats[0];
        swapchainInfo.sampleCount = 1;
        swapchainInfo.width = kTextureSize;
        swapchainInfo.height = kTextureSize;
        swapchainInfo.faceCount = 1;
        swapchainInfo.arraySize = 1;
        swapchainInfo.mipCount = 1;
        XR_TRY(xrCreateSwapchain(session_, &swapchainInfo, &swapchain_));
        uint32_t imageCount = 0;
        XR_TRY(xrEnumerateSwapchainImages(swapchain_, 0, &imageCount, nullptr));
        const XrSwapchainImageOpenGLESKHR emptyImage{XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR};
        images_.assign(imageCount, emptyImage);
        XR_TRY(xrEnumerateSwapchainImages(swapchain_, imageCount, &imageCount,
                                          reinterpret_cast<XrSwapchainImageBaseHeader*>(images_.data())));
        LOGI("OpenXR session ready: %ux%u swapchain, %u images", kTextureSize, kTextureSize, imageCount);
        return true;
    }

    // --- projectM: one instance on the session's GL context ---
    bool InitProjectM() {
        InstallTextures(assets_, textureDir_);
        std::string index = ReadAsset(assets_, "presets.idx");
        for (size_t start = 0; start < index.size();) {
            size_t end = index.find('\n', start);
            if (end == std::string::npos) end = index.size();
            const size_t tab = index.find('\t', start);
            const size_t nameEnd = tab < end ? tab : end;
            if (nameEnd > start) presets_.emplace_back(index, start, nameEnd - start);
            start = end + 1;
        }
        if (presets_.empty()) {
            error_ = "No bundled presets";
            return false;
        }
        projectm_opengl_forget_framebuffer_guard();  // fresh context: drop guard state of any earlier instance
        pm_ = projectm_create();
        if (!pm_) {
            error_ = "projectM could not start";
            return false;
        }
        projectm_set_window_size(pm_, kTextureSize, kTextureSize);
        projectm_set_fps(pm_, static_cast<int32_t>(kMaxFps));
        projectm_set_preset_locked(pm_, true);  // presets advance on our own timer
        projectm_set_soft_cut_duration(pm_, 3.0);
        projectm_opengl_set_line_reference_size(pm_, 1024, 768);  // same line weight as the TV engine
        const char* texturePath = textureDir_.c_str();
        projectm_set_texture_search_paths(pm_, &texturePath, 1);
        projectm_set_preset_switch_failed_event_callback(
            pm_,
            [](const char* file, const char* message, void* self) {
                LOGW("Preset failed (%s): %s", file ? file : "", message ? message : "");
                static_cast<QuestHost*>(self)->presetFailed_ = true;
            },
            this);
        glGenFramebuffers(1, &fbo_);
        rng_.seed(static_cast<unsigned>(Now() * 1000.0));
        NextPreset(false);
        return true;
    }

    void NextPreset(bool smooth) {
        for (int attempt = 0; attempt < 8; ++attempt) {
            const std::string& name = presets_[rng_() % presets_.size()];
            std::string data = ReadAsset(assets_, "presets/" + name);
            if (data.empty()) continue;
            presetFailed_ = false;
            projectm_load_preset_data(pm_, data.c_str(), smooth);
            if (!presetFailed_) {
                LOGI("Preset: %s", name.c_str());
                break;
            }
        }
        lastPreset_ = Now();
    }

    // --- frame loop ---
    void Loop() {
        while (!stop_ && !exitRequested_) {
            PollEvents();
            if (state_ == XR_SESSION_STATE_READY && !running_ && resumed_) {
                XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (XR_FAILED(xrBeginSession(session_, &begin))) {
                    error_ = "xrBeginSession failed";
                    return;
                }
                running_ = true;
            }
            if (!running_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            if (!Frame()) return;
        }
    }

    void PollEvents() {
        for (;;) {
            XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
            if (xrPollEvent(instance_, &event) != XR_SUCCESS) return;
            if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
                exitRequested_ = true;
            } else if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                state_ = reinterpret_cast<XrEventDataSessionStateChanged*>(&event)->state;
                LOGI("Session state %d", static_cast<int>(state_));
                if (state_ == XR_SESSION_STATE_STOPPING) {
                    xrEndSession(session_);
                    running_ = false;
                } else if (state_ == XR_SESSION_STATE_EXITING || state_ == XR_SESSION_STATE_LOSS_PENDING) {
                    exitRequested_ = true;
                }
            }
        }
    }

    bool Frame() {
        XrFrameState frameState{XR_TYPE_FRAME_STATE};
        XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
        XR_TRY(xrWaitFrame(session_, &waitInfo, &frameState));
        XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
        XR_TRY(xrBeginFrame(session_, &beginInfo));

        XrCompositionLayerEquirect2KHR layer{XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR};
        const XrCompositionLayerBaseHeader* layers[1] = {reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer)};
        uint32_t layerCount = 0;
        if (frameState.shouldRender && RenderToSwapchain()) {
            layer.space = space_;
            layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            layer.subImage.swapchain = swapchain_;
            layer.subImage.imageRect.extent = {kTextureSize, kTextureSize};
            layer.pose.orientation.w = 1.f;  // dome centred straight ahead (-Z) of the session's start pose
            layer.radius = kDomeRadius;
            layer.centralHorizontalAngle = 2.f * kHalfSpan;
            layer.upperVerticalAngle = kHalfSpan;
            layer.lowerVerticalAngle = -kHalfSpan;
            layerCount = 1;
        }
        XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
        endInfo.displayTime = frameState.predictedDisplayTime;
        endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        endInfo.layerCount = layerCount;
        endInfo.layers = layers;
        XR_TRY(xrEndFrame(session_, &endInfo));
        return true;
    }

    // Draws one projectM frame into the next swapchain image. Above kMaxFps the previously released
    // image is submitted again instead (valid: the compositor keeps using the last released image).
    bool RenderToSwapchain() {
        const double now = Now();
        if (haveImage_ && now - lastRender_ < 1.0 / kMaxFps - 0.001) return true;

        uint32_t index = 0;
        XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        waitInfo.timeout = XR_INFINITE_DURATION;
        if (XR_FAILED(xrAcquireSwapchainImage(swapchain_, &acquireInfo, &index)) ||
            XR_FAILED(xrWaitSwapchainImage(swapchain_, &waitInfo))) {
            return haveImage_;
        }
        DrainPcm();
        if (now - lastPreset_ > kPresetSeconds) NextPreset(true);

        glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, images_[index].image, 0);
        glViewport(0, 0, kTextureSize, kTextureSize);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
            projectm_opengl_render_frame_fbo(pm_, fbo_);
        } else {
            LOGW("Swapchain framebuffer incomplete");
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glFlush();  // the compositor reads the texture from another context

        XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        if (XR_FAILED(xrReleaseSwapchainImage(swapchain_, &releaseInfo))) return haveImage_;
        haveImage_ = true;
        lastRender_ = now;
        return true;
    }

    void DrainPcm() {
        std::vector<float> pcm;
        {
            std::lock_guard<std::mutex> lock(pcmMutex_);
            pcm.swap(pcm_);
        }
        for (size_t offset = 0; offset < pcm.size(); offset += 1024) {
            const size_t frames = std::min<size_t>(1024, pcm.size() - offset);
            projectm_pcm_add_float(pm_, pcm.data() + offset, static_cast<unsigned int>(frames), PROJECTM_MONO);
        }
    }

    void Shutdown() {
        if (pm_) {
            projectm_destroy(pm_);
            pm_ = nullptr;
        }
        if (fbo_) glDeleteFramebuffers(1, &fbo_);
        fbo_ = 0;
        if (swapchain_ != XR_NULL_HANDLE) xrDestroySwapchain(swapchain_);
        if (space_ != XR_NULL_HANDLE) xrDestroySpace(space_);
        if (session_ != XR_NULL_HANDLE) xrDestroySession(session_);
        if (instance_ != XR_NULL_HANDLE) xrDestroyInstance(instance_);
        if (display_ != EGL_NO_DISPLAY) {
            eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (surface_ != EGL_NO_SURFACE) eglDestroySurface(display_, surface_);
            if (context_ != EGL_NO_CONTEXT) eglDestroyContext(display_, context_);
            eglTerminate(display_);
        }
    }

    // Tells the activity to finish: the session ended (user chose Exit) or could not start.
    void NotifyExit(JNIEnv* env) {
        jclass cls = env->GetObjectClass(activity_);
        jmethodID method = env->GetMethodID(cls, "onNativeExit", "(Ljava/lang/String;)V");
        jstring reason = error_.empty() ? nullptr : env->NewStringUTF(error_.c_str());
        if (method) env->CallVoidMethod(activity_, method, reason);
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (reason) env->DeleteLocalRef(reason);
        env->DeleteLocalRef(cls);
    }

    JavaVM* vm_;
    jobject activity_;
    jobject assetManager_;
    AAssetManager* assets_;
    std::string textureDir_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> resumed_{false};
    std::atomic<bool> presetFailed_{false};
    bool exitRequested_ = false;
    bool running_ = false;
    bool haveImage_ = false;
    double lastRender_ = 0;
    double lastPreset_ = 0;
    std::string error_;

    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLConfig config_ = nullptr;
    EGLContext context_ = EGL_NO_CONTEXT;
    EGLSurface surface_ = EGL_NO_SURFACE;

    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId systemId_ = XR_NULL_SYSTEM_ID;
    XrSession session_ = XR_NULL_HANDLE;
    XrSpace space_ = XR_NULL_HANDLE;
    XrSwapchain swapchain_ = XR_NULL_HANDLE;
    XrSessionState state_ = XR_SESSION_STATE_UNKNOWN;
    std::vector<XrSwapchainImageOpenGLESKHR> images_;

    projectm_handle pm_ = nullptr;
    GLuint fbo_ = 0;
    std::vector<std::string> presets_;
    std::mt19937 rng_;

    std::mutex pcmMutex_;
    std::vector<float> pcm_;
};

QuestHost* g_host = nullptr;  // touched from the Android main thread only, except PushPcm
std::mutex g_hostMutex;

}  // namespace

extern "C" {

JNIEXPORT void JNICALL QUEST_FN(nativeStart)(JNIEnv* env, jclass, jobject activity, jobject assetManager,
                                             jstring textureDir) {
    std::lock_guard<std::mutex> lock(g_hostMutex);
    if (g_host) return;
    JavaVM* vm = nullptr;
    env->GetJavaVM(&vm);
    const char* path = env->GetStringUTFChars(textureDir, nullptr);
    // Global references: the render thread uses both until nativeStop returns.
    jobject activityRef = env->NewGlobalRef(activity);
    jobject assetManagerRef = env->NewGlobalRef(assetManager);
    g_host = new QuestHost(vm, activityRef, assetManagerRef, AAssetManager_fromJava(env, assetManagerRef), path);
    env->ReleaseStringUTFChars(textureDir, path);
    g_host->Start();
}

JNIEXPORT void JNICALL QUEST_FN(nativeSetResumed)(JNIEnv*, jclass, jboolean resumed) {
    std::lock_guard<std::mutex> lock(g_hostMutex);
    if (g_host) g_host->SetResumed(resumed == JNI_TRUE);
}

JNIEXPORT void JNICALL QUEST_FN(nativePushPcm)(JNIEnv* env, jclass, jfloatArray samples, jint count) {
    std::lock_guard<std::mutex> lock(g_hostMutex);
    if (!g_host || count <= 0) return;
    jfloat* data = env->GetFloatArrayElements(samples, nullptr);
    g_host->PushPcm(data, static_cast<size_t>(count));
    env->ReleaseFloatArrayElements(samples, data, JNI_ABORT);
}

JNIEXPORT void JNICALL QUEST_FN(nativeStop)(JNIEnv* env, jclass) {
    QuestHost* host = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_hostMutex);
        host = g_host;
        g_host = nullptr;
    }
    if (host) {
        host->Stop(env);
        delete host;
    }
}

}  // extern "C"
