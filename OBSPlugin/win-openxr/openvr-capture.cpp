#define NOMINMAX

#include <obs-module.h>
#include <d3d11.h>
#include <dxgi.h>
#include <openvr.h>
#include <winrt/base.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <mutex>
#include <new>
#include <string>

#include "dxgi_format_info.h"
#include "openvr-capture.h"

#pragma comment(lib, "d3d11.lib")

namespace {

#define openvr_blog(level, message, ...) blog(level, "[win_openxr_mirror/openvr] " message, ##__VA_ARGS__)

    using VrInitInternal2 = uint32_t(VR_CALLTYPE*)(vr::EVRInitError*, vr::EVRApplicationType, const char*);
    using VrShutdownInternal = void(VR_CALLTYPE*)();
    using VrGetGenericInterface = void*(VR_CALLTYPE*)(const char*, vr::EVRInitError*);
    using VrGetErrorDescription = const char*(VR_CALLTYPE*)(vr::EVRInitError);

    extern "C" IMAGE_DOS_HEADER __ImageBase;

    std::wstring sibling_openvr_dll_path() {
        std::array<wchar_t, 32768> module_path{};
        const DWORD length = GetModuleFileNameW(
            reinterpret_cast<HMODULE>(&__ImageBase), module_path.data(), static_cast<DWORD>(module_path.size()));
        if (length == 0 || length >= module_path.size())
            return {};

        std::wstring path(module_path.data(), length);
        const size_t slash = path.find_last_of(L"\\/");
        if (slash == std::wstring::npos)
            return {};
        path.resize(slash + 1);
        path += L"openvr_api.dll";
        return path;
    }

    class OpenVrRuntime {
      public:
        bool acquire(vr::IVRSystem*& system, vr::IVRCompositor*& compositor, std::string& failure) {
            std::scoped_lock lock(mutex_);
            if (users_ > 0 && system_ && compositor_) {
                ++users_;
                system = system_;
                compositor = compositor_;
                return true;
            }

            const std::wstring dll_path = sibling_openvr_dll_path();
            if (dll_path.empty()) {
                failure = "could not resolve the OBS plugin directory";
                return false;
            }

            module_ = LoadLibraryExW(
                dll_path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            if (!module_) {
                failure = "openvr_api.dll is missing beside win-openxr.dll (Windows error " +
                          std::to_string(GetLastError()) + ")";
                return false;
            }

            init_ = reinterpret_cast<VrInitInternal2>(GetProcAddress(module_, "VR_InitInternal2"));
            shutdown_ = reinterpret_cast<VrShutdownInternal>(GetProcAddress(module_, "VR_ShutdownInternal"));
            get_interface_ = reinterpret_cast<VrGetGenericInterface>(GetProcAddress(module_, "VR_GetGenericInterface"));
            get_error_description_ = reinterpret_cast<VrGetErrorDescription>(
                GetProcAddress(module_, "VR_GetVRInitErrorAsEnglishDescription"));
            if (!init_ || !shutdown_ || !get_interface_) {
                failure = "openvr_api.dll does not expose the required OpenVR SDK entry points";
                unload_module();
                return false;
            }

            vr::EVRInitError error = vr::VRInitError_None;
            init_(&error, vr::VRApplication_Background, nullptr);
            if (error != vr::VRInitError_None) {
                failure = describe_error(error);
                shutdown_();
                unload_module();
                return false;
            }

            system_ = static_cast<vr::IVRSystem*>(get_interface_(vr::IVRSystem_Version, &error));
            if (!system_ || error != vr::VRInitError_None) {
                failure = "OpenVR system interface unavailable: " + describe_error(error);
                shutdown_();
                unload_module();
                return false;
            }

            error = system_->SetSDKVersion(
                vr::k_nSteamVRVersionMajor, vr::k_nSteamVRVersionMinor, vr::k_nSteamVRVersionBuild);
            if (error != vr::VRInitError_None) {
                failure = "SteamVR rejected OpenVR SDK 2.15.6: " + describe_error(error);
                shutdown_();
                unload_module();
                return false;
            }

            compositor_ = static_cast<vr::IVRCompositor*>(get_interface_(vr::IVRCompositor_Version, &error));
            if (!compositor_ || error != vr::VRInitError_None) {
                failure = "OpenVR compositor interface unavailable: " + describe_error(error);
                shutdown_();
                unload_module();
                return false;
            }

            users_ = 1;
            system = system_;
            compositor = compositor_;
            const char* runtime_version = system_->GetRuntimeVersion();
            openvr_blog(LOG_INFO,
                        "connected to SteamVR through OpenVR SDK 2.15.6 (runtime %s)",
                        runtime_version ? runtime_version : "unknown");
            return true;
        }

        void release() {
            std::scoped_lock lock(mutex_);
            if (users_ == 0)
                return;
            if (--users_ != 0)
                return;
            shutdown_runtime();
        }

        void force_shutdown() {
            std::scoped_lock lock(mutex_);
            users_ = 0;
            shutdown_runtime();
        }

      private:
        std::string describe_error(vr::EVRInitError error) const {
            if (get_error_description_) {
                const char* description = get_error_description_(error);
                if (description && *description)
                    return description;
            }
            return "OpenVR error " + std::to_string(static_cast<int>(error));
        }

        void shutdown_runtime() {
            compositor_ = nullptr;
            system_ = nullptr;
            if (shutdown_)
                shutdown_();
            unload_module();
        }

        void unload_module() {
            init_ = nullptr;
            shutdown_ = nullptr;
            get_interface_ = nullptr;
            get_error_description_ = nullptr;
            if (module_) {
                FreeLibrary(module_);
                module_ = nullptr;
            }
        }

        std::mutex mutex_;
        HMODULE module_ = nullptr;
        VrInitInternal2 init_ = nullptr;
        VrShutdownInternal shutdown_ = nullptr;
        VrGetGenericInterface get_interface_ = nullptr;
        VrGetErrorDescription get_error_description_ = nullptr;
        vr::IVRSystem* system_ = nullptr;
        vr::IVRCompositor* compositor_ = nullptr;
        uint32_t users_ = 0;
    };

