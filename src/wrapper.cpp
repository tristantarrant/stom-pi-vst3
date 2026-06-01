/*
 * Stom-Pi VST3 Wrapper — LV2 plugin that hosts VST3 via Carla backend
 * Copyright (C) 2026 Dataforte <info@dataforte.net>
 * Licensed under GPLv2+
 *
 * Uses lv2_lib_descriptor() to read per-bundle wrapper.json config,
 * allowing a single .so to serve different VST3 plugins.
 */

#include "CarlaNativePlugin.h"
#include "CarlaHost.h"

#include "lv2/lv2.h"
#include "lv2/state.h"
#include "lv2/urid.h"
#include "lv2/atom.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

using namespace CARLA_BACKEND_NAMESPACE;

#define MAX_PARAMS 256

struct ParamMapping {
    uint32_t lv2_index;
    uint32_t vst3_param_id;
    float min;
    float max;
    float def;
};

static const char* VST3_SEARCH_PATHS[] = {
    "/var/lib/stom-pi/vst3",
    "/usr/lib/vst3",
    "/usr/local/lib/vst3",
    nullptr
};

struct WrapperConfig {
    std::string uri;
    std::string vst3_path;
    std::string name;
    uint32_t audio_inputs;
    uint32_t audio_outputs;
    std::vector<ParamMapping> parameters;
};

struct WrapperInstance {
    const WrapperConfig* config;

    const NativePluginDescriptor* carla_desc;
    NativePluginHandle carla_handle;
    CarlaHostHandle carla_host;
    NativeHostDescriptor host_desc;

    float* port_buffers[MAX_PARAMS + 16]; // control + audio ports
    float prev_values[MAX_PARAMS];
    uint32_t total_ports;

    double sample_rate;
    uint32_t buffer_size;

    // Dummy buffers for mono plugins (Carla rack always expects stereo)
    float* dummy_in;
    float* dummy_out;
    uint32_t dummy_size;
};

// --- NativeHostDescriptor callbacks ---

static uint32_t host_get_buffer_size(NativeHostHandle handle) {
    return static_cast<WrapperInstance*>(handle)->buffer_size;
}

static double host_get_sample_rate(NativeHostHandle handle) {
    return static_cast<WrapperInstance*>(handle)->sample_rate;
}

static bool host_is_offline(NativeHostHandle) {
    return false;
}

static const NativeTimeInfo* host_get_time_info(NativeHostHandle) {
    static NativeTimeInfo info = {};
    return &info;
}

static bool host_write_midi_event(NativeHostHandle, const NativeMidiEvent*) {
    return false;
}

static void host_ui_parameter_changed(NativeHostHandle, uint32_t, float) {}
static void host_ui_midi_program_changed(NativeHostHandle, uint8_t, uint32_t, uint32_t) {}
static void host_ui_custom_data_changed(NativeHostHandle, const char*, const char*) {}
static void host_ui_closed(NativeHostHandle) {}

static intptr_t host_dispatcher(NativeHostHandle, NativeHostDispatcherOpcode opcode,
                                int32_t, intptr_t, void*, float) {
    switch (opcode) {
        case NATIVE_HOST_OPCODE_NULL:
        case NATIVE_HOST_OPCODE_UPDATE_PARAMETER:
        case NATIVE_HOST_OPCODE_UPDATE_MIDI_PROGRAM:
        case NATIVE_HOST_OPCODE_RELOAD_PARAMETERS:
        case NATIVE_HOST_OPCODE_RELOAD_MIDI_PROGRAMS:
        case NATIVE_HOST_OPCODE_RELOAD_ALL:
        case NATIVE_HOST_OPCODE_UI_UNAVAILABLE:
        case NATIVE_HOST_OPCODE_HOST_IDLE:
        case NATIVE_HOST_OPCODE_INTERNAL_PLUGIN:
        case NATIVE_HOST_OPCODE_QUEUE_INLINE_DISPLAY:
        case NATIVE_HOST_OPCODE_UI_TOUCH_PARAMETER:
        case NATIVE_HOST_OPCODE_REQUEST_IDLE:
        case NATIVE_HOST_OPCODE_GET_FILE_PATH:
        case NATIVE_HOST_OPCODE_UI_RESIZE:
        case NATIVE_HOST_OPCODE_PREVIEW_BUFFER_DATA:
            break;
    }
    return 0;
}

