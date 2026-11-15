/*
 * Optional VST3 -> BareDAW Device adapter. This translation unit is compiled
 * only when BAREDAW_ENABLE_VST3=ON; no SDK header or symbol enters baredaw_core.
 */
#include "Vst3Adapter.h"

#include "bridge/plugin-host/NativePluginEditor.h"
#include "core/AudioBuffer.h"
#include "core/Device.h"
#include "core/MidiBuffer.h"
#include "engine/public/Transport.h"

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/hosting/processdata.h"
#include "public.sdk/source/vst/utility/stringconvert.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace pluginhost::vst3adapter
{
namespace
{
using namespace Steinberg;
using namespace Steinberg::Vst;

struct RegistrationInfo
{
    VST3::Hosting::Module::Ptr module;
    VST3::Hosting::ClassInfo classInfo;
    std::string typeName;
    std::string displayName;
    double sampleRate = 48000.0;
    uint32_t maxBlock = 2048;
    uint8_t acceptsIo = 0;
    uint8_t producesIo = 0;
    bool isInstrument = false;
    std::shared_ptr<std::atomic<uint32_t>> liveInstances =
        std::make_shared<std::atomic<uint32_t>>(0);
};

IPtr<HostApplication> hostApplication()
{
    static IPtr<HostApplication> app = owned(new HostApplication());
    PluginContextFactory::instance().setPluginContext(app);
    return app;
}

class Vst3PluginDevice;

class ComponentHandler final : public IComponentHandler
{
public:
    explicit ComponentHandler(Vst3PluginDevice &owner) : owner_(owner) {}

    tresult PLUGIN_API beginEdit(ParamID id) override;
    tresult PLUGIN_API performEdit(ParamID id, ParamValue value) override;
    tresult PLUGIN_API endEdit(ParamID id) override;
    tresult PLUGIN_API restartComponent(int32 flags) override;

    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override
    {
        if (!obj) return kInvalidArgument;
        if (FUnknownPrivate::iidEqual(iid, IComponentHandler::iid) ||
            FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
            *obj = static_cast<IComponentHandler *>(this);
            return kResultTrue;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1000; }
    uint32 PLUGIN_API release() override { return 1000; }

private:
    Vst3PluginDevice &owner_;
};

class PlugFrame final : public IPlugFrame
{
public:
    explicit PlugFrame(Vst3PluginDevice &owner) : owner_(owner) {}
    tresult PLUGIN_API resizeView(IPlugView *view, ViewRect *newSize) override;
    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override
    {
        if (!obj) return kInvalidArgument;
        if (FUnknownPrivate::iidEqual(iid, IPlugFrame::iid) ||
            FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
            *obj = static_cast<IPlugFrame *>(this);
            return kResultTrue;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1000; }
    uint32 PLUGIN_API release() override { return 1000; }

private:
    Vst3PluginDevice &owner_;
};

class Vst3PluginDevice final : public Device, public NativePluginEditor
{
public:
    explicit Vst3PluginDevice(const RegistrationInfo &info)
        : info_(info), componentHandler_(*this), plugFrame_(*this),
          events_(MidiBuffer::MaxMidiEventsPerBlock + 32)
    {
        auto configurationFailed = [&](const char *stage) {
            debugUi("instance.configure.failed", stage);
        };
        sampleRate = info.sampleRate;
        hostApplication();
        provider_ = owned(new PlugProvider(info.module->getFactory(), info.classInfo, true));
        component_ = owned(provider_->getComponent());
        controller_ = owned(provider_->getController());
        if (!component_) {
            configurationFailed("component");
            return;
        }
        processor_ = U::cast<IAudioProcessor>(component_);
        if (!processor_) {
            configurationFailed("audio-processor");
            return;
        }
        if (processor_->canProcessSampleSize(kSample32) != kResultTrue) {
            configurationFailed("32-bit-sample-support");
            return;
        }

        if (controller_) controller_->setComponentHandler(&componentHandler_);
        const int32 parameterCount = controller_ ? controller_->getParameterCount() : 0;
        inputParameters_.setMaxParameters(std::max<int32>(1, parameterCount));

        const int32 outputBuses = component_->getBusCount(kAudio, kOutput);
        const int32 inputBuses = component_->getBusCount(kAudio, kInput);
        debugUi("instance.io",
                "audioInputs=" + std::to_string(inputBuses) +
                " audioOutputs=" + std::to_string(outputBuses) +
                " eventInputs=" +
                std::to_string(component_->getBusCount(kEvent, kInput)));
        if (outputBuses < 1) {
            configurationFailed("main-output-bus");
            return;
        }

        // Negotiate the plugin's advertised default layouts before activating
        // buses. Some instruments tolerate hosts skipping this; others remain
        // silent or fail setupProcessing until setBusArrangements is called.
        component_->setIoMode(kSimple);
        std::vector<SpeakerArrangement> inputArrangements(inputBuses, SpeakerArr::kEmpty);
        std::vector<SpeakerArrangement> outputArrangements(outputBuses, SpeakerArr::kEmpty);
        auto readArrangement = [&](BusDirection direction, int32 count,
                                   std::vector<SpeakerArrangement> &arrangements) {
            for (int32 bus = 0; bus < count; ++bus) {
                if (processor_->getBusArrangement(direction, bus, arrangements[bus]) == kResultTrue)
                    continue;
                BusInfo busInfo{};
                if (component_->getBusInfo(kAudio, direction, bus, busInfo) != kResultTrue)
                    continue;
                if (busInfo.channelCount == 1) arrangements[bus] = SpeakerArr::kMono;
                if (busInfo.channelCount == 2) arrangements[bus] = SpeakerArr::kStereo;
            }
        };
        readArrangement(kInput, inputBuses, inputArrangements);
        readArrangement(kOutput, outputBuses, outputArrangements);
        const tresult arrangementResult = processor_->setBusArrangements(
            inputArrangements.empty() ? nullptr : inputArrangements.data(), inputBuses,
            outputArrangements.data(), outputBuses);
        debugUi("instance.bus-arrangements",
                "result=" + std::to_string(arrangementResult));

        if (component_->activateBus(kAudio, kOutput, 0, true) != kResultTrue) {
            configurationFailed("activate-main-output");
            return;
        }
        for (int32 bus = 1; bus < outputBuses; ++bus)
            component_->activateBus(kAudio, kOutput, bus, false);

        if (inputBuses > 0) {
            if (info.isInstrument) {
                // BareDAW's instrument slot has no upstream insert signal.
                // Several synths advertise an optional audio input; activating
                // it makes them wait for a carrier instead of behaving as a
                // MIDI instrument. Sidechain/input routing can opt in later.
                component_->activateBus(kAudio, kInput, 0, false);
            } else if (component_->activateBus(kAudio, kInput, 0, true) != kResultTrue) {
                configurationFailed("activate-main-input");
                return;
            }
        }
        for (int32 bus = 1; bus < inputBuses; ++bus)
            component_->activateBus(kAudio, kInput, bus, false);

        const int32 eventInputs = component_->getBusCount(kEvent, kInput);
        if (eventInputs > 0) component_->activateBus(kEvent, kInput, 0, true);

        BusInfo outputInfo{};
        if (component_->getBusInfo(kAudio, kOutput, 0, outputInfo) != kResultTrue ||
            outputInfo.channelCount < 1 || outputInfo.channelCount > 2) {
            configurationFailed("main-output-layout");
            return;
        }
        outputChannels_ = outputInfo.channelCount;
        if (inputBuses > 0) {
            BusInfo inputInfo{};
            if (component_->getBusInfo(kAudio, kInput, 0, inputInfo) != kResultTrue ||
                inputInfo.channelCount < 1 || inputInfo.channelCount > 2) {
                if (!info.isInstrument) {
                    configurationFailed("main-input-layout");
                    return;
                }
            } else {
                inputChannels_ = inputInfo.channelCount;
            }
        }

        outputScratch_.resize(static_cast<size_t>(outputChannels_) * info.maxBlock);
        for (int32 ch = 0; ch < outputChannels_; ++ch)
            outputPtrs_[ch] = outputScratch_.data() + static_cast<size_t>(ch) * info.maxBlock;
        if (info.isInstrument && inputChannels_ > 0)
            inputScratch_.resize(static_cast<size_t>(inputChannels_) * info.maxBlock);

        if (!processData_.prepare(*component_, 0, kSample32)) {
            configurationFailed("process-data");
            return;
        }
        processData_.inputEvents = &events_;
        processData_.inputParameterChanges = &inputParameters_;
        processData_.processContext = &processContext_;
        processContext_.sampleRate = info.sampleRate;

        if (!configureProcessing(info.sampleRate)) {
            configurationFailed("processing");
            return;
        }
        setTypeName(info.typeName);
        valid_ = true;
        ++*info_.liveInstances;
        counted_ = true;
    }

    ~Vst3PluginDevice() override
    {
        debugUi("instance.destructor.begin",
                "processBlocks=" + std::to_string(processCallCount()));
        destroy();
        if (controller_) controller_->setComponentHandler(nullptr);
        if (processor_ && processing_) processor_->setProcessing(false);
        if (component_ && active_) component_->setActive(false);
        processData_.unprepare();
        controller_ = nullptr;
        processor_ = nullptr;
        component_ = nullptr;
        provider_ = nullptr;
        if (counted_) --*info_.liveInstances;
        debugUi("instance.destructor.end");
    }

    bool isValid() const { return valid_; }
    void debugUi(const char *action, const std::string &detail = {}) const
    {
        std::fprintf(stderr, "[plugin-debug vst3] %s  plugin=\"%s\"%s%s\n",
                     action, info_.displayName.c_str(),
                     detail.empty() ? "" : "  ", detail.c_str());
        std::fflush(stderr);
    }
    DeviceIO accepts() const override { return static_cast<DeviceIO>(info_.acceptsIo); }
    DeviceIO produces() const override { return static_cast<DeviceIO>(info_.producesIo); }
    void prepare(double sr) override { setSampleRate(sr); }
    // BareDAW accepts live MIDI while transport is stopped. VST instruments
    // therefore need continuous process() calls: otherwise note events remain
    // queued indefinitely and a later transport start delivers a stale burst.
    bool hasTail() const override { return info_.isInstrument; }
    bool requiresContinuousProcessing() const override { return true; }
    uint64_t processCallCount() const override
    {
        return processCalls_.load(std::memory_order_relaxed);
    }

    void setBlockContext(const Transport *transport, int) override
    {
        transportPlaying_ = transport && transport->isPlaying();
        transportLooping_ = transport && transport->isLooping();
        if (!transport) return;
        transportTempo_ = transport->bpm;
        timeSigNumerator_ = transport->numerator;
        timeSigDenominator_ = transport->denominator;
        cycleStartMusic_ = transport->loopStartBeat;
        cycleEndMusic_ = transport->loopEndBeat;
    }

    void processMidi(const MidiBuffer &midi, int64_t blockStart) override
    {
        for (int i = 0; i < midi.count; ++i) {
            const RawMidiEvent &raw = midi.events[i];
            const uint8_t status = raw.status & 0xF0;
            if (status != 0x80 && status != 0x90) continue;
            if (raw.d1 >= 128) continue;
            Event event{};
            event.busIndex = 0;
            event.sampleOffset = static_cast<int32>(std::clamp<int64_t>(
                static_cast<int64_t>(raw.sampleTime) - blockStart,
                0, static_cast<int64_t>(info_.maxBlock - 1)));
            event.flags = Event::kIsLive;
            const int channel = raw.status & 0x0F;
            const bool noteOn = status == 0x90 && raw.d2 != 0;
            event.type = noteOn ? Event::kNoteOnEvent : Event::kNoteOffEvent;
            if (noteOn) {
                event.noteOn.channel = channel;
                event.noteOn.pitch = raw.d1;
                event.noteOn.velocity = raw.d2 / 127.0f;
                event.noteOn.tuning = 0.0f;
                event.noteOn.length = 0;
                event.noteOn.noteId = -1;
                activeNotes_[channel][raw.d1] = true;
            } else {
                event.noteOff.channel = channel;
                event.noteOff.pitch = raw.d1;
                event.noteOff.velocity = raw.d2 / 127.0f;
                event.noteOff.tuning = 0.0f;
                event.noteOff.noteId = -1;
                activeNotes_[channel][raw.d1] = false;
            }
            events_.addEvent(event);
        }
    }

    void process(AudioBuffer &buffer, int frames, int writeOffset,
                 int64_t globalSamplePosition) override
    {
        if (!valid_ || !processing_ || frames <= 0 ||
            frames > static_cast<int>(info_.maxBlock)) return;
        const int availableFrames = std::min(frames, buffer.numFrames() - writeOffset);
        if (availableFrames <= 0 || outputChannels_ > buffer.numChannels()) return;

        std::array<Sample32 *, 2> inputPtrs{};
        if (inputChannels_ > 0) {
            if (info_.isInstrument) {
                for (int32 ch = 0; ch < inputChannels_; ++ch) {
                    inputPtrs[ch] = inputScratch_.data() +
                                    static_cast<size_t>(ch) * info_.maxBlock;
                    std::fill_n(inputPtrs[ch], availableFrames, 0.0f);
                }
            } else {
                if (inputChannels_ > buffer.numChannels()) return;
                for (int32 ch = 0; ch < inputChannels_; ++ch)
                    inputPtrs[ch] = buffer.channel(ch) + writeOffset;
            }
            processData_.setChannelBuffers(kInput, 0, inputPtrs.data(), inputChannels_);
        }
        for (int32 ch = 0; ch < outputChannels_; ++ch)
            std::fill_n(outputPtrs_[ch], availableFrames, 0.0f);
        processData_.setChannelBuffers(kOutput, 0, outputPtrs_.data(), outputChannels_);
        processData_.numSamples = availableFrames;
        processData_.processMode = kRealtime;
        processData_.symbolicSampleSize = kSample32;
        processContext_.continousTimeSamples = globalSamplePosition;
        processContext_.projectTimeSamples = globalSamplePosition;
        processContext_.state = ProcessContext::kContTimeValid |
                                ProcessContext::kProjectTimeMusicValid |
                                ProcessContext::kTempoValid |
                                ProcessContext::kTimeSigValid;
        if (transportPlaying_) processContext_.state |= ProcessContext::kPlaying;
        if (transportLooping_) {
            processContext_.state |= ProcessContext::kCycleActive |
                                     ProcessContext::kCycleValid;
        }
        processContext_.tempo = transportTempo_;
        processContext_.projectTimeMusic =
            globalSamplePosition * processContext_.tempo / (60.0 * sampleRate);
        processContext_.timeSigNumerator = timeSigNumerator_;
        processContext_.timeSigDenominator = timeSigDenominator_;
        processContext_.cycleStartMusic = cycleStartMusic_;
        processContext_.cycleEndMusic = cycleEndMusic_;

        // Native editors call IComponentHandler on the UI thread. Move those
        // edits through a fixed SPSC ring so the audio thread never races the
        // SDK's non-thread-safe ParameterChanges container or takes a lock.
        drainEditorParameters();

        processCalls_.fetch_add(1, std::memory_order_relaxed);
        if (processor_->process(processData_) == kResultOk) {
            for (int32 ch = 0; ch < outputChannels_; ++ch)
                std::copy_n(outputPtrs_[ch], availableFrames,
                            buffer.channel(ch) + writeOffset);
            // BareDAW's main chain is stereo today. A mono VST3 output should
            // occupy the stereo chain rather than leave the old right channel
            // behind (which is especially surprising for mono effects).
            if (outputChannels_ == 1 && buffer.numChannels() > 1)
                std::copy_n(outputPtrs_[0], availableFrames,
                            buffer.channel(1) + writeOffset);
        }
        events_.clear();
        inputParameters_.clearQueue();
    }

    void allNotesOff() override
    {
        for (int channel = 0; channel < 16; ++channel) {
            for (int pitch = 0; pitch < 128; ++pitch) {
                if (!activeNotes_[channel][pitch]) continue;
                Event event{};
                event.busIndex = 0;
                event.type = Event::kNoteOffEvent;
                event.noteOff.channel = channel;
                event.noteOff.pitch = pitch;
                event.noteOff.velocity = 0.0f;
                event.noteOff.noteId = -1;
                events_.addEvent(event);
                activeNotes_[channel][pitch] = false;
            }
        }
    }

    void setSampleRate(double sr) override
    {
        if (sr <= 0.0 || std::fabs(sr - sampleRate) < 0.001) return;
        if (processor_ && processing_) processor_->setProcessing(false);
        processing_ = false;
        if (component_ && active_) component_->setActive(false);
        active_ = false;
        valid_ = configureProcessing(sr);
        sampleRate = sr;
    }

    std::vector<InstrumentParameterDescriptor> getParameterDescriptors() const override
    {
        if (!controller_) return {};
        std::vector<InstrumentParameterDescriptor> result;
        const int32 count = controller_->getParameterCount();
        result.reserve(static_cast<size_t>(count));
        for (int32 index = 0; index < count; ++index) {
            ParameterInfo info{};
            if (controller_->getParameterInfo(index, info) != kResultTrue) continue;
            if ((info.flags & ParameterInfo::kIsHidden) != 0 ||
                (info.flags & ParameterInfo::kIsReadOnly) != 0) continue;
            InstrumentParameterDescriptor out;
            out.id = std::to_string(info.id);
            out.name = StringConvert::convert(info.title, 128);
            out.unit = StringConvert::convert(info.units, 128);
            out.minValue = 0.0f;
            out.maxValue = 1.0f;
            out.step = info.stepCount > 0 ? 1.0f / info.stepCount : 0.0f;
            out.defaultValue = static_cast<float>(info.defaultNormalizedValue);
            out.currentValue = static_cast<float>(controller_->getParamNormalized(info.id));
            if (info.stepCount > 0 && info.stepCount < 32) {
                for (int32 step = 0; step <= info.stepCount; ++step) {
                    String128 text{};
                    const ParamValue value = static_cast<double>(step) / info.stepCount;
                    if (controller_->getParamStringByValue(info.id, value, text) != kResultTrue)
                        break;
                    out.enumValues.push_back(StringConvert::convert(text, 128));
                }
                if (!out.enumValues.empty()) out.type = "enum";
            }
            if ((info.flags & ParameterInfo::kIsBypass) != 0) out.style = "button";
            result.push_back(std::move(out));
        }
        return result;
    }

    bool setParameterValue(const std::string &id, float value) override
    {
        if (!controller_) return false;
        char *end = nullptr;
        const unsigned long parsed = std::strtoul(id.c_str(), &end, 10);
        if (end == id.c_str() || *end != '\0') return false;
        queueParameter(static_cast<ParamID>(parsed), std::clamp<double>(value, 0.0, 1.0));
        return true;
    }

    bool getParameterValue(const std::string &id, float &value) const override
    {
        if (!controller_) return false;
        char *end = nullptr;
        const unsigned long parsed = std::strtoul(id.c_str(), &end, 10);
        if (end == id.c_str() || *end != '\0') return false;
        value = static_cast<float>(controller_->getParamNormalized(
            static_cast<ParamID>(parsed)));
        return true;
    }

    void queueParameter(ParamID id, ParamValue value)
    {
        if (controller_) controller_->setParamNormalized(id, value);
        enqueueProcessParameter(id, value);
    }

    void queueParameterFromEditor(ParamID id, ParamValue value)
    {
        const uint32_t write = editorParamWrite_.load(std::memory_order_relaxed);
        const uint32_t next = (write + 1) % editorParamRing_.size();
        if (next == editorParamRead_.load(std::memory_order_acquire)) return;
        editorParamRing_[write] = {id, value};
        editorParamWrite_.store(next, std::memory_order_release);
    }

    void enqueueProcessParameter(ParamID id, ParamValue value)
    {
        int32 queueIndex = 0;
        if (IParamValueQueue *queue = inputParameters_.addParameterData(id, queueIndex)) {
            int32 pointIndex = 0;
            queue->addPoint(0, value, pointIndex);
        }
    }

    void drainEditorParameters()
    {
        uint32_t read = editorParamRead_.load(std::memory_order_relaxed);
        const uint32_t write = editorParamWrite_.load(std::memory_order_acquire);
        while (read != write) {
            const auto change = editorParamRing_[read];
            enqueueProcessParameter(change.id, change.value);
            read = (read + 1) % editorParamRing_.size();
        }
        editorParamRead_.store(read, std::memory_order_release);
    }

    void *queryExtension(const char *extensionId) override
    {
        return extensionId && std::strcmp(extensionId, kNativePluginEditorExtension) == 0
                   ? static_cast<NativePluginEditor *>(this)
                   : nullptr;
    }

    bool supports(const char *platformType) const override
    {
#if SMTG_OS_MACOS
        return controller_ && platformType &&
               std::strcmp(platformType, kNativeEditorCocoa) == 0;
#elif SMTG_OS_WINDOWS
        return controller_ && platformType &&
               std::strcmp(platformType, kNativeEditorWin32) == 0;
#else
        (void)platformType;
        return false;
#endif
    }

    bool create(const char *platformType, void *parent,
                uint32_t &width, uint32_t &height) override
    {
        debugUi("editor.create.begin", platformType ? platformType : "<null>");
        if (!parent || !supports(platformType) || !controller_) return false;
        debugUi("editor.createView.begin");
        if (!view_) view_ = owned(controller_->createView(ViewType::kEditor));
        debugUi("editor.createView.end", view_ ? "success" : "failed");
        if (!view_) return false;
        auto discardView = [&] {
            if (viewAttached_) {
                debugUi("editor.removed.begin", "create cleanup");
                view_->removed();
                debugUi("editor.removed.end", "create cleanup");
            }
            view_->setFrame(nullptr);
            viewAttached_ = false;
            view_ = nullptr;
        };
#if SMTG_OS_MACOS
        const char *vstPlatform = kPlatformTypeNSView;
#elif SMTG_OS_WINDOWS
        const char *vstPlatform = kPlatformTypeHWND;
#else
        return false;
#endif
        debugUi("editor.platform-check.begin", vstPlatform);
        if (view_->isPlatformTypeSupported(vstPlatform) != kResultTrue) {
            debugUi("editor.platform-check.end", "unsupported");
            // createView() is allowed to allocate native framework resources.
            // Release a rejected view immediately instead of leaving its event
            // sources alive until the audio device is eventually destroyed.
            discardView();
            return false;
        }
        debugUi("editor.platform-check.end", "supported");

        // Most VST3 editors report their size before attached(), but a few
        // (notably Massive X) only finish constructing their UI after they have
        // an actual parent NSView. Keep getSize() as the normal path and use a
        // bounded provisional host size for those attach-first editors.
        ViewRect rect{};
        debugUi("editor.getSize.begin", "before attach");
        const bool hadInitialSize =
            view_->getSize(&rect) == kResultTrue &&
            rect.right > rect.left && rect.bottom > rect.top;
        if (hadInitialSize) {
            editorWidth_ = rect.right - rect.left;
            editorHeight_ = rect.bottom - rect.top;
            debugUi("editor.getSize.end",
                    "before attach " + std::to_string(editorWidth_) + "x" +
                    std::to_string(editorHeight_));
        } else {
            editorWidth_ = static_cast<int32>(width > 0 ? width : 900);
            editorHeight_ = static_cast<int32>(height > 0 ? height : 600);
            debugUi("editor.getSize.end",
                    "before attach unavailable; provisional " +
                    std::to_string(editorWidth_) + "x" +
                    std::to_string(editorHeight_));
        }
        view_->setFrame(&plugFrame_);
        debugUi("editor.attached.begin");
        if (!viewAttached_ && view_->attached(parent, vstPlatform) != kResultTrue) {
            debugUi("editor.attached.end", "failed");
            discardView();
            return false;
        }
        debugUi("editor.attached.end", "success");
        viewAttached_ = true;

        ViewRect attachedRect{};
        debugUi("editor.getSize.begin", "after attach");
        if (view_->getSize(&attachedRect) == kResultTrue &&
            attachedRect.right > attachedRect.left &&
            attachedRect.bottom > attachedRect.top) {
            editorWidth_ = attachedRect.right - attachedRect.left;
            editorHeight_ = attachedRect.bottom - attachedRect.top;
            debugUi("editor.getSize.end",
                    "after attach " + std::to_string(editorWidth_) + "x" +
                    std::to_string(editorHeight_));
        } else {
            debugUi("editor.getSize.end",
                    "after attach unavailable; keeping " +
                    std::to_string(editorWidth_) + "x" +
                    std::to_string(editorHeight_));
        }
        width = static_cast<uint32_t>(editorWidth_);
        height = static_cast<uint32_t>(editorHeight_);
        debugUi("editor.create.end", std::to_string(width) + "x" +
                                     std::to_string(height));
        return true;
    }

    bool setVisible(bool) override { return viewAttached_; }

    void setResizeCallback(void *context, ResizeCallback callback) override
    {
        resizeContext_ = context;
        resizeCallback_ = callback;
    }

    void destroy() override
    {
        if (!view_) return;
        debugUi("editor.destroy.begin");
        if (viewAttached_) {
            debugUi("editor.removed.begin");
            view_->removed();
            debugUi("editor.removed.end");
        }
        view_->setFrame(nullptr);
        viewAttached_ = false;
        view_ = nullptr;
        debugUi("editor.destroy.end");
    }

    tresult resizeEditor(IPlugView *view, ViewRect *newSize)
    {
        debugUi("editor.resize.request", newSize
            ? std::to_string(newSize->right - newSize->left) + "x" +
              std::to_string(newSize->bottom - newSize->top)
            : "<null>");
        if (!view_ || view != view_.get() || !newSize) return kInvalidArgument;
        if (view_->checkSizeConstraint(newSize) != kResultTrue) {
            debugUi("editor.resize.constraint", "rejected");
            return kResultFalse;
        }

        // Some editors call IPlugFrame::resizeView() again from onSize().
        // Calling straight back into onSize() in that case creates an A -> B
        // -> A recursion which presents as a violently oscillating window.
        // Keep only the newest nested request and finish the current handshake
        // before acknowledging it.
        if (resizeInProgress_) {
            pendingResize_ = *newSize;
            hasPendingResize_ = true;
            debugUi("editor.resize.coalesced",
                std::to_string(newSize->right - newSize->left) + "x" +
                std::to_string(newSize->bottom - newSize->top));
            return kResultTrue;
        }

        ViewRect requested = *newSize;
        tresult result = kResultTrue;
        resizeInProgress_ = true;
        constexpr int kMaxResizePasses = 4;
        int pass = 0;
        for (; pass < kMaxResizePasses; ++pass) {
            hasPendingResize_ = false;
            const int32 requestedWidth =
                std::max<int32>(1, requested.right - requested.left);
            const int32 requestedHeight =
                std::max<int32>(1, requested.bottom - requested.top);

            if (requestedWidth == editorWidth_ &&
                requestedHeight == editorHeight_) {
                debugUi("editor.resize.unchanged",
                    std::to_string(requestedWidth) + "x" +
                    std::to_string(requestedHeight));
            } else {
                debugUi("editor.onSize.begin");
                result = view_->onSize(&requested);
                debugUi("editor.onSize.end", "result=" + std::to_string(result));
                if (result != kResultTrue) break;

                editorWidth_ = requestedWidth;
                editorHeight_ = requestedHeight;
                // Cocoa window changes are scheduled onto the next main-loop
                // turn by the app. Notify it only after the plugin accepted the
                // requested size, never while onSize() is on the stack.
                if (resizeCallback_)
                    resizeCallback_(resizeContext_,
                                    static_cast<uint32_t>(editorWidth_),
                                    static_cast<uint32_t>(editorHeight_));
            }

            if (!hasPendingResize_) break;
            requested = pendingResize_;
        }
        resizeInProgress_ = false;
        if (hasPendingResize_) {
            debugUi("editor.resize.loop-suppressed",
                "more than " + std::to_string(kMaxResizePasses) + " passes");
            hasPendingResize_ = false;
        }
        return result;
    }

private:
    bool configureProcessing(double sr)
    {
        if (!processor_ || !component_) {
            debugUi("processing.configure.failed", "missing-interface");
            return false;
        }
        ProcessSetup setup{};
        setup.processMode = kRealtime;
        setup.symbolicSampleSize = kSample32;
        setup.maxSamplesPerBlock = static_cast<int32>(info_.maxBlock);
        setup.sampleRate = sr;
        if (processor_->setupProcessing(setup) != kResultTrue) {
            debugUi("processing.configure.failed", "setupProcessing");
            return false;
        }
        if (component_->setActive(true) != kResultTrue) {
            debugUi("processing.configure.failed", "setActive");
            return false;
        }
        active_ = true;
        const tresult processingResult = processor_->setProcessing(true);
        // Older VST3s (including AIR Mini Grand 1.3.1) inherit or implement
        // setProcessing() as kNotImplemented even though process() itself is
        // valid. Steinberg's AudioHost sample and BaseWrapper both continue in
        // this case. Preserve hard failures while accepting that legacy result.
        if (processingResult != kResultTrue &&
            processingResult != kNotImplemented) {
            debugUi("processing.configure.failed",
                    "setProcessing result=" +
                    std::to_string(processingResult));
            component_->setActive(false);
            active_ = false;
            return false;
        }
        if (processingResult == kNotImplemented)
            debugUi("processing.configure.compatibility",
                    "setProcessing not implemented; continuing");
        processing_ = true;
        processContext_.sampleRate = sr;
        return true;
    }

    RegistrationInfo info_;
    IPtr<PlugProvider> provider_;
    IPtr<IComponent> component_;
    IPtr<IAudioProcessor> processor_;
    IPtr<IEditController> controller_;
    ComponentHandler componentHandler_;
    PlugFrame plugFrame_;
    HostProcessData processData_;
    ProcessContext processContext_{};
    EventList events_;
    ParameterChanges inputParameters_;
    std::vector<float> outputScratch_;
    std::vector<float> inputScratch_;
    std::array<Sample32 *, 2> outputPtrs_{};
    std::array<std::array<bool, 128>, 16> activeNotes_{};
    struct PendingEditorParameter { ParamID id = 0; ParamValue value = 0.0; };
    std::array<PendingEditorParameter, 1024> editorParamRing_{};
    std::atomic<uint32_t> editorParamWrite_{0};
    std::atomic<uint32_t> editorParamRead_{0};
    std::atomic<uint64_t> processCalls_{0};
    IPtr<IPlugView> view_;
    int32 inputChannels_ = 0;
    int32 outputChannels_ = 0;
    int32 editorWidth_ = 0;
    int32 editorHeight_ = 0;
    bool active_ = false;
    bool processing_ = false;
    bool valid_ = false;
    bool counted_ = false;
    bool viewAttached_ = false;
    bool resizeInProgress_ = false;
    bool hasPendingResize_ = false;
    ViewRect pendingResize_{};
    bool transportPlaying_ = false;
    bool transportLooping_ = false;
    void *resizeContext_ = nullptr;
    ResizeCallback resizeCallback_ = nullptr;
    double transportTempo_ = 120.0;
    int32 timeSigNumerator_ = 4;
    int32 timeSigDenominator_ = 4;
    double cycleStartMusic_ = 0.0;
    double cycleEndMusic_ = 0.0;
};

tresult PLUGIN_API ComponentHandler::beginEdit(ParamID id)
{
    owner_.debugUi("parameter.beginEdit", "id=" + std::to_string(id));
    return kResultTrue;
}

tresult PLUGIN_API ComponentHandler::performEdit(ParamID id, ParamValue value)
{
    owner_.debugUi("parameter.performEdit", "id=" + std::to_string(id) +
                   " value=" + std::to_string(value));
    owner_.queueParameterFromEditor(id, std::clamp<ParamValue>(value, 0.0, 1.0));
    return kResultTrue;
}

tresult PLUGIN_API ComponentHandler::endEdit(ParamID id)
{
    owner_.debugUi("parameter.endEdit", "id=" + std::to_string(id));
    return kResultTrue;
}

tresult PLUGIN_API ComponentHandler::restartComponent(int32 flags)
{
    owner_.debugUi("component.restart", "flags=" + std::to_string(flags));
    return kResultTrue;
}

tresult PLUGIN_API PlugFrame::resizeView(IPlugView *view, ViewRect *newSize)
{
    return owner_.resizeEditor(view, newSize);
}

void *vst3Factory(void *hostCtx, double sampleRate, uint32_t maxBlock)
{
    auto *info = static_cast<RegistrationInfo *>(hostCtx);
    std::fprintf(stderr, "[plugin-debug vst3] instance.create.begin  plugin=\"%s\" sr=%.0f block=%u\n",
                 info->displayName.c_str(), sampleRate, maxBlock);
    std::fflush(stderr);
    RegistrationInfo live = *info;
    live.sampleRate = sampleRate;
    live.maxBlock = maxBlock;
    auto *device = new Vst3PluginDevice(live);
    if (!device->isValid()) {
        std::fprintf(stderr, "[plugin-debug vst3] instance.create.end  plugin=\"%s\" failed\n",
                     info->displayName.c_str());
        std::fflush(stderr);
        delete device;
        return nullptr;
    }
    std::fprintf(stderr, "[plugin-debug vst3] instance.create.end  plugin=\"%s\" success ptr=%p\n",
                 info->displayName.c_str(), static_cast<void *>(device));
    std::fflush(stderr);
    return device;
}

void vst3Destroy(void *, void *instance)
{
    auto *device = static_cast<Vst3PluginDevice *>(instance);
    if (device) device->debugUi("instance.destroy.begin");
    delete device;
    std::fprintf(stderr, "[plugin-debug vst3] instance.destroy.end ptr=%p\n", instance);
    std::fflush(stderr);
}

}

size_t registerFromBundle(const char *bundlePath,
                          double sampleRate,
                          uint32_t maxBlock,
                          const BareDAW_HostApi &hostApi,
                          std::vector<void *> &ownedRegistrationData)
{
    if (!bundlePath || !hostApi.register_device) return 0;
    hostApplication();
    std::string error;
    auto module = VST3::Hosting::Module::create(bundlePath, error);
    if (!module) {
        if (hostApi.log)
            hostApi.log(hostApi.host_ctx, 1,
                        (std::string("VST3 module load failed: ") + error).c_str());
        return 0;
    }

    size_t registered = 0;
    const auto factory = module->getFactory();
    for (const auto &classInfo : factory.classInfos()) {
        if (classInfo.category() != kVstAudioEffectClass) continue;

        const auto &subCategories = classInfo.subCategories();
        const bool declaredInstrument =
            std::find(subCategories.begin(), subCategories.end(),
                      std::string(PlugType::kInstrument)) != subCategories.end();

        IPtr<PlugProvider> probe = owned(new PlugProvider(factory, classInfo, true));
        IPtr<IComponent> component = owned(probe->getComponent());
        if (!component) continue;

        uint8_t accepts = 0;
        uint8_t produces = 0;
        const bool hasAudioInput = component->getBusCount(kAudio, kInput) > 0;
        const bool hasEventInput = component->getBusCount(kEvent, kInput) > 0;
        const bool isInstrument = declaredInstrument || (hasEventInput && !hasAudioInput);
        // Optional audio inputs on synths/vocoders do not turn them into insert
        // effects. DeviceIO describes their place in BareDAW's chain; the
        // adapter still supplies their actual VST3 audio input buses.
        if (hasAudioInput && !isInstrument) accepts |= 2;
        if (hasEventInput) accepts |= 1;
        if (component->getBusCount(kAudio, kOutput) > 0) produces |= 2;
        if ((produces & 2) == 0) continue; // MIDI-only output is not routed yet.

        auto *info = new RegistrationInfo;
        info->module = module;
        info->classInfo = classInfo;
        info->typeName = "vst3:" + classInfo.ID().toString();
        info->displayName = classInfo.name();
        info->sampleRate = sampleRate;
        info->maxBlock = maxBlock;
        info->acceptsIo = accepts;
        info->producesIo = produces;
        info->isInstrument = isInstrument;

        if (hostApi.register_device(info, info->typeName.c_str(), vst3Factory,
                                    vst3Destroy, accepts, produces) != 0) {
            delete info;
            continue;
        }
        ownedRegistrationData.push_back(info);
        ++registered;
    }
    return registered;
}

bool hasLiveInstances(const void *registrationData)
{
    const auto *info = static_cast<const RegistrationInfo *>(registrationData);
    return info && info->liveInstances->load(std::memory_order_acquire) != 0;
}

void releaseRegistrationData(void *registrationData)
{
    delete static_cast<RegistrationInfo *>(registrationData);
}

std::string registrationTypeName(const void *registrationData)
{
    const auto *info = static_cast<const RegistrationInfo *>(registrationData);
    return info ? info->typeName : std::string{};
}

std::string registrationDisplayName(const void *registrationData)
{
    const auto *info = static_cast<const RegistrationInfo *>(registrationData);
    return info ? info->displayName : std::string{};
}

}
