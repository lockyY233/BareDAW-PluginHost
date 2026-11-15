// tests/fixtures/fake_clap_plugin.cpp
//
// In-tree CLAP fixture loaded by tests/native/plugin_host_smoke.cpp. Exports
// `clap_entry` (a struct symbol, per CLAP convention) and a single plugin:
// "com.baredaw.testtone". The plugin is a 0-in / 2-out instrument that emits a
// sine wave at the frequency of the most recent MIDI note-on (440 Hz default).
//
// Phase 1 subset only — the smoke test exercises the minimum surface the
// ClapAdapter implements: init/destroy/activate/deactivate/start_processing/
// stop_processing/reset/process + audio-ports (1 stereo out) + note-ports
// (1 MIDI in) + params (1 "amplitude" param). Anything fancier is a future
// test fixture.

#include "clap.h"
#include "ext/audio-ports.h"
#include "ext/note-ports.h"
#include "ext/params.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{

// Per-instance state — one sine voice whose frequency tracks the last note-on.
struct TestToneState
{
    double sampleRate = 44100.0;
    double phase = 0.0;
    double freq = 440.0;
    float amplitude = 0.5f;
    bool active = false;
    bool processing = false;
};

// Audio-ports extension: 0 input ports, 1 stereo output port.
uint32_t ap_count(const clap_plugin_t *, bool is_input)
{
    return is_input ? 0u : 1u;
}

bool ap_get(const clap_plugin_t *, uint32_t /*index*/, bool is_input,
            clap_audio_port_info_t *info)
{
    if (is_input) return false;
    info->id = 1;
    std::strncpy(info->name, "main", CLAP_NAME_SIZE - 1);
    info->name[CLAP_NAME_SIZE - 1] = '\0';
    info->channel_count = 2;
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

const clap_plugin_audio_ports_t kAudioPorts = {ap_count, ap_get};

// Note-ports extension: 1 MIDI input port.
uint32_t np_count(const clap_plugin_t *, bool is_input)
{
    return is_input ? 1u : 0u;
}

bool np_get(const clap_plugin_t *, uint32_t /*index*/, bool is_input,
            clap_note_port_info_t *info)
{
    if (!is_input) return false;
    info->id = 1;
    std::strncpy(info->name, "midi_in", CLAP_NAME_SIZE - 1);
    info->name[CLAP_NAME_SIZE - 1] = '\0';
    info->supported_dialects = CLAP_NOTE_DIALECT_MIDI;
    info->preferred_dialect = CLAP_NOTE_DIALECT_MIDI;
    return true;
}

const clap_plugin_note_ports_t kNotePorts = {np_count, np_get};

// Params extension: 1 "amplitude" param (0..1, default 0.5).
uint32_t params_count(const clap_plugin_t *) { return 1; }

bool params_info(const clap_plugin_t *, uint32_t i, clap_param_info_t *info)
{
    if (i != 0) return false;
    info->id = 1;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    info->cookie = nullptr;
    std::strncpy(info->name, "amplitude", CLAP_NAME_SIZE - 1);
    info->name[CLAP_NAME_SIZE - 1] = '\0';
    std::strncpy(info->module, "fake_clap", CLAP_PATH_SIZE - 1);
    info->module[CLAP_PATH_SIZE - 1] = '\0';
    info->min_value = 0.0;
    info->max_value = 1.0;
    info->default_value = 0.5;
    return true;
}

bool params_value(const clap_plugin_t *plugin, clap_id id, double *out)
{
    if (id != 1) return false;
    *out = static_cast<double>(
        static_cast<TestToneState *>(plugin->plugin_data)->amplitude);
    return true;
}

bool params_value_to_text(const clap_plugin_t *, clap_id, double value,
                          char *out, uint32_t cap)
{
    std::snprintf(out, cap, "%f", value);
    return true;
}

bool params_text_to_value(const clap_plugin_t *, clap_id, const char *text,
                          double *out)
{
    char *end = nullptr;
    double v = strtod(text, &end);
    if (end == text) return false;
    *out = v;
    return true;
}

void params_flush(const clap_plugin_t *,
                  const clap_input_events_t *,
                  const clap_output_events_t *) {}

const clap_plugin_params_t kParams = {params_count, params_info, params_value,
                                      params_value_to_text, params_text_to_value,
                                      params_flush};

// Plugin lifecycle.
bool plugin_init(const clap_plugin_t *) { return true; }

void plugin_destroy(const clap_plugin_t *) {}

bool plugin_activate(const clap_plugin_t *plugin, double sample_rate,
                     uint32_t /*min_frames*/, uint32_t /*max_frames*/)
{
    auto *s = static_cast<TestToneState *>(plugin->plugin_data);
    s->sampleRate = sample_rate;
    s->phase = 0.0;
    s->active = true;
    return true;
}

void plugin_deactivate(const clap_plugin_t *plugin)
{
    auto *s = static_cast<TestToneState *>(plugin->plugin_data);
    s->active = false;
}

bool plugin_start_processing(const clap_plugin_t *plugin)
{
    static_cast<TestToneState *>(plugin->plugin_data)->processing = true;
    return true;
}

void plugin_stop_processing(const clap_plugin_t *plugin)
{
    static_cast<TestToneState *>(plugin->plugin_data)->processing = false;
}

void plugin_reset(const clap_plugin_t *plugin)
{
    auto *s = static_cast<TestToneState *>(plugin->plugin_data);
    s->phase = 0.0;
}

clap_process_status plugin_process(const clap_plugin_t *plugin,
                                    const clap_process_t *proc)
{
    auto *s = static_cast<TestToneState *>(plugin->plugin_data);
    if (!s->active || !s->processing) return CLAP_PROCESS_CONTINUE;

    // Ingest MIDI events — translate note-on to frequency. CLAP delivers MIDI
    // bytes in `clap_event_midi_t.data[0..2]`: [0]=status+channel, [1]=d1,
    // [2]=d2.
    if (proc->in_events) {
        const uint32_t n = proc->in_events->size(proc->in_events);
        for (uint32_t i = 0; i < n; ++i) {
            const clap_event_header_t *h =
                proc->in_events->get(proc->in_events, i);
            if (!h) continue;
            if (h->type == CLAP_EVENT_PARAM_VALUE) {
                const auto *param =
                    reinterpret_cast<const clap_event_param_value_t *>(h);
                if (param->param_id == 1)
                    s->amplitude = static_cast<float>(param->value);
                continue;
            }
            if (h->type != CLAP_EVENT_MIDI) continue;
            const auto *midi = reinterpret_cast<const clap_event_midi_t *>(h);
            const uint8_t status = midi->data[0] & 0xF0;
            const uint8_t note = midi->data[1];
            const uint8_t vel = midi->data[2];
            if (status == 0x90 && vel > 0) {
                // MIDI note → A4=440 Hz at note 69.
                s->freq = 440.0 * std::pow(2.0, (note - 69) / 12.0);
            } else if (status == 0x80 || (status == 0x90 && vel == 0)) {
                // Note-off — drop freq to a sub-audible default so we don't
                // keep emitting after release.
                s->freq = 0.0;
            }
        }
    }

    // Render audio. For v1 we just write a constant amplitude * sin(phase) to
    // every output channel (the smoke test only checks that output is non-zero
    // and bound by amplitude).
    const uint32_t frames = proc->frames_count;
    const float amp = s->amplitude;
    for (uint32_t i = 0; i < frames; ++i) {
        const float sample = (s->freq == 0.0) ? 0.0f : amp * static_cast<float>(std::sin(s->phase));
        for (uint32_t ch = 0; ch < proc->audio_outputs[0].channel_count; ++ch) {
            proc->audio_outputs[0].data32[ch][i] = sample;
        }
        s->phase += 2.0 * M_PI * s->freq / s->sampleRate;
        if (s->phase > 2.0 * M_PI) s->phase -= 2.0 * M_PI;
    }
    return CLAP_PROCESS_CONTINUE;
}

const void *plugin_get_extension(const clap_plugin_t *, const char *id)
{
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &kAudioPorts;
    if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &kParams;
    if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS)) return &kNotePorts;
    return nullptr;
}