static std::string resolve_vst3_path(const std::string& bundle_name) {
    // If already absolute, use as-is
    if (!bundle_name.empty() && bundle_name[0] == '/')
        return bundle_name;

    // Also check $HOME/.vst3
    const char* home = getenv("HOME");

    for (const char** p = VST3_SEARCH_PATHS; *p; p++) {
        std::string candidate = std::string(*p) + "/" + bundle_name;
        if (access(candidate.c_str(), F_OK) == 0)
            return candidate;
    }

    if (home) {
        std::string candidate = std::string(home) + "/.vst3/" + bundle_name;
        if (access(candidate.c_str(), F_OK) == 0)
            return candidate;
    }

    return "";
}

// --- Minimal JSON parser for wrapper.json ---

static std::string json_get_string(const char* json, const char* key) {
    char search[256];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char* pos = strstr(json, search);
    if (!pos) return "";
    pos = strchr(pos + strlen(search), ':');
    if (!pos) return "";
    pos = strchr(pos, '"');
    if (!pos) return "";
    pos++;
    const char* end = strchr(pos, '"');
    if (!end) return "";
    return std::string(pos, end - pos);
}

static double json_get_number(const char* json, const char* key) {
    char search[256];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char* pos = strstr(json, search);
    if (!pos) return 0.0;
    pos = strchr(pos + strlen(search), ':');
    if (!pos) return 0.0;
    while (*pos && (*pos == ' ' || *pos == ':')) pos++;
    return atof(pos);
}