    OpenVrRuntime g_runtime;

    struct Crop {
        double top = 0.0;
        double left = 0.0;
        double bottom = 0.0;
        double right = 0.0;
    };

    struct OpenVrCapture {
        obs_source_t* source = nullptr;
        winrt::com_ptr<ID3D11Device> device;
        winrt::com_ptr<ID3D11DeviceContext> device_context;
        vr::IVRSystem* system = nullptr;
        vr::IVRCompositor* compositor = nullptr;
        std::array<ID3D11ShaderResourceView*, 2> mirror_views{};
        std::array<winrt::com_ptr<ID3D11Texture2D>, 2> mirror_textures;
        winrt::com_ptr<ID3D11Texture2D> composite_texture;
        winrt::com_ptr<ID3D11Texture2D> output_texture;
        gs_texture_t* obs_texture = nullptr;
        Crop crop;
        int eye = 1;
        bool fill_canvas = true;
        bool initialized = false;
        bool runtime_acquired = false;
        bool active = false;
        uint32_t source_width = 0;
        uint32_t source_height = 0;
        uint32_t output_width = 100;
        uint32_t output_height = 100;
        uint32_t crop_x = 0;
        uint32_t crop_y = 0;
        uint32_t canvas_width = 0;
        uint32_t canvas_height = 0;
        ULONGLONG last_init_attempt = 0;
        ULONGLONG last_error_log = 0;
        uint64_t rendered_frames = 0;
    };

    void release_mirror_views(OpenVrCapture* context) {
        for (size_t i = 0; i < context->mirror_views.size(); ++i) {
            context->mirror_textures[i] = nullptr;
            if (context->mirror_views[i] && context->compositor)
                context->compositor->ReleaseMirrorTextureD3D11(context->mirror_views[i]);
            context->mirror_views[i] = nullptr;
        }
    }

    void deinitialize(OpenVrCapture* context) {
        context->initialized = false;
        if (context->obs_texture) {
            obs_enter_graphics();
            gs_texture_destroy(context->obs_texture);
            obs_leave_graphics();
            context->obs_texture = nullptr;
        }
        context->output_texture = nullptr;
        context->composite_texture = nullptr;
        release_mirror_views(context);
        context->device_context = nullptr;
        context->device = nullptr;
        context->system = nullptr;
        context->compositor = nullptr;
        if (context->runtime_acquired) {
            context->runtime_acquired = false;
            g_runtime.release();
        }
    }