void plugin_on_main_thread(const clap_plugin_t *) {}

// Single plugin instance + descriptor. factory->create_plugin returns a
// pointer to this; plugin_data points at the TestToneState.
TestToneState g_state{};
const clap_plugin_t g_plugin{
    /* desc          */ nullptr,    // patched below
    /* plugin_data   */ &g_state,
    /* init          */ plugin_init,
    /* destroy       */ plugin_destroy,
    /* activate      */ plugin_activate,
    /* deactivate    */ plugin_deactivate,
    /* start_processing */ plugin_start_processing,
    /* stop_processing  */ plugin_stop_processing,
    /* reset         */ plugin_reset,
    /* process       */ plugin_process,
    /* get_extension */ plugin_get_extension,
    /* on_main_thread*/ plugin_on_main_thread,
};

const char *const kFeatures[] = {"instrument", nullptr};

const clap_plugin_descriptor_t kDescriptor = {
    CLAP_VERSION,
    /* id          */ "com.baredaw.testtone",
    /* name        */ "TestTone",
    /* vendor      */ "BareDAW",
    /* url         */ "https://github.com/BareDAW/testtone",
    /* manual_url  */ "",
    /* support_url */ "",
    /* version     */ "0.1.0",
    /* description */ "Sine-tone fixture for plugin-host smoke tests.",
    /* features    */ kFeatures,
};

}

// Factory + entry — exported as the global symbols the ClapAdapter looks up.
uint32_t factory_get_plugin_count(const clap_plugin_factory_t *) { return 1; }

const clap_plugin_descriptor_t *factory_get_plugin_descriptor(
    const clap_plugin_factory_t *, uint32_t index)
{
    return index == 0 ? &kDescriptor : nullptr;
}

const clap_plugin_t *factory_create_plugin(const clap_plugin_factory_t *,
                                            const clap_host_t *,
                                            const char *plugin_id)
{
    if (!plugin_id || std::strcmp(plugin_id, kDescriptor.id) != 0) return nullptr;
    auto *p = new clap_plugin_t(g_plugin);
    p->desc = &kDescriptor;
    return p;
}

const clap_plugin_factory_t kFactory = {
    factory_get_plugin_count,
    factory_get_plugin_descriptor,
    factory_create_plugin,
};

const void *entry_get_factory(const char *factory_id)
{
    if (!std::strcmp(factory_id, CLAP_PLUGIN_FACTORY_ID)) return &kFactory;
    return nullptr;
}

bool entry_init(const char *) { return true; }
void entry_deinit(void) {}

const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION,
    /* init     */ entry_init,
    /* deinit   */ entry_deinit,
    /* get_factory */ entry_get_factory,
};