static bool parse_wrapper_config(const char* bundle_path, WrapperConfig& config) {
    std::string path = std::string(bundle_path) + "wrapper.json";
    FILE* f = fopen(path.c_str(), "r");
    if (!f) {
        fprintf(stderr, "vst3-wrapper: cannot open %s\n", path.c_str());
        return false;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    std::vector<char> buf(size + 1);
    fread(buf.data(), 1, size, f);
    buf[size] = '\0';
    fclose(f);

    const char* json = buf.data();
    config.uri = json_get_string(json, "uri");
    config.name = json_get_string(json, "name");

    // Try vst3_bundle (relative name, resolved against search paths) first,
    // fall back to vst3_path (absolute, legacy)
    std::string vst3_bundle = json_get_string(json, "vst3_bundle");
    if (!vst3_bundle.empty()) {
        config.vst3_path = resolve_vst3_path(vst3_bundle);
        if (config.vst3_path.empty()) {
            fprintf(stderr, "vst3-wrapper: cannot find %s in any search path\n",
                    vst3_bundle.c_str());
            return false;
        }
    } else {
        config.vst3_path = json_get_string(json, "vst3_path");
    }
    config.audio_inputs = (uint32_t)json_get_number(json, "audio_inputs");
    config.audio_outputs = (uint32_t)json_get_number(json, "audio_outputs");

    // Parse parameters array
    const char* params = strstr(json, "\"parameters\"");
    if (params) {
        params = strchr(params, '[');
        if (params) {
            params++;
            while (*params) {
                const char* obj = strchr(params, '{');
                if (!obj) break;
                const char* obj_end = strchr(obj, '}');
                if (!obj_end) break;

                std::string chunk(obj, obj_end - obj + 1);
                ParamMapping pm;
                pm.lv2_index = (uint32_t)json_get_number(chunk.c_str(), "lv2_index");
                pm.vst3_param_id = (uint32_t)json_get_number(chunk.c_str(), "vst3_param_id");
                pm.min = (float)json_get_number(chunk.c_str(), "min");
                pm.max = (float)json_get_number(chunk.c_str(), "max");
                pm.def = (float)json_get_number(chunk.c_str(), "default");
                config.parameters.push_back(pm);

                params = obj_end + 1;
            }
        }
    }

    if (config.uri.empty() || config.vst3_path.empty()) {
        fprintf(stderr, "vst3-wrapper: invalid wrapper.json (missing uri or vst3_path)\n");
        return false;
    }

    return true;
}

// --- LV2 plugin callbacks ---

static LV2_Handle wrapper_instantiate(const LV2_Descriptor* descriptor,
                                      double rate,
                                      const char* bundle_path,
                                      const LV2_Feature* const* features) {
    (void)features;

    WrapperConfig* config = new WrapperConfig();
    if (!parse_wrapper_config(bundle_path, *config)) {
        delete config;
        return nullptr;
    }

    if (config->uri != descriptor->URI) {
        fprintf(stderr, "vst3-wrapper: URI mismatch: config=%s descriptor=%s\n",
                config->uri.c_str(), descriptor->URI);
        delete config;
        return nullptr;
    }

    WrapperInstance* inst = new WrapperInstance();
    inst->config = config;
    inst->sample_rate = rate;
    inst->buffer_size = 1024;
    memset(inst->port_buffers, 0, sizeof(inst->port_buffers));
    memset(inst->prev_values, 0, sizeof(inst->prev_values));

    // Set up host descriptor
    memset(&inst->host_desc, 0, sizeof(inst->host_desc));
    inst->host_desc.handle = inst;
    inst->host_desc.resourceDir = "";
    inst->host_desc.uiName = "stom-pi-vst3";
    inst->host_desc.get_buffer_size = host_get_buffer_size;
    inst->host_desc.get_sample_rate = host_get_sample_rate;
    inst->host_desc.is_offline = host_is_offline;
    inst->host_desc.get_time_info = host_get_time_info;
    inst->host_desc.write_midi_event = host_write_midi_event;
    inst->host_desc.ui_parameter_changed = host_ui_parameter_changed;
    inst->host_desc.ui_midi_program_changed = host_ui_midi_program_changed;
    inst->host_desc.ui_custom_data_changed = host_ui_custom_data_changed;
    inst->host_desc.ui_closed = host_ui_closed;
    inst->host_desc.dispatcher = host_dispatcher;

    inst->carla_desc = carla_get_native_rack_plugin();
    if (!inst->carla_desc) {
        fprintf(stderr, "vst3-wrapper: carla_get_native_rack_plugin() failed\n");
        delete config;
        delete inst;
        return nullptr;
    }

    inst->carla_handle = inst->carla_desc->instantiate(&inst->host_desc);
    if (!inst->carla_handle) {
        fprintf(stderr, "vst3-wrapper: Carla instantiate failed\n");
        delete config;
        delete inst;
        return nullptr;
    }

    inst->carla_host = carla_create_native_plugin_host_handle(
        inst->carla_desc, inst->carla_handle);
    if (!inst->carla_host) {
        fprintf(stderr, "vst3-wrapper: carla_create_native_plugin_host_handle failed\n");
        inst->carla_desc->cleanup(inst->carla_handle);
        delete config;
        delete inst;
        return nullptr;
    }

    // Set VST3 search path to the directory containing the .vst3 bundle
    std::string vst3_dir = config->vst3_path;
    size_t last_sep = vst3_dir.rfind('/');
    if (last_sep != std::string::npos)
        vst3_dir = vst3_dir.substr(0, last_sep);
    carla_set_engine_option(inst->carla_host, ENGINE_OPTION_PLUGIN_PATH,
                            PLUGIN_VST3, vst3_dir.c_str());

    // Load the VST3 plugin
    bool loaded = carla_add_plugin(inst->carla_host,
                                   BINARY_NATIVE, PLUGIN_VST3,
                                   config->vst3_path.c_str(),
                                   config->name.c_str(),
                                   "", 0, nullptr, 0);
    if (!loaded) {
        fprintf(stderr, "vst3-wrapper: failed to load VST3: %s (%s)\n",
                config->vst3_path.c_str(),
                carla_get_last_error(inst->carla_host));
        carla_host_handle_free(inst->carla_host);
        inst->carla_desc->cleanup(inst->carla_handle);
        delete config;
        delete inst;
        return nullptr;
    }

    // Activate the loaded plugin inside Carla
    carla_set_active(inst->carla_host, 0, true);

    // Allocate dummy buffers for mono plugins (Carla rack expects stereo)
    inst->dummy_size = 8192;
    inst->dummy_in = (float*)calloc(inst->dummy_size, sizeof(float));
    inst->dummy_out = (float*)calloc(inst->dummy_size, sizeof(float));

    // Port layout: control inputs, then audio inputs, then audio outputs
    inst->total_ports = (uint32_t)config->parameters.size()
                      + config->audio_inputs
                      + config->audio_outputs;

    // Set initial parameter values from defaults (Carla uses denormalized values)
    for (size_t i = 0; i < config->parameters.size(); i++) {
        const auto& pm = config->parameters[i];
        carla_set_parameter_value(inst->carla_host, 0, pm.vst3_param_id, pm.def);
        inst->prev_values[i] = pm.def;
    }

    fprintf(stderr, "vst3-wrapper: loaded %s (%zu params, %u in, %u out)\n",
            config->name.c_str(), config->parameters.size(),
            config->audio_inputs, config->audio_outputs);

    return inst;
}

static void wrapper_connect_port(LV2_Handle instance, uint32_t port, void* data) {
    WrapperInstance* inst = static_cast<WrapperInstance*>(instance);
    if (port < inst->total_ports) {
        inst->port_buffers[port] = static_cast<float*>(data);
    }
}

static void wrapper_activate(LV2_Handle instance) {
    WrapperInstance* inst = static_cast<WrapperInstance*>(instance);
    inst->carla_desc->activate(inst->carla_handle);
}

static void wrapper_run(LV2_Handle instance, uint32_t n_samples) {
    WrapperInstance* inst = static_cast<WrapperInstance*>(instance);
    const WrapperConfig* cfg = inst->config;
    uint32_t num_params = (uint32_t)cfg->parameters.size();

    // Sync control port values to VST3 parameters (Carla uses denormalized values)
    for (uint32_t i = 0; i < num_params; i++) {
        float* port = inst->port_buffers[i];
        if (!port) continue;

        float value = *port;
        if (value != inst->prev_values[i]) {
            const auto& pm = cfg->parameters[i];
            carla_set_parameter_value(inst->carla_host, 0, pm.vst3_param_id, value);
            inst->prev_values[i] = value;
        }
    }

    // Set up audio buffers — Carla rack always expects stereo
    float* inputs[2];
    float* outputs[2];

    inputs[0] = (cfg->audio_inputs > 0) ? inst->port_buffers[num_params] : inst->dummy_in;
    inputs[1] = (cfg->audio_inputs > 1) ? inst->port_buffers[num_params + 1] : inst->dummy_in;
    outputs[0] = (cfg->audio_outputs > 0) ? inst->port_buffers[num_params + cfg->audio_inputs] : inst->dummy_out;
    outputs[1] = (cfg->audio_outputs > 1) ? inst->port_buffers[num_params + cfg->audio_inputs + 1] : inst->dummy_out;

    // Clear dummy input buffer
    if (n_samples <= inst->dummy_size)
        memset(inst->dummy_in, 0, n_samples * sizeof(float));

    // Process audio through Carla
    inst->buffer_size = n_samples;
    inst->carla_desc->process(inst->carla_handle,
                              inputs, outputs, n_samples,
                              nullptr, 0);
}

static void wrapper_deactivate(LV2_Handle instance) {
    WrapperInstance* inst = static_cast<WrapperInstance*>(instance);
    inst->carla_desc->deactivate(inst->carla_handle);
}

static void wrapper_cleanup(LV2_Handle instance) {
    WrapperInstance* inst = static_cast<WrapperInstance*>(instance);

    carla_set_engine_about_to_close(inst->carla_host);
    carla_remove_all_plugins(inst->carla_host);
    carla_host_handle_free(inst->carla_host);
    inst->carla_desc->cleanup(inst->carla_handle);

    free(inst->dummy_in);
    free(inst->dummy_out);
    delete inst->config;
    delete inst;
}

// --- LV2 State ---

static LV2_State_Status wrapper_save(LV2_Handle instance,
                                      LV2_State_Store_Function store,
                                      LV2_State_Handle handle,
                                      uint32_t,
                                      const LV2_Feature* const* features) {
    WrapperInstance* inst = static_cast<WrapperInstance*>(instance);

    // Find URID map
    LV2_URID_Map* map = nullptr;
    for (int i = 0; features && features[i]; i++) {
        if (!strcmp(features[i]->URI, LV2_URID__map)) {
            map = static_cast<LV2_URID_Map*>(features[i]->data);
        }
    }
    if (!map) return LV2_STATE_ERR_NO_FEATURE;

    carla_prepare_for_save(inst->carla_host, 0);

    const char* chunk = carla_get_chunk_data(inst->carla_host, 0);
    if (chunk) {
        LV2_URID key = map->map(map->handle, "urn:stom-pi:vst3:state");
        LV2_URID type = map->map(map->handle, LV2_ATOM__String);
        store(handle, key, chunk, strlen(chunk) + 1, type,
              LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
    }

    return LV2_STATE_SUCCESS;
}

static LV2_State_Status wrapper_restore(LV2_Handle instance,
                                         LV2_State_Retrieve_Function retrieve,
                                         LV2_State_Handle handle,
                                         uint32_t,
                                         const LV2_Feature* const* features) {
    WrapperInstance* inst = static_cast<WrapperInstance*>(instance);

    LV2_URID_Map* map = nullptr;
    for (int i = 0; features && features[i]; i++) {
        if (!strcmp(features[i]->URI, LV2_URID__map)) {
            map = static_cast<LV2_URID_Map*>(features[i]->data);
        }
    }
    if (!map) return LV2_STATE_ERR_NO_FEATURE;

    LV2_URID key = map->map(map->handle, "urn:stom-pi:vst3:state");
    size_t size = 0;
    uint32_t type = 0;
    uint32_t flags = 0;
    const void* value = retrieve(handle, key, &size, &type, &flags);

    if (value && size > 0) {
        carla_set_chunk_data(inst->carla_host, 0, static_cast<const char*>(value));
    }

    return LV2_STATE_SUCCESS;
}

static const LV2_State_Interface state_interface = {
    wrapper_save,
    wrapper_restore
};

static const void* wrapper_extension_data(const char* uri) {
    if (!strcmp(uri, LV2_STATE__interface))
        return &state_interface;
    return nullptr;
}

// --- LV2 Library Descriptor ---

struct LibHandle {
    WrapperConfig config;
    LV2_Descriptor descriptor;
    char* uri_storage;
};

static void lib_cleanup(LV2_Lib_Handle handle) {
    LibHandle* lib = static_cast<LibHandle*>(handle);
    free(lib->uri_storage);
    delete lib;
}

static const LV2_Descriptor* lib_get_plugin(LV2_Lib_Handle handle, uint32_t index) {
    if (index != 0) return nullptr;
    LibHandle* lib = static_cast<LibHandle*>(handle);
    return &lib->descriptor;
}

extern "C" {

LV2_SYMBOL_EXPORT
const LV2_Lib_Descriptor* lv2_lib_descriptor(const char* bundle_path,
                                              const LV2_Feature* const*) {
    LibHandle* lib = new LibHandle();

    if (!parse_wrapper_config(bundle_path, lib->config)) {
        delete lib;
        return nullptr;
    }

    lib->uri_storage = strdup(lib->config.uri.c_str());

    lib->descriptor.URI = lib->uri_storage;
    lib->descriptor.instantiate = wrapper_instantiate;
    lib->descriptor.connect_port = wrapper_connect_port;
    lib->descriptor.activate = wrapper_activate;
    lib->descriptor.run = wrapper_run;
    lib->descriptor.deactivate = wrapper_deactivate;
    lib->descriptor.cleanup = wrapper_cleanup;
    lib->descriptor.extension_data = wrapper_extension_data;

    LV2_Lib_Descriptor* lib_desc = new LV2_Lib_Descriptor();
    lib_desc->handle = lib;
    lib_desc->size = sizeof(LV2_Lib_Descriptor);
    lib_desc->cleanup = lib_cleanup;
    lib_desc->get_plugin = lib_get_plugin;

    return lib_desc;
}

} // extern "C"