    bool get_mirror_view(OpenVrCapture* context, size_t slot, vr::EVREye eye, std::string& failure) {
        void* view = nullptr;
        const vr::EVRCompositorError error =
            context->compositor->GetMirrorTextureD3D11(eye, context->device.get(), &view);
        if (error != vr::VRCompositorError_None || !view) {
            failure = "GetMirrorTextureD3D11 failed for the " + std::string(eye == vr::Eye_Left ? "left" : "right") +
                      " eye (OpenVR compositor error " + std::to_string(static_cast<int>(error)) + ")";
            return false;
        }

        context->mirror_views[slot] = static_cast<ID3D11ShaderResourceView*>(view);
        winrt::com_ptr<ID3D11Resource> resource;
        context->mirror_views[slot]->GetResource(resource.put());
        if (!resource ||
            FAILED(resource->QueryInterface(__uuidof(ID3D11Texture2D), context->mirror_textures[slot].put_void()))) {
            failure = "SteamVR's mirror resource is not a D3D11 Texture2D";
            return false;
        }
        return true;
    }

    bool create_output_texture(OpenVrCapture* context, DXGI_FORMAT source_format, std::string& failure) {
        obs_mirror_ipc::DxgiFormatInfo format_info{};
        if (!obs_mirror_ipc::GetFormatInfo(source_format, format_info)) {
            failure = "unsupported SteamVR mirror DXGI format " + std::to_string(static_cast<int>(source_format));
            return false;
        }

        D3D11_TEXTURE2D_DESC output_desc{};
        output_desc.Width = context->output_width;
        output_desc.Height = context->output_height;
        output_desc.MipLevels = 1;
        output_desc.ArraySize = 1;
        output_desc.Format = format_info.linear;
        output_desc.SampleDesc.Count = 1;
        output_desc.Usage = D3D11_USAGE_DEFAULT;
        output_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        output_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

        HRESULT hr = context->device->CreateTexture2D(&output_desc, nullptr, context->output_texture.put());
        if (FAILED(hr)) {
            failure = "could not create the OBS OpenVR output texture (HRESULT " +
                      std::to_string(static_cast<unsigned long>(hr)) + ")";
            return false;
        }

        winrt::com_ptr<IDXGIResource> shared_resource = context->output_texture.try_as<IDXGIResource>();
        if (!shared_resource) {
            failure = "the OpenVR output texture cannot be shared with OBS";
            return false;
        }

        HANDLE handle = nullptr;
        hr = shared_resource->GetSharedHandle(&handle);
        const uintptr_t handle_value = reinterpret_cast<uintptr_t>(handle);
        if (FAILED(hr) || handle_value == 0 || handle_value > std::numeric_limits<uint32_t>::max()) {
            failure = "could not obtain a compatible shared texture handle for OBS";
            return false;
        }

        obs_enter_graphics();
        context->obs_texture = gs_texture_open_shared(static_cast<uint32_t>(handle_value));
        obs_leave_graphics();
        if (!context->obs_texture) {
            failure = "OBS could not open the SteamVR mirror texture; verify OBS and SteamVR use the same GPU";
            return false;
        }
        return true;
    }

    void apply_crop(OpenVrCapture* context) {
        const uint32_t left = std::clamp(
            static_cast<uint32_t>(context->crop.left / 100.0 * context->source_width), 0u, context->source_width - 1);
        const uint32_t top = std::clamp(
            static_cast<uint32_t>(context->crop.top / 100.0 * context->source_height), 0u, context->source_height - 1);
        const uint32_t remaining_width = context->source_width - left;
        const uint32_t remaining_height = context->source_height - top;
        const uint32_t right =
            std::clamp(static_cast<uint32_t>(context->crop.right / 100.0 * remaining_width), 0u, remaining_width - 1);
        const uint32_t bottom = std::clamp(
            static_cast<uint32_t>(context->crop.bottom / 100.0 * remaining_height), 0u, remaining_height - 1);

        context->crop_x = left;
        context->crop_y = top;
        context->output_width = remaining_width - right;
        context->output_height = remaining_height - bottom;

        obs_video_info video_info{};
        if (!context->fill_canvas || !obs_get_video_info(&video_info) || video_info.base_width == 0 ||
            video_info.base_height == 0)
            return;

        context->canvas_width = video_info.base_width;
        context->canvas_height = video_info.base_height;
        const double target = static_cast<double>(video_info.base_width) / video_info.base_height;
        const double current = static_cast<double>(context->output_width) / context->output_height;
        if (current > target) {
            const uint32_t trimmed = std::max(static_cast<uint32_t>(context->output_height * target), 1u);
            if (trimmed < context->output_width) {
                context->crop_x += (context->output_width - trimmed) / 2;
                context->output_width = trimmed;
            }
        } else if (current < target) {
            const uint32_t trimmed = std::max(static_cast<uint32_t>(context->output_width / target), 1u);
            if (trimmed < context->output_height) {
                context->crop_y += (context->output_height - trimmed) / 2;
                context->output_height = trimmed;
            }
        }
    }

