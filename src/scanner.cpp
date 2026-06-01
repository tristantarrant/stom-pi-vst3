/*
 * Stom-Pi VST3 Scanner — introspects a VST3 bundle and outputs wrapper.json
 * Copyright (C) 2026 Dataforte <info@dataforte.net>
 * Licensed under GPLv2+
 *
 * Usage: stom-pi-vst3-scan <path-to.vst3> [output-dir]
 *
 * Outputs wrapper.json with parameter metadata for the LV2 wrapper.
 * If output-dir is omitted, writes to stdout.
 */

#include "CarlaNativePlugin.h"
#include "CarlaHost.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace CARLA_BACKEND_NAMESPACE;

struct ScanContext {
    uint32_t buffer_size;
    double sample_rate;
};

static uint32_t host_get_buffer_size(NativeHostHandle handle) {
    return static_cast<ScanContext*>(handle)->buffer_size;
}

static double host_get_sample_rate(NativeHostHandle handle) {
    return static_cast<ScanContext*>(handle)->sample_rate;
}

static bool host_is_offline(NativeHostHandle) { return false; }

static intptr_t host_dispatcher(NativeHostHandle, NativeHostDispatcherOpcode,
                                int32_t, intptr_t, void*, float) {
    return 0;
}

static std::string json_escape(const char* s) {
    std::string out;
    if (!s) return out;
    for (; *s; s++) {
        switch (*s) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            default:   out += *s; break;
        }
    }
    return out;
}

static std::string make_symbol(const char* name) {
    std::string sym;
    if (!name) return "param";
    for (const char* p = name; *p; p++) {
        char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') {
            sym += c;
        } else if (c >= 'A' && c <= 'Z') {
            sym += (char)(c + 32);
        } else if (c == ' ' || c == '-') {
            sym += '_';
        }
    }
    if (sym.empty()) sym = "param";
    return sym;
}

