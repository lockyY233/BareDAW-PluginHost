/*
 * ClapAdapter — implementation. See ClapAdapter.h for scope and docs/Plugin_Host_Plan.md
 * §8 for the subset of CLAP wrapped.
 */
#include "ClapAdapter.h"
#include "bridge/plugin-host/PluginDescriptor.h"
#include "bridge/plugin-host/HostApi.h"
#include "bridge/plugin-host/NativePluginEditor.h"

#include "clap/clap.h"
#include "clap/ext/audio-ports.h"
#include "clap/ext/gui.h"
#include "clap/ext/params.h"
#include "clap/ext/note-ports.h"

#include "core/Device.h"
#include "core/AudioBuffer.h"
#include "core/MidiBuffer.h"
#include "io/RawMIDIEvent.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace pluginhost::clapadapter
{

namespace
{

// Per-plugin-instance state: lives inside ClapPluginDevice, holds the CLAP
// plugin pointer + the extensions we touch + a scratch MIDI buffer fed by
// the engine's MidiBuffer in processMidi() and consumed in process().
struct ClapInstanceState
{
    const clap_plugin_t *plugin = nullptr;
    const clap_plugin_factory_t *factory = nullptr;
    const clap_host_t *host = nullptr;

    const clap_plugin_audio_ports_t *audioPorts = nullptr;
    const clap_plugin_gui_t *gui = nullptr;
    const clap_plugin_params_t *params = nullptr;
    const clap_plugin_note_ports_t *notePorts = nullptr;

    uint32_t audioInputCount = 0;
    uint32_t audioOutputCount = 0;
    std::vector<clap_audio_port_info_t> inputPortInfos;
    std::vector<clap_audio_buffer_t> inputBuffers;
    std::vector<std::vector<float *>> inputPortChannelPtrs;
    std::vector<std::vector<float>> sidechainSilence;
    clap_audio_port_info_t mainOutputInfo{};
    std::array<float *, 64> outputChannelPtrs{};
    std::vector<float> outputScratch;
    bool guiCreated = false;
    bool guiVisible = false;

    // Scratch for MIDI delivery during process(). Each event is a fully
    // formed clap_event_midi_t; the vtable below hands them out via
    // input_events.get(idx).
    static constexpr uint32_t kMaxMidiPerBlock = 256;
    clap_event_midi_t midiScratch[kMaxMidiPerBlock];
    uint32_t midiScratchCount = 0;
    static constexpr uint32_t kMaxParamsPerBlock = 256;
    clap_event_param_value_t paramScratch[kMaxParamsPerBlock];
    uint32_t paramScratchCount = 0;

    // input_events vtable — points into this ClapInstanceState via ctx.
    static uint32_t inEventsSize(const clap_input_events *list)
    {
        auto *self = reinterpret_cast<ClapInstanceState *>(list->ctx);
        return self->paramScratchCount + self->midiScratchCount;
    }
    static const clap_event_header_t *inEventsGet(const clap_input_events *list, uint32_t idx)
    {
        ClapInstanceState *self = reinterpret_cast<ClapInstanceState *>(list->ctx);
        if (idx < self->paramScratchCount)
            return &self->paramScratch[idx].header;
        idx -= self->paramScratchCount;
        if (idx >= self->midiScratchCount) return nullptr;
        return &self->midiScratch[idx].header;
    }
    clap_input_events_t inputEvents{nullptr, inEventsSize, inEventsGet};

    // output_events: we don't accept MIDI back from the plugin in v1. Drop.
    static bool outEventsTryPush(const clap_output_events *, const clap_event_header_t *) { return false; }
    clap_output_events_t outputEvents{nullptr, outEventsTryPush};
};

// Per-registration state — captured by the factory/destroy trampolines so
// each `DeviceRegistry::create(type)` knows which CLAP plugin to instantiate.
struct ClapRegistrationInfo
{
    const clap_plugin_factory_t *factory;
    clap_host_t host;
    const clap_plugin_descriptor_t *descriptor; // owned by the CLAP lib
    char *typeName;                              // owned by us (heap strdup)
    double sampleRate;
    uint32_t maxBlock;
    uint8_t acceptsIo;                           // 1=Midi, 2=Audio
    uint8_t producesIo;
    bool isInstrument = false;
    std::shared_ptr<std::atomic_uint32_t> liveInstances;
};

void CLAP_ABI hostGuiResizeHintsChanged(const clap_host_t *) {}
bool CLAP_ABI hostGuiRequestResize(const clap_host_t *, uint32_t, uint32_t) { return false; }
bool CLAP_ABI hostGuiRequestShow(const clap_host_t *) { return false; }
bool CLAP_ABI hostGuiRequestHide(const clap_host_t *) { return false; }
void CLAP_ABI hostGuiClosed(const clap_host_t *, bool) {}

const clap_host_gui_t kHostGui{
    hostGuiResizeHintsChanged,
    hostGuiRequestResize,
    hostGuiRequestShow,
    hostGuiRequestHide,
    hostGuiClosed,
};

const void *CLAP_ABI hostGetExtension(const clap_host_t *, const char *extensionId)
{
    if (extensionId && std::strcmp(extensionId, CLAP_EXT_GUI) == 0) return &kHostGui;
    return nullptr;
}

void CLAP_ABI hostRequestRestart(const clap_host_t *) {}
void CLAP_ABI hostRequestProcess(const clap_host_t *) {}
void CLAP_ABI hostRequestCallback(const clap_host_t *) {}

clap_host_t makeClapHost(void *hostData)
{
    clap_host_t host{};
    host.clap_version = CLAP_VERSION;
    host.host_data = hostData;
    host.name = "BareDAW";
    host.vendor = "Entropy Music";
    host.url = "https://github.com/EntropyMusic/BareDAW";
    host.version = "0.0.0-plugin-host";
    host.get_extension = hostGetExtension;
    host.request_restart = hostRequestRestart;
    host.request_process = hostRequestProcess;
    host.request_callback = hostRequestCallback;
    return host;
}

// The host-side Device subclass. One instance per DeviceRegistry::create().
// Wraps a freshly-cloned CLAP plugin instance and forwards Device calls.
class ClapPluginDevice : public Device, public NativePluginEditor
{
public:
    ClapPluginDevice(const ClapRegistrationInfo &info)
        : info_(info)
    {
        sampleRate = info.sampleRate;

        // One fresh CLAP plugin instance per Device. factory->create_plugin
        // is thread-safe per CLAP; plugin->init / activate / start_processing
        // are main-thread only — and main.mm's runInteractive (the audio
        // thread between blocks) counts as "main thread" for CLAP.
        state_.plugin = info.factory->create_plugin(info.factory, &info_.host,
                                                    info.descriptor->id);
        if (!state_.plugin) return;

        state_.factory = info.factory;
        state_.host = &info_.host;

        state_.inputEvents.ctx = &state_;
        state_.outputEvents.ctx = &state_;

        if (!state_.plugin->init(state_.plugin)) {
            state_.plugin->destroy(state_.plugin);
            state_.plugin = nullptr;
            return;
        }

        // Extensions may only be queried after plugin->init(). Any extension
        // can be absent: effects need no note ports and MIDI effects need no
        // audio ports.
        state_.audioPorts = static_cast<const clap_plugin_audio_ports_t *>(
            state_.plugin->get_extension(state_.plugin, CLAP_EXT_AUDIO_PORTS));
        state_.gui = static_cast<const clap_plugin_gui_t *>(
            state_.plugin->get_extension(state_.plugin, CLAP_EXT_GUI));
        state_.params = static_cast<const clap_plugin_params_t *>(
            state_.plugin->get_extension(state_.plugin, CLAP_EXT_PARAMS));
        state_.notePorts = static_cast<const clap_plugin_note_ports_t *>(
            state_.plugin->get_extension(state_.plugin, CLAP_EXT_NOTE_PORTS));

        // Audio-port queries are main-thread-only. Cache the supported layout
        // now, before activation, so process() never calls the port extension.
        if (state_.audioPorts) {
            state_.audioInputCount = state_.audioPorts->count(state_.plugin, true);
            state_.audioOutputCount = state_.audioPorts->count(state_.plugin, false);
            state_.inputPortInfos.resize(state_.audioInputCount);
            for (uint32_t port = 0; port < state_.audioInputCount; ++port) {
                if (!state_.audioPorts->get(state_.plugin, port, true,
                                            &state_.inputPortInfos[port])) {
                    state_.plugin->destroy(state_.plugin);
                    state_.plugin = nullptr;
                    return;
                }
            }
            if (state_.audioOutputCount == 1 &&
                !state_.audioPorts->get(state_.plugin, 0, false,
                                        &state_.mainOutputInfo)) {
                state_.plugin->destroy(state_.plugin);
                state_.plugin = nullptr;
                return;
            }
        }
        if (state_.audioInputCount > 8 || state_.audioOutputCount != 1 ||
            state_.mainOutputInfo.channel_count == 0 ||
            state_.mainOutputInfo.channel_count > state_.outputChannelPtrs.size()) {
            state_.plugin->destroy(state_.plugin);
            state_.plugin = nullptr;
            return;
        }
        state_.outputScratch.resize(static_cast<size_t>(info.maxBlock) *
                                    state_.mainOutputInfo.channel_count);
        for (uint32_t ch = 0; ch < state_.mainOutputInfo.channel_count; ++ch)
            state_.outputChannelPtrs[ch] = state_.outputScratch.data() +
                                           static_cast<size_t>(ch) * info.maxBlock;

        state_.inputBuffers.resize(state_.audioInputCount);
        state_.inputPortChannelPtrs.resize(state_.audioInputCount);
        state_.sidechainSilence.resize(state_.audioInputCount);
        for (uint32_t port = 0; port < state_.audioInputCount; ++port) {
            const uint32_t channels = state_.inputPortInfos[port].channel_count;
            if (channels == 0 || channels > 64) {
                state_.plugin->destroy(state_.plugin);
                state_.plugin = nullptr;
                return;
            }
            state_.inputPortChannelPtrs[port].resize(channels);
            if (port > 0)
                state_.sidechainSilence[port].resize(static_cast<size_t>(channels) *
                                                     info.maxBlock, 0.0f);
            state_.inputBuffers[port].channel_count = channels;
            state_.inputBuffers[port].data32 = state_.inputPortChannelPtrs[port].data();
        }

        if (!state_.plugin->activate(state_.plugin, info.sampleRate,
                                     /*min_frames*/ 1,
                                     /*max_frames*/ info.maxBlock)) {
            state_.plugin->destroy(state_.plugin);
            state_.plugin = nullptr;
            return;
        }
        if (!state_.plugin->start_processing(state_.plugin)) {
            state_.plugin->deactivate(state_.plugin);
            state_.plugin->destroy(state_.plugin);
            state_.plugin = nullptr;
            return;
        }

        // Stamp type name for project save/load — DeviceRegistry normally does
        // this on create(), but we cast through void* so do it ourselves.
        setTypeName(info.typeName);
        active_ = true;
        ++*info_.liveInstances;
        counted_ = true;
    }

    ~ClapPluginDevice() override
    {
        debugUi("instance.destructor.begin",
                "processBlocks=" + std::to_string(processCallCount()));
        if (state_.plugin) {
            destroy();
            if (active_) {
                state_.plugin->stop_processing(state_.plugin);
                state_.plugin->deactivate(state_.plugin);
            }
            state_.plugin->destroy(state_.plugin);
        }
        if (counted_) --*info_.liveInstances;
        debugUi("instance.destructor.end");
    }

    // IO derived at registration time (the descriptor is constant across
    // instances of the same plugin type). The PluginManager hands us the
    // accepts/produces flags via info_; we just expose them.
    DeviceIO accepts() const override
    {
        return static_cast<DeviceIO>(info_.acceptsIo);
    }
    DeviceIO produces() const override
    {
        return static_cast<DeviceIO>(info_.producesIo);
    }

    void prepare(double sr) override { setSampleRate(sr); }

    bool isValid() const { return active_ && state_.plugin; }
    void debugUi(const char *action, const std::string &detail = {}) const
    {
        const char *name = info_.descriptor && info_.descriptor->name
            ? info_.descriptor->name : (info_.typeName ? info_.typeName : "<unknown>");
        std::fprintf(stderr, "[plugin-debug clap] %s  plugin=\"%s\"%s%s\n",
                     action, name, detail.empty() ? "" : "  ", detail.c_str());
        std::fflush(stderr);
    }
    // CLAP instruments may request processing while the transport is stopped,
    // and queued note-offs must always reach process(). The current engine's
    // continuous-instrument capability is expressed through hasTail().
    bool hasTail() const override { return info_.isInstrument; }
    bool requiresContinuousProcessing() const override { return true; }
    uint64_t processCallCount() const override
    {
        return processCalls_.load(std::memory_order_relaxed);
    }

    void *queryExtension(const char *extensionId) override
    {
        return extensionId && std::strcmp(extensionId, kNativePluginEditorExtension) == 0
                   ? static_cast<NativePluginEditor *>(this)
                   : nullptr;
    }

    bool supports(const char *api) const override
    {
        return active_ && state_.plugin && state_.gui && api &&
               state_.gui->is_api_supported &&
               state_.gui->is_api_supported(state_.plugin, api, false);
    }

    bool create(const char *api, void *parent,
                uint32_t &width, uint32_t &height) override
    {
        debugUi("editor.create.begin", api ? api : "<null>");
        if (!parent || !supports(api)) return false;
        if (!state_.guiCreated) {
            debugUi("editor.gui.create.begin");
            if (!state_.gui->create || !state_.gui->create(state_.plugin, api, false))
                return false;
            debugUi("editor.gui.create.end", "success");
            state_.guiCreated = true;

            // CLAP's required embedded-GUI order is create → get_size →
            // set_parent → show. In particular, some editors size their child
            // view during set_parent and need the host to know that size first.
            if (!state_.gui->get_size ||
                !state_.gui->get_size(state_.plugin, &width, &height) ||
                width == 0 || height == 0) {
                destroy();
                return false;
            }
            debugUi("editor.get_size", std::to_string(width) + "x" +
                                       std::to_string(height));

            clap_window_t window{};
            window.api = api;
            window.ptr = parent;
            if (!state_.gui->set_parent ||
                !state_.gui->set_parent(state_.plugin, &window)) {
                debugUi("editor.set_parent.end", "failed");
                state_.gui->destroy(state_.plugin);
                state_.guiCreated = false;
                return false;
            }
            debugUi("editor.set_parent.end", "success");
            debugUi("editor.create.end");
            return true;
        }

        if (!state_.gui->get_size ||
            !state_.gui->get_size(state_.plugin, &width, &height) ||
            width == 0 || height == 0) {
            destroy();
            return false;
        }
        debugUi("editor.create.end", std::to_string(width) + "x" +
                                     std::to_string(height));
        return true;
    }

    bool setVisible(bool visible) override
    {
        if (!state_.guiCreated || !state_.gui) return false;
        if (state_.guiVisible == visible) return true;
        debugUi(visible ? "editor.show.begin" : "editor.hide.begin");
        const bool ok = visible
                            ? (state_.gui->show && state_.gui->show(state_.plugin))
                            : (state_.gui->hide && state_.gui->hide(state_.plugin));
        if (ok) state_.guiVisible = visible;
        debugUi(visible ? "editor.show.end" : "editor.hide.end",
                ok ? "success" : "failed");
        return ok;
    }

    void destroy() override
    {
        if (!state_.guiCreated || !state_.gui) return;
        debugUi("editor.destroy.begin");
        if (state_.guiVisible && state_.gui->hide)
            state_.gui->hide(state_.plugin);
        if (state_.gui->destroy) state_.gui->destroy(state_.plugin);
        state_.guiCreated = false;
        state_.guiVisible = false;
        debugUi("editor.destroy.end");
    }

    // MVP audio subset: one main output plus up to eight input buses. Port 0 is
    // the engine signal; additional buses (typically sidechains) receive
    // silence until BareDAW has explicit sidechain routing.
    void process(AudioBuffer &buffer, int frames, int writeOffset,
                 int64_t globalSamplePosition) override
    {
        if (!active_ || !state_.plugin || !state_.audioPorts) return;
        const uint32_t inCount = state_.audioInputCount;
        const auto &outInfo = state_.mainOutputInfo;
        if (outInfo.channel_count > static_cast<uint32_t>(buffer.numChannels())) return;
        const int availableCh = static_cast<int>(outInfo.channel_count);
        const int availableFr = std::min<int>(frames, buffer.numFrames() - writeOffset);
        if (availableCh <= 0 || availableFr <= 0 ||
            availableFr > static_cast<int>(info_.maxBlock)) return;

        // Render into preallocated scratch, then copy back into the engine
        // buffer. This works even when a plugin does not support in-place I/O.
        clap_audio_buffer_t outBuf{};
        outBuf.channel_count = static_cast<uint32_t>(availableCh);
        outBuf.data32 = state_.outputChannelPtrs.data();
        outBuf.constant_mask = 0;
        outBuf.latency = 0;

        for (uint32_t port = 0; port < inCount; ++port) {
            const uint32_t channels = state_.inputPortInfos[port].channel_count;
            if (port == 0) {
                if (channels > static_cast<uint32_t>(buffer.numChannels())) return;
                for (uint32_t ch = 0; ch < channels; ++ch)
                    state_.inputPortChannelPtrs[port][ch] =
                        buffer.channel(static_cast<int>(ch)) + writeOffset;
                state_.inputBuffers[port].constant_mask = 0;
            } else {
                auto &silence = state_.sidechainSilence[port];
                for (uint32_t ch = 0; ch < channels; ++ch) {
                    state_.inputPortChannelPtrs[port][ch] =
                        silence.data() + static_cast<size_t>(ch) * info_.maxBlock;
                    std::fill_n(state_.inputPortChannelPtrs[port][ch], availableFr, 0.0f);
                }
                state_.inputBuffers[port].constant_mask =
                    channels >= 64 ? UINT64_MAX : ((uint64_t{1} << channels) - 1);
            }
            state_.inputBuffers[port].latency = 0;
        }

        clap_process_t proc{};
        proc.steady_time = globalSamplePosition;
        proc.frames_count = static_cast<uint32_t>(availableFr);
        proc.audio_inputs = inCount ? state_.inputBuffers.data() : nullptr;
        proc.audio_inputs_count = inCount;
        proc.audio_outputs = &outBuf;
        proc.audio_outputs_count = 1;
        proc.in_events = &state_.inputEvents;
        proc.out_events = &state_.outputEvents;
        proc.transport = nullptr;

        processCalls_.fetch_add(1, std::memory_order_relaxed);
        const clap_process_status status = state_.plugin->process(state_.plugin, &proc);
        if (status != CLAP_PROCESS_ERROR) {
            for (int ch = 0; ch < availableCh; ++ch) {
                std::copy_n(state_.outputChannelPtrs[ch], availableFr,
                            buffer.channel(ch) + writeOffset);
            }
        }

        // After process: zero out the scratch so next block's MIDI queue starts
        // empty (we re-fill from processMidi each block).
        state_.midiScratchCount = 0;
        state_.paramScratchCount = 0;
    }

    void processMidi(const MidiBuffer &midi, int64_t globalSamplePosition) override
    {
        // Convert engine's RawMidiEvent list to a CLAP clap_event_midi_t array
        // for delivery during the upcoming process(). BareDAW delivers all
        // block-scoped MIDI before process() (Track.cpp:100), so the scratch
        // is guaranteed to be populated when process() runs.
        const int capacity = static_cast<int>(ClapInstanceState::kMaxMidiPerBlock -
                                              state_.midiScratchCount);
        const int n = std::min<int>(midi.count, capacity);
        for (int i = 0; i < n; ++i) {
            clap_event_midi_t &ev = state_.midiScratch[state_.midiScratchCount++];
            std::memset(&ev, 0, sizeof(ev));
            ev.header.size = sizeof(clap_event_midi_t);
            const int64_t offset = static_cast<int64_t>(midi.events[i].sampleTime) -
                                   globalSamplePosition;
            ev.header.time = static_cast<uint32_t>(std::clamp<int64_t>(
                offset, 0, static_cast<int64_t>(info_.maxBlock - 1)));
            ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            ev.header.type = static_cast<uint16_t>(CLAP_EVENT_MIDI);
            ev.header.flags = 0;
            ev.port_index = 0;
            ev.data[0] = midi.events[i].status;
            ev.data[1] = midi.events[i].d1;
            ev.data[2] = midi.events[i].d2;
        }
    }

    void setSampleRate(double sr) override
    {
        if (sr <= 0.0) return;
        if (std::fabs(sr - sampleRate) < 0.001) return;
        if (state_.plugin && active_) {
            state_.plugin->stop_processing(state_.plugin);
            state_.plugin->deactivate(state_.plugin);
            active_ = false;
            if (state_.plugin->activate(state_.plugin, sr, 1, info_.maxBlock)) {
                if (state_.plugin->start_processing(state_.plugin))
                    active_ = true;
                else
                    state_.plugin->deactivate(state_.plugin);
            }
        }
        sampleRate = sr;
        info_.sampleRate = sr;
    }

    void onStop() override
    {
        if (state_.plugin) {
            state_.plugin->reset(state_.plugin); // CLAP-defined: kills voices, resets state
        }
    }

    void allNotesOff() override
    {
        // Send all-notes-off CCs (123) on every channel. CLAP plugins that
        // don't implement note-ports are unaffected.
        if (!active_ || !state_.plugin) return;
        // 16 channels. We pack as many as possible.
        for (uint8_t ch = 0; ch < 16 && state_.midiScratchCount < ClapInstanceState::kMaxMidiPerBlock; ++ch) {
            clap_event_midi_t &ev = state_.midiScratch[state_.midiScratchCount++];
            std::memset(&ev, 0, sizeof(ev));
            ev.header.size = sizeof(clap_event_midi_t);
            ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            ev.header.type = static_cast<uint16_t>(CLAP_EVENT_MIDI);
            ev.port_index = 0;
            ev.data[0] = static_cast<uint8_t>(0xB0 | (ch & 0x0F)); // CC, channel ch
            ev.data[1] = 123; // all notes off
            ev.data[2] = 0;
        }
        // The next process block consumes these events; processMidi appends any
        // regular block MIDI without clearing this pending cleanup batch.
    }

    std::vector<InstrumentParameterDescriptor> getParameterDescriptors() const override
    {
        if (!active_ || !state_.params) return {};
        if (!parameterDescriptorsCached_) {
            const uint32_t n = state_.params->count(state_.plugin);
            parameterDescriptors_.reserve(n);
            for (uint32_t i = 0; i < n; ++i) {
                clap_param_info_t info{};
                if (!state_.params->get_info(state_.plugin, i, &info)) continue;
                if ((info.flags & CLAP_PARAM_IS_HIDDEN) != 0 ||
                    (info.flags & CLAP_PARAM_IS_READONLY) != 0) continue;

                InstrumentParameterDescriptor d;
                char idBuf[32];
                std::snprintf(idBuf, sizeof(idBuf), "%u", info.id);
                d.id = idBuf;
                d.name = info.name;
                d.minValue = static_cast<float>(info.min_value);
                d.maxValue = static_cast<float>(info.max_value);
                d.step = (info.flags & CLAP_PARAM_IS_STEPPED) ? 1.0f : 0.0f;
                d.defaultValue = static_cast<float>(info.default_value);
                d.currentValue = d.defaultValue;

                const int64_t enumMin = static_cast<int64_t>(std::ceil(info.min_value));
                const int64_t enumMax = static_cast<int64_t>(std::floor(info.max_value));
                if (d.step > 0.0f && enumMin == 0 && enumMax >= enumMin &&
                    enumMax - enumMin < 32 && state_.params->value_to_text) {
                    for (int64_t value = enumMin; value <= enumMax; ++value) {
                        char text[CLAP_NAME_SIZE]{};
                        if (!state_.params->value_to_text(state_.plugin, info.id,
                                                         static_cast<double>(value),
                                                         text, sizeof(text))) break;
                        d.enumValues.emplace_back(text);
                    }
                    if (!d.enumValues.empty()) d.type = "enum";
                }
                if ((info.flags & CLAP_PARAM_IS_BYPASS) != 0) {
                    d.style = "button";
                    if (d.enumValues.empty()) d.enumValues = {"Off", "On"};
                }
                parameterDescriptors_.push_back(std::move(d));
            }
            parameterDescriptorsCached_ = true;
        }

        auto out = parameterDescriptors_;
        for (auto &d : out) {
            double value = d.currentValue;
            const clap_id paramId = static_cast<clap_id>(std::strtoul(d.id.c_str(), nullptr, 10));
            if (state_.params->get_value(state_.plugin, paramId, &value))
                d.currentValue = static_cast<float>(value);
        }
        return out;
    }

    bool setParameterValue(const std::string &id, float value) override
    {
        if (!active_ || !state_.params) return false;
        // We only support integer param ids in v1 (CLAP's typical case).
        char *end = nullptr;
        const unsigned long parsed = std::strtoul(id.c_str(), &end, 10);
        if (end == id.c_str() || *end != '\0') return false;
        const clap_plugin_params_t *params = state_.params;
        const uint32_t count = params->count(state_.plugin);
        for (uint32_t i = 0; i < count; ++i) {
            clap_param_info_t info{};
            if (!params->get_info(state_.plugin, i, &info)) continue;
            if (info.id != static_cast<clap_id>(parsed)) continue;
            if (state_.paramScratchCount >= ClapInstanceState::kMaxParamsPerBlock)
                return false;
            clap_event_param_value_t &ev =
                state_.paramScratch[state_.paramScratchCount++];
            std::memset(&ev, 0, sizeof(ev));
            ev.header.size = sizeof(ev);
            ev.header.time = 0;
            ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            ev.header.type = CLAP_EVENT_PARAM_VALUE;
            ev.param_id = info.id;
            ev.cookie = info.cookie;
            ev.note_id = -1;
            ev.port_index = -1;
            ev.channel = -1;
            ev.key = -1;
            ev.value = std::clamp<double>(value, info.min_value, info.max_value);
            return true;
        }
        return false;
    }

    bool getParameterValue(const std::string &id, float &value) const override
    {
        if (!active_ || !state_.params) return false;
        char *end = nullptr;
        const unsigned long parsed = std::strtoul(id.c_str(), &end, 10);
        if (end == id.c_str() || *end != '\0') return false;
        double v = 0.0;
        if (!state_.params->get_value(state_.plugin,
                                       static_cast<clap_id>(parsed), &v)) return false;
        value = static_cast<float>(v);
        return true;
    }

private:
    ClapRegistrationInfo info_;
    ClapInstanceState state_;
    bool active_ = false;
    bool counted_ = false;
    std::atomic<uint64_t> processCalls_{0};
    mutable bool parameterDescriptorsCached_ = false;
    mutable std::vector<InstrumentParameterDescriptor> parameterDescriptors_;
};

// ──── registration glue ──────────────────────────────────────────────────

// Trampolines invoked by PluginManager's register_device callback when
// DeviceRegistry::create(typeName) is called.
void *CLAP_ABI clapFactory(void *hostCtx, double sampleRate, uint32_t maxBlock)
{
    auto *info = reinterpret_cast<ClapRegistrationInfo *>(hostCtx);
    const char *name = info->descriptor && info->descriptor->name
        ? info->descriptor->name : (info->typeName ? info->typeName : "<unknown>");
    std::fprintf(stderr, "[plugin-debug clap] instance.create.begin  plugin=\"%s\" sr=%.0f block=%u\n",
                 name, sampleRate, maxBlock);
    std::fflush(stderr);
    // The registration info has fixed sample rate / block size at registration
    // time. The factory trampoline signature takes fresh sr/block, but for v1
    // we use the registration-time values (the host's engine sample rate doesn't
    // change mid-session). If the host passes different values we honour theirs.
    ClapRegistrationInfo live = *info;
    live.sampleRate = sampleRate;
    live.maxBlock = maxBlock;
    auto *dev = new ClapPluginDevice(live);
    if (!dev->isValid()) {
        std::fprintf(stderr, "[plugin-debug clap] instance.create.end  plugin=\"%s\" failed\n", name);
        std::fflush(stderr);
        delete dev;
        return nullptr;
    }
    std::fprintf(stderr, "[plugin-debug clap] instance.create.end  plugin=\"%s\" success ptr=%p\n",
                 name, static_cast<void *>(dev));
    std::fflush(stderr);
    // The DeviceRegistry::create path expects a valid Device*. ClapPluginDevice
    // IS a Device — return opaque.
    return dev;
}

void CLAP_ABI clapDestroy(void * /*hostCtx*/, void *instance)
{
    // ClapPluginDevice's destructor handles deactivate/stop_processing/destroy.
    auto *device = reinterpret_cast<ClapPluginDevice *>(instance);
    if (device) device->debugUi("instance.destroy.begin");
    delete device;
    std::fprintf(stderr, "[plugin-debug clap] instance.destroy.end ptr=%p\n", instance);
    std::fflush(stderr);
}

// Read audio port / note port counts and produce the (accepts, produces) IO
// byte the DeviceRegistry expects.
void computeIo(const clap_plugin_t *plugin,
               const clap_plugin_audio_ports_t *audioPorts,
               const clap_plugin_note_ports_t *notePorts,
               uint8_t &acceptsOut, uint8_t &producesOut)
{
    uint8_t acc = 0;
    uint8_t prod = 0;
    if (notePorts) {
        if (notePorts->count(plugin, true) > 0) acc |= 1; // Midi in
        if (notePorts->count(plugin, false) > 0) prod |= 1; // Midi out
    }
    if (audioPorts) {
        if (audioPorts->count(plugin, true) > 0) acc |= 2; // Audio in
        if (audioPorts->count(plugin, false) > 0) prod |= 2; // Audio out
    }
    acceptsOut = acc;
    producesOut = prod;
}

} // anonymous namespace

// Public entry — see ClapAdapter.h.
size_t registerFromLib(void * /*dlHandle*/,
                       const char *libPath,
                       const clap_plugin_entry_t *entry,
                       double sampleRate,
                       uint32_t maxBlock,
                       const BareDAW_HostApi &hostApi,
                       std::vector<void *> &ownedRegistrationData)
{
    if (!entry) return 0;
    if (!clap_version_is_compatible(entry->clap_version)) {
        hostApi.log(hostApi.host_ctx, 1, "incompatible CLAP entry version");
        return 0;
    }

    // CLAP § entry.h: init() must be called before any other symbol from this
    // DSO. Idempotent — we make a single call. PluginManager handles deinit()
    // on unload (after unregister_device for every registered type).
    if (!entry->init(libPath)) {
        hostApi.log(hostApi.host_ctx, 1, "clap_entry.init failed");
        return 0;
    }

    auto *factory = static_cast<const clap_plugin_factory_t *>(
        entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    if (!factory) {
        hostApi.log(hostApi.host_ctx, 1, "no clap.plugin-factory in this DSO");
        entry->deinit();
        return 0;
    }

    clap_host_t probeHost = makeClapHost(hostApi.host_ctx);

    const uint32_t pluginCount = factory->get_plugin_count(factory);
    size_t registered = 0;
    for (uint32_t i = 0; i < pluginCount; ++i) {
        const clap_plugin_descriptor_t *desc = factory->get_plugin_descriptor(factory, i);
        if (!desc || !desc->id) {
            hostApi.log(hostApi.host_ctx, 1,
                        "clap plugin descriptor missing id; skipping");
            continue;
        }

        // Probe: create a temporary plugin instance to inspect audio ports /
        // note ports. We then immediately destroy — only register_device the
        // success path. This costs an init/activate/deactivate/destroy per
        // plugin at scan time, but it's a one-time scan cost and avoids
        // needing two probe passes (descriptor-only + per-instance).
        //
        // CLAP says create_plugin / init / activate / destroy are main-thread
        // only. registerFromLib is called from PluginManager::scanAndLoad()
        // on the control thread, which counts as main-thread for CLAP.
        const clap_plugin_t *probe =
            factory->create_plugin(factory,
                                   &probeHost,
                                   desc->id);
        if (!probe) {
            hostApi.log(hostApi.host_ctx, 1,
                        (std::string("clap plugin create_plugin returned null: ") + desc->id).c_str());
            continue;
        }

        if (!probe->init(probe)) {
            hostApi.log(hostApi.host_ctx, 1,
                        (std::string("clap plugin init failed: ") + desc->id).c_str());
            probe->destroy(probe);
            continue;
        }

        const clap_plugin_audio_ports_t *audioPorts = static_cast<const clap_plugin_audio_ports_t *>(
            probe->get_extension(probe, CLAP_EXT_AUDIO_PORTS));
        const clap_plugin_note_ports_t *notePorts = static_cast<const clap_plugin_note_ports_t *>(
            probe->get_extension(probe, CLAP_EXT_NOTE_PORTS));

        // Capture port counts before destroying the initialized probe. Missing
        // extensions simply mean that kind of I/O/parameter surface is absent.
        uint8_t acceptsIo = 0;
        uint8_t producesIo = 0;
        computeIo(probe, audioPorts, notePorts, acceptsIo, producesIo);
        if (acceptsIo == 0 && producesIo == 0) {
            hostApi.log(hostApi.host_ctx, 1,
                        (std::string("clap plugin has no discoverable audio or note ports: ")
                         + desc->id).c_str());
            probe->destroy(probe);
            continue;
        }

        // Tear down the probe before registering — DeviceRegistry::create
        // will create a fresh instance via our factory trampoline.
        probe->destroy(probe);

        // Build per-registration info (heap-allocated; outlives the device).
        auto *info = new ClapRegistrationInfo();
        info->factory = factory;
        info->host = makeClapHost(hostApi.host_ctx);
        info->descriptor = desc;
        info->typeName = strdup(desc->id);
        info->sampleRate = sampleRate;
        info->maxBlock = maxBlock;
        bool declaredInstrument = false;
        if (desc->features) {
            for (const char *const *feature = desc->features; *feature; ++feature) {
                if (std::strcmp(*feature, CLAP_PLUGIN_FEATURE_INSTRUMENT) == 0) {
                    declaredInstrument = true;
                    break;
                }
            }
        }
        info->isInstrument = declaredInstrument ||
            ((acceptsIo & 1) != 0 && (acceptsIo & 2) == 0 && (producesIo & 2) != 0);
        if (info->isInstrument)
            acceptsIo &= static_cast<uint8_t>(~2u); // optional audio input is not chain role
        info->acceptsIo = acceptsIo;
        info->producesIo = producesIo;
        info->liveInstances = std::make_shared<std::atomic_uint32_t>(0);

        // Register. Pass `info` as the hostCtx argument — not hostApi.host_ctx
        // — so the PluginManager's trampoline stashes our per-type
        // ClapRegistrationInfo and clapFactory receives it on the next
        // DeviceRegistry::create(). PluginManager owns no plugin memory; we
        // own info and its typeName until PluginManager::unloadAll runs
        // (which calls unregister_device, our cleanup hook).
        const int rc = hostApi.register_device(
            info,
            info->typeName,
            clapFactory,
            clapDestroy,
            info->acceptsIo,
            info->producesIo);
        if (rc != 0) {
            hostApi.log(hostApi.host_ctx, 1,
                        (std::string("clap plugin type already registered: ") + desc->id).c_str());
            free(info->typeName);
            delete info;
            continue;
        }
        ownedRegistrationData.push_back(info);
        hostApi.log(hostApi.host_ctx, 0,
                    (std::string("registered CLAP device: ") + desc->id).c_str());
        ++registered;
    }

    if (registered == 0) entry->deinit();

    return registered;
}

bool hasLiveInstances(const void *registrationData)
{
    const auto *info = static_cast<const ClapRegistrationInfo *>(registrationData);
    return info && info->liveInstances && info->liveInstances->load() != 0;
}

void releaseRegistrationData(void *registrationData)
{
    auto *info = static_cast<ClapRegistrationInfo *>(registrationData);
    if (!info) return;
    free(info->typeName);
    delete info;
}

std::string registrationTypeName(const void *registrationData)
{
    const auto *info = static_cast<const ClapRegistrationInfo *>(registrationData);
    return info && info->typeName ? info->typeName : "";
}

std::string registrationDisplayName(const void *registrationData)
{
    const auto *info = static_cast<const ClapRegistrationInfo *>(registrationData);
    if (!info || !info->descriptor) return {};
    return info->descriptor->name ? info->descriptor->name : registrationTypeName(info);
}

}