    bool initialize(OpenVrCapture* context, bool forced = false) {
        if (context->initialized)
            return true;
        const ULONGLONG now = GetTickCount64();
        if (!forced && now - context->last_init_attempt < 2000)
            return false;

        deinitialize(context);
        context->last_init_attempt = now;

        std::string failure;
        D3D11_TEXTURE2D_DESC first_desc{};
        D3D11_TEXTURE2D_DESC second_desc{};
        D3D11_TEXTURE2D_DESC composite_desc{};
        obs_mirror_ipc::DxgiFormatInfo format_info{};
        if (!g_runtime.acquire(context->system, context->compositor, failure)) {
            if (context->last_error_log == 0 || now - context->last_error_log >= 30000) {
                openvr_blog(
                    LOG_WARNING, "[%s] waiting for SteamVR: %s", obs_source_get_name(context->source), failure.c_str());
                context->last_error_log = now;
            }
            return false;
        }
        context->runtime_acquired = true;

        obs_enter_graphics();
        if (gs_get_device_type() == GS_DEVICE_DIRECT3D_11)
            context->device.copy_from(static_cast<ID3D11Device*>(gs_get_device_obj()));
        obs_leave_graphics();
        if (!context->device) {
            failure = "OBS is not using its required D3D11 graphics backend";
            goto fail;
        }
        context->device->GetImmediateContext(context->device_context.put());
        if (!context->device_context) {
            failure = "OBS did not provide a D3D11 immediate context";
            goto fail;
        }

        if (context->eye == 2) {
            if (!get_mirror_view(context, 0, vr::Eye_Left, failure) ||
                !get_mirror_view(context, 1, vr::Eye_Right, failure))
                goto fail;
        } else if (!get_mirror_view(context, 0, context->eye == 0 ? vr::Eye_Left : vr::Eye_Right, failure)) {
            goto fail;
        }

        context->mirror_textures[0]->GetDesc(&first_desc);
        if (first_desc.Width == 0 || first_desc.Height == 0) {
            failure = "SteamVR returned an empty mirror texture";
            goto fail;
        }
        context->source_width = first_desc.Width;
        context->source_height = first_desc.Height;

        if (context->eye == 2) {
            context->mirror_textures[1]->GetDesc(&second_desc);
            if (second_desc.Width == 0 || second_desc.Height == 0 || second_desc.Format != first_desc.Format) {
                failure = "SteamVR returned incompatible left and right mirror textures";
                goto fail;
            }
            context->source_width = first_desc.Width + second_desc.Width;
            context->source_height = std::min(first_desc.Height, second_desc.Height);

            if (!obs_mirror_ipc::GetFormatInfo(first_desc.Format, format_info)) {
                failure =
                    "unsupported SteamVR mirror DXGI format " + std::to_string(static_cast<int>(first_desc.Format));
                goto fail;
            }
            composite_desc.Width = context->source_width;
            composite_desc.Height = context->source_height;
            composite_desc.MipLevels = 1;
            composite_desc.ArraySize = 1;
            composite_desc.Format = format_info.linear;
            composite_desc.SampleDesc.Count = 1;
            composite_desc.Usage = D3D11_USAGE_DEFAULT;
            const HRESULT hr =
                context->device->CreateTexture2D(&composite_desc, nullptr, context->composite_texture.put());
            if (FAILED(hr)) {
                failure = "could not create the stereo OpenVR compositor texture";
                goto fail;
            }
        }

        apply_crop(context);
        if (!create_output_texture(context, first_desc.Format, failure))
            goto fail;

        context->initialized = true;
        context->last_error_log = 0;
        context->rendered_frames = 0;
        openvr_blog(LOG_INFO,
                    "[%s] initialized SteamVR mirror: %ux%u source, %ux%u output, eye=%d, fill canvas=%s",
                    obs_source_get_name(context->source),
                    context->source_width,
                    context->source_height,
                    context->output_width,
                    context->output_height,
                    context->eye,
                    context->fill_canvas ? "yes" : "no");
        return true;

    fail:
        openvr_blog(LOG_WARNING,
                    "[%s] OpenVR capture initialization failed: %s",
                    obs_source_get_name(context->source),
                    failure.c_str());
        context->last_error_log = now;
        deinitialize(context);
        return false;
    }