static std::string make_uri(const char* name) {
    std::string lower;
    if (!name) return "unknown";
    for (const char* p = name; *p; p++) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c += 32;
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')
            lower += c;
        else if (c == ' ')
            lower += '-';
    }
    return "urn:stom-pi:vst3:" + lower;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <path-to.vst3> [output-dir]\n", argv[0]);
        return 1;
    }

    const char* vst3_path = argv[1];
    const char* output_dir = (argc > 2) ? argv[2] : nullptr;

    ScanContext ctx;
    ctx.buffer_size = 512;
    ctx.sample_rate = 48000.0;

    NativeHostDescriptor host_desc;
    memset(&host_desc, 0, sizeof(host_desc));
    host_desc.handle = &ctx;
    host_desc.resourceDir = "";
    host_desc.uiName = "stom-pi-vst3-scan";
    host_desc.get_buffer_size = host_get_buffer_size;
    host_desc.get_sample_rate = host_get_sample_rate;
    host_desc.is_offline = host_is_offline;
    host_desc.dispatcher = host_dispatcher;

    const NativePluginDescriptor* carla_desc = carla_get_native_rack_plugin();
    if (!carla_desc) {
        fprintf(stderr, "Error: carla_get_native_rack_plugin() failed\n");
        return 1;
    }

    NativePluginHandle carla_handle = carla_desc->instantiate(&host_desc);
    if (!carla_handle) {
        fprintf(stderr, "Error: Carla instantiate failed\n");
        return 1;
    }

    CarlaHostHandle carla_host = carla_create_native_plugin_host_handle(
        carla_desc, carla_handle);
    if (!carla_host) {
        fprintf(stderr, "Error: carla_create_native_plugin_host_handle failed\n");
        carla_desc->cleanup(carla_handle);
        return 1;
    }

    // Set VST3 search path to the parent directory of the .vst3 bundle
    std::string vst3_dir(vst3_path);
    size_t last_sep = vst3_dir.rfind('/');
    if (last_sep != std::string::npos)
        vst3_dir = vst3_dir.substr(0, last_sep);
    carla_set_engine_option(carla_host, ENGINE_OPTION_PLUGIN_PATH,
                            PLUGIN_VST3, vst3_dir.c_str());

    bool loaded = carla_add_plugin(carla_host, BINARY_NATIVE, PLUGIN_VST3,
                                   vst3_path, "", "", 0, nullptr, 0);
    if (!loaded) {
        fprintf(stderr, "Error: failed to load VST3: %s (%s)\n",
                vst3_path, carla_get_last_error(carla_host));
        carla_host_handle_free(carla_host);
        carla_desc->cleanup(carla_handle);
        return 1;
    }

    // Get plugin info
    const CarlaPluginInfo* info = carla_get_plugin_info(carla_host, 0);
    const char* plugin_name = info ? info->name : "Unknown";
    const char* plugin_maker = (info && info->maker && info->maker[0]) ? info->maker : "";
    const char* plugin_copyright = (info && info->copyright && info->copyright[0]) ? info->copyright : "";
    const char* plugin_label = (info && info->label && info->label[0]) ? info->label : "";

    // Map Carla category to LV2 class
    const char* lv2_category = "lv2:Plugin";
    if (info) {
        switch (info->category) {
            case PLUGIN_CATEGORY_SYNTH:       lv2_category = "lv2:InstrumentPlugin"; break;
            case PLUGIN_CATEGORY_DELAY:       lv2_category = "lv2:ReverbPlugin"; break;
            case PLUGIN_CATEGORY_EQ:          lv2_category = "lv2:EQPlugin"; break;
            case PLUGIN_CATEGORY_FILTER:      lv2_category = "lv2:FilterPlugin"; break;
            case PLUGIN_CATEGORY_DISTORTION:  lv2_category = "lv2:DistortionPlugin"; break;
            case PLUGIN_CATEGORY_DYNAMICS:    lv2_category = "lv2:DynamicsPlugin"; break;
            case PLUGIN_CATEGORY_MODULATOR:   lv2_category = "lv2:ModulatorPlugin"; break;
            case PLUGIN_CATEGORY_UTILITY:     lv2_category = "lv2:UtilityPlugin"; break;
            default: break;
        }
    }

    // Get port counts
    const CarlaPortCountInfo* audio_info = carla_get_audio_port_count_info(carla_host, 0);
    uint32_t audio_ins = audio_info ? audio_info->ins : 0;
    uint32_t audio_outs = audio_info ? audio_info->outs : 0;

    // Get parameter info
    uint32_t param_count = carla_get_parameter_count(carla_host, 0);

    std::string uri = make_uri(plugin_name);

    // Build JSON
    FILE* out = stdout;
    if (output_dir) {
        std::string filepath = std::string(output_dir) + "/wrapper.json";
        out = fopen(filepath.c_str(), "w");
        if (!out) {
            fprintf(stderr, "Error: cannot write to %s\n", filepath.c_str());
            carla_host_handle_free(carla_host);
            carla_desc->cleanup(carla_handle);
            return 1;
        }
    }

    fprintf(out, "{\n");
    // Store just the bundle name, not the full path
    std::string vst3_basename(vst3_path);
    size_t sep = vst3_basename.rfind('/');
    if (sep != std::string::npos)
        vst3_basename = vst3_basename.substr(sep + 1);
    fprintf(out, "  \"vst3_bundle\": \"%s\",\n", json_escape(vst3_basename.c_str()).c_str());
    fprintf(out, "  \"uri\": \"%s\",\n", uri.c_str());
    fprintf(out, "  \"name\": \"%s\",\n", json_escape(plugin_name).c_str());
    if (plugin_maker[0])
        fprintf(out, "  \"maker\": \"%s\",\n", json_escape(plugin_maker).c_str());
    if (plugin_copyright[0])
        fprintf(out, "  \"license\": \"%s\",\n", json_escape(plugin_copyright).c_str());
    if (plugin_label[0])
        fprintf(out, "  \"label\": \"%s\",\n", json_escape(plugin_label).c_str());
    fprintf(out, "  \"category\": \"%s\",\n", lv2_category);
    fprintf(out, "  \"audio_inputs\": %u,\n", audio_ins);
    fprintf(out, "  \"audio_outputs\": %u,\n", audio_outs);
    fprintf(out, "  \"parameters\": [\n");

    uint32_t lv2_index = 0;
    for (uint32_t i = 0; i < param_count; i++) {
        const CarlaParameterInfo* pinfo = carla_get_parameter_info(carla_host, 0, i);
        const ParameterData* pdata = carla_get_parameter_data(carla_host, 0, i);

        // Skip output-only and non-automatable parameters
        if (pdata && (pdata->hints & PARAMETER_IS_READ_ONLY))
            continue;

        // Skip bypass parameter — LV2 handles bypass separately
        if (pinfo && pinfo->name) {
            std::string pname_lower = pinfo->name;
            for (auto& c : pname_lower) c = tolower(c);
            if (pname_lower == "bypass" || pname_lower == "fx_bypass" || pname_lower == "fxbypass")
                continue;
        }

        const ParameterRanges* ranges = carla_get_parameter_ranges(carla_host, 0, i);
        float def_val = carla_get_default_parameter_value(carla_host, 0, i);

        if (lv2_index > 0) fprintf(out, ",\n");
        fprintf(out, "    {\n");
        fprintf(out, "      \"lv2_index\": %u,\n", lv2_index);
        fprintf(out, "      \"lv2_symbol\": \"%s\",\n",
                make_symbol(pinfo ? pinfo->name : "param").c_str());
        fprintf(out, "      \"lv2_name\": \"%s\",\n",
                json_escape(pinfo ? pinfo->name : "Parameter").c_str());
        fprintf(out, "      \"vst3_param_id\": %u,\n", i);
        fprintf(out, "      \"min\": %g,\n", ranges ? ranges->min : 0.0);
        fprintf(out, "      \"max\": %g,\n", ranges ? ranges->max : 1.0);
        fprintf(out, "      \"default\": %g", def_val);
        if (pinfo && pinfo->unit && pinfo->unit[0]) {
            fprintf(out, ",\n      \"unit\": \"%s\"",
                    json_escape(pinfo->unit).c_str());
        }
        if (pdata && (pdata->hints & PARAMETER_IS_BOOLEAN)) {
            fprintf(out, ",\n      \"toggle\": true");
        }
        if (pdata && (pdata->hints & PARAMETER_IS_INTEGER)) {
            fprintf(out, ",\n      \"integer\": true");
        }
        fprintf(out, "\n    }");
        lv2_index++;
    }

    fprintf(out, "\n  ]\n");
    fprintf(out, "}\n");

    if (output_dir && out != stdout) {
        fclose(out);
        fprintf(stderr, "Wrote %s/wrapper.json: %s (%u params, %u in, %u out)\n",
                output_dir, plugin_name, lv2_index, audio_ins, audio_outs);
    }

    // Cleanup
    carla_set_engine_about_to_close(carla_host);
    carla_remove_all_plugins(carla_host);
    carla_host_handle_free(carla_host);
    carla_desc->cleanup(carla_handle);

    return 0;
}