    const char* get_name(void*) {
        return obs_module_text("OpenVRMirrorCapture");
    }

    void update(void* data, obs_data_t* settings) {
        auto* context = static_cast<OpenVrCapture*>(data);
        const int eye = std::clamp(static_cast<int>(obs_data_get_int(settings, "openvr_eye")), 0, 2);
        Crop crop{};
        crop.top = std::clamp(obs_data_get_double(settings, "openvr_crop_top"), 0.0, 100.0);
        crop.bottom = std::clamp(obs_data_get_double(settings, "openvr_crop_bottom"), 0.0, 100.0);
        crop.left = std::clamp(obs_data_get_double(settings, "openvr_crop_left"), 0.0, 100.0);
        crop.right = std::clamp(obs_data_get_double(settings, "openvr_crop_right"), 0.0, 100.0);
        const bool fill_canvas = obs_data_get_bool(settings, "openvr_fill_canvas");
        const bool changed = eye != context->eye || fill_canvas != context->fill_canvas ||
                             crop.top != context->crop.top || crop.bottom != context->crop.bottom ||
                             crop.left != context->crop.left || crop.right != context->crop.right;
        context->eye = eye;
        context->crop = crop;
        context->fill_canvas = fill_canvas;
        if (changed && context->initialized) {
            deinitialize(context);
            if (context->active)
                initialize(context, true);
        }
    }

    void defaults(obs_data_t* settings) {
        obs_data_set_default_int(settings, "openvr_eye", 1);
        obs_data_set_default_bool(settings, "openvr_fill_canvas", true);
        obs_data_set_default_double(settings, "openvr_crop_top", 0.0);
        obs_data_set_default_double(settings, "openvr_crop_bottom", 0.0);
        obs_data_set_default_double(settings, "openvr_crop_left", 0.0);
        obs_data_set_default_double(settings, "openvr_crop_right", 0.0);
    }

    void show(void* data) {
        auto* context = static_cast<OpenVrCapture*>(data);
        context->active = true;
        initialize(context, true);
    }

    void hide(void* data) {
        auto* context = static_cast<OpenVrCapture*>(data);
        context->active = false;
        deinitialize(context);
    }

    void* create(obs_data_t* settings, obs_source_t* source) {
        auto* context = new (std::nothrow) OpenVrCapture{};
        if (!context)
            return nullptr;
        context->source = source;
        update(context, settings);
        return context;
    }

    void destroy(void* data) {
        auto* context = static_cast<OpenVrCapture*>(data);
        deinitialize(context);
        delete context;
    }

    uint32_t get_width(void* data) {
        return static_cast<OpenVrCapture*>(data)->output_width;
    }

    uint32_t get_height(void* data) {
        return static_cast<OpenVrCapture*>(data)->output_height;
    }

    void render(void* data, gs_effect_t*) {
        auto* context = static_cast<OpenVrCapture*>(data);
        if (!context->active && obs_source_active(context->source))
            context->active = true;
        if (!context->initialized) {
            if (context->active)
                initialize(context);
            if (!context->initialized)
                return;
        }

        ID3D11Texture2D* copy_source = context->mirror_textures[0].get();
        if (context->eye == 2) {
            D3D11_TEXTURE2D_DESC left_desc{};
            D3D11_TEXTURE2D_DESC right_desc{};
            context->mirror_textures[0]->GetDesc(&left_desc);
            context->mirror_textures[1]->GetDesc(&right_desc);
            D3D11_BOX left_box{0, 0, 0, left_desc.Width, std::min(left_desc.Height, context->source_height), 1};
            D3D11_BOX right_box{0, 0, 0, right_desc.Width, std::min(right_desc.Height, context->source_height), 1};
            context->device_context->CopySubresourceRegion(
                context->composite_texture.get(), 0, 0, 0, 0, context->mirror_textures[0].get(), 0, &left_box);
            context->device_context->CopySubresourceRegion(context->composite_texture.get(),
                                                           0,
                                                           left_desc.Width,
                                                           0,
                                                           0,
                                                           context->mirror_textures[1].get(),
                                                           0,
                                                           &right_box);
            copy_source = context->composite_texture.get();
        }

        D3D11_BOX crop_box{context->crop_x,
                           context->crop_y,
                           0,
                           context->crop_x + context->output_width,
                           context->crop_y + context->output_height,
                           1};
        context->device_context->CopySubresourceRegion(
            context->output_texture.get(), 0, 0, 0, 0, copy_source, 0, &crop_box);
        context->device_context->Flush();

        gs_effect_t* effect = obs_get_base_effect(OBS_EFFECT_OPAQUE);
        while (gs_effect_loop(effect, "Draw"))
            obs_source_draw(context->obs_texture, 0, 0, 0, 0, false);
        ++context->rendered_frames;
    }

    void tick(void* data, float) {
        auto* context = static_cast<OpenVrCapture*>(data);
        const bool active = obs_source_active(context->source);
        if (!active && context->active) {
            context->active = false;
            deinitialize(context);
        } else if (active) {
            context->active = true;
            if (!context->initialized)
                initialize(context);
            if (context->initialized && context->fill_canvas) {
                obs_video_info video_info{};
                if (obs_get_video_info(&video_info) && (video_info.base_width != context->canvas_width ||
                                                        video_info.base_height != context->canvas_height)) {
                    deinitialize(context);
                    initialize(context, true);
                }
            }
        }
    }

    bool reinitialize_button(obs_properties_t*, obs_property_t*, void* data) {
        auto* context = static_cast<OpenVrCapture*>(data);
        context->last_init_attempt = 0;
        deinitialize(context);
        if (context->active || obs_source_active(context->source)) {
            context->active = true;
            initialize(context, true);
        }
        return false;
    }

    obs_properties_t* properties(void* data) {
        obs_properties_t* props = obs_properties_create();
        obs_property_t* eye = obs_properties_add_list(
            props, "openvr_eye", obs_module_text("OpenVREyeCapture"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
        obs_property_list_add_int(eye, obs_module_text("EyeLeft"), 0);
        obs_property_list_add_int(eye, obs_module_text("EyeRight"), 1);
        obs_property_list_add_int(eye, obs_module_text("EyeBoth"), 2);
        obs_properties_add_bool(props, "openvr_fill_canvas", obs_module_text("FillCanvas"));
        obs_properties_add_float_slider(
            props, "openvr_crop_top", obs_module_text("CropTopPercentage"), 0.0, 100.0, 0.1);
        obs_properties_add_float_slider(
            props, "openvr_crop_bottom", obs_module_text("CropBottomPercentage"), 0.0, 100.0, 0.1);
        obs_properties_add_float_slider(
            props, "openvr_crop_left", obs_module_text("CropLeftPercentage"), 0.0, 100.0, 0.1);
        obs_properties_add_float_slider(
            props, "openvr_crop_right", obs_module_text("CropRightPercentage"), 0.0, 100.0, 0.1);
        obs_properties_add_text(props, "openvr_note", obs_module_text("OpenVRCaptureNote"), OBS_TEXT_INFO);
        obs_properties_add_button(
            props, "openvr_reinitialize", obs_module_text("ReinitializeOpenVRSource"), reinitialize_button);
        UNUSED_PARAMETER(data);
        return props;
    }

} // namespace

void register_openvr_capture_source() {
    obs_source_info info{};
    info.id = "openvrmirror_capture";
    info.type = OBS_SOURCE_TYPE_INPUT;
    info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
    info.get_name = get_name;
    info.create = create;
    info.destroy = destroy;
    info.update = update;
    info.get_defaults = defaults;
    info.show = show;
    info.hide = hide;
    info.get_width = get_width;
    info.get_height = get_height;
    info.video_render = render;
    info.video_tick = tick;
    info.get_properties = properties;
    obs_register_source(&info);
    openvr_blog(LOG_INFO, "registered native SteamVR/OpenVR capture source (SDK 2.15.6)");
}

void shutdown_openvr_capture_runtime() {
    g_runtime.force_shutdown();
}
