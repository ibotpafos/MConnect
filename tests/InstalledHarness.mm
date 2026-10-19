// Software-only validation of the installed plugin. No audio device or DAW.
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_extra/juce_gui_extra.h>
#import <Cocoa/Cocoa.h>
#import <Vision/Vision.h>
#include <AudioToolbox/AudioToolbox.h>
#include <mach-o/dyld.h>
#include <mach/task_policy.h>
#include <chrono>
#include <iostream>
#include <thread>

class InstalledToneSource final : public juce::Thread
{
public:
    InstalledToneSource(juce::AudioProcessor& p, double sr, int frames, float level)
        : Thread("Installed plugin test signal"), processor(p), rate(sr), block(frames), gain(level) {}
    void run() override
    {
        juce::AudioBuffer<float> audio(2, block);
        juce::MidiBuffer midi;
        uint64_t frame = 0;
        auto due = std::chrono::steady_clock::now();
        while (!threadShouldExit())
        {
            if (!paused.load())
            {
                const auto level = gain.load();
                for (int i = 0; i < block; ++i)
                {
                    const auto t = static_cast<double>(frame + static_cast<uint64_t>(i)) / rate;
                    audio.setSample(0, i, static_cast<float>(level * std::sin(2 * juce::MathConstants<double>::pi * 997 * t)));
                    audio.setSample(1, i, static_cast<float>(level * std::sin(2 * juce::MathConstants<double>::pi * 1499 * t)));
                }
                processor.processBlock(audio, midi);
                ++blocks;
            }
            frame += static_cast<uint64_t>(block);
            due += std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(block / rate));
            std::this_thread::sleep_until(due);
        }
    }
    std::atomic<bool> paused {false};
    std::atomic<float> gain;
    std::atomic<uint64_t> blocks {0};
private:
    juce::AudioProcessor& processor;
    double rate;
    int block;
};

class InstalledHarness final : public juce::JUCEApplication, private juce::Timer
{
public:
    const juce::String getApplicationName() override { return "MConnect installed test host"; }
    const juce::String getApplicationVersion() override { return "1.0"; }
    bool moreThanOneInstanceAllowed() override { return true; }
    void initialise(const juce::String& commandLine) override
    {
        task_category_policy_data_t category {TASK_FOREGROUND_APPLICATION};
        task_policy_set(mach_task_self(), TASK_CATEGORY_POLICY,
            reinterpret_cast<task_policy_t>(&category), TASK_CATEGORY_POLICY_COUNT);
        activity = [[[NSProcessInfo processInfo] beginActivityWithOptions:
            (NSActivityUserInitiatedAllowingIdleSystemSleep | NSActivityLatencyCritical)
            reason:@"Installed MConnect audio validation"] retain];
        const auto args = juce::StringArray::fromTokens(commandLine, true);
        int seconds = 900;
        hideAfterPairing = args.contains("--no-editor");
        for (int i = 0; i + 1 < args.size(); ++i)
        {
            const auto value = args[i + 1].unquoted();
            if (args[i] == "--plugin") plugin = juce::File(value);
            if (args[i] == "--info") info = juce::File(value);
            if (args[i] == "--control") control = juce::File(value);
            if (args[i] == "--rate") rate = value.getDoubleValue();
            if (args[i] == "--block") block = value.getIntValue();
            if (args[i] == "--seconds") seconds = value.getIntValue();
            if (args[i] == "--gain") gain = value.getFloatValue();
        }
        if (!plugin.isDirectory() || info == juce::File() || rate < 32000 || rate > 192000
            || block < 1 || block > 8192 || gain < 0 || gain > 0.25f)
            return fail("Expected --plugin bundle --info file and valid signal settings");
        if (args.contains("--register-au") && !registerBuiltAudioUnit())
            return fail("Cannot register the requested AU factory for this test process");
        const auto identifier = auTestIdentifier.isEmpty() ? plugin.getFullPathName() : auTestIdentifier;
        juce::addDefaultFormatsToManager(formats);
        juce::OwnedArray<juce::PluginDescription> descriptions;
        for (auto* format : formats.getFormats())
            if (format->fileMightContainThisPluginType(identifier))
                format->findAllTypesForFile(descriptions, identifier);
        if (descriptions.size() != 1) return fail("Expected exactly one plugin in the installed bundle");
        description = *descriptions[0];
        // Keep the requested file, or the explicit process-local AU factory
        // identifier chosen above; never silently select an installed build.
        description.fileOrIdentifier = identifier;
        juce::String error;
        processor = formats.createPluginInstance(description, rate, block, error);
        if (!processor) return fail("Cannot load installed plugin: " + error);
        for (uint32_t i = 0; i < _dyld_image_count(); ++i)
        {
            const juce::String name(_dyld_get_image_name(i));
            if (name.startsWith(plugin.getFullPathName() + "/")) loadedBinary = name;
        }
        if (loadedBinary.isEmpty()) return fail("Requested installed bundle was not loaded into this host");
        processor->setPlayConfigDetails(2, 2, rate, block);
        startSource();
        auto* editor = processor->createEditorAndMakeActive();
        if (editor == nullptr) return fail("Installed plugin has no editor");
        window = std::make_unique<Window>(editor);
        deadline = juce::Time::getMillisecondCounterHiRes() + seconds * 1000.0;
        pairingDeadline = juce::Time::getMillisecondCounterHiRes() + 20000.0;
        startTimer(250);
    }
    void shutdown() override
    {
        stopTimer();
        if (tone) tone->stopThread(2000);
        window.reset();
        tone.reset();
        if (processor) processor->releaseResources();
        processor.reset();
        if (testAuBundle) { CFRelease(testAuBundle); testAuBundle = nullptr; }
        if (activity)
        {
            [[NSProcessInfo processInfo] endActivity:activity];
            [activity release];
            activity = nil;
        }
    }
    void systemRequestedQuit() override { quit(); }
    void anotherInstanceStarted(const juce::String&) override {}
private:
    bool registerBuiltAudioUnit()
    {
        if (!plugin.hasFileExtension("component")) return false;
        const auto path = plugin.getFullPathName();
        auto url = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault,
            reinterpret_cast<const UInt8*>(path.toRawUTF8()), static_cast<CFIndex>(path.getNumBytesAsUTF8()), true);
        if (!url) return false;
        testAuBundle = CFBundleCreate(kCFAllocatorDefault, url);
        CFRelease(url);
        if (!testAuBundle || !CFBundleLoadExecutable(testAuBundle)) return false;
        auto* components = (NSArray*)CFBundleGetValueForInfoDictionaryKey(testAuBundle, CFSTR("AudioComponents"));
        if (![components isKindOfClass:[NSArray class]] || components.count != 1) return false;
        auto* metadata = (NSDictionary*)components.firstObject;
        auto* name = (NSString*)metadata[@"factoryFunction"];
        auto* type = (NSString*)metadata[@"type"];
        if (![name isKindOfClass:[NSString class]] || ![type isKindOfClass:[NSString class]] || type.length != 4) return false;
        auto factory = reinterpret_cast<AudioComponentFactoryFunction>(
            CFBundleGetFunctionPointerForName(testAuBundle, (CFStringRef)name));
        if (!factory) return false;
        // JUCE's ordinary AU loader resolves a file via the system registry and
        // may select the installed version. Private test IDs select this exact
        // build's factory; AudioComponentRegister affects this process only.
        AudioComponentDescription component {};
        component.componentType = juce::ByteOrder::bigEndianInt(type.UTF8String);
        component.componentSubType = 0x4d635473; // McTs
        component.componentManufacturer = 0x54657374; // Test
        const auto version = [metadata[@"version"] unsignedIntValue];
        if (!AudioComponentRegister(&component, CFSTR("MConnect build validation"), version, factory)) return false;
        auTestIdentifier = "AudioUnit:" + juce::String(type.UTF8String) + ",McTs,Test";
        return true;
    }

    class Window final : public juce::DocumentWindow
    {
    public:
        explicit Window(juce::AudioProcessorEditor* editor)
            : DocumentWindow("MConnect installed plugin | test signal", juce::Colour(0xff101016), allButtons)
        {
            setUsingNativeTitleBar(true);
            setContentOwned(editor, true);
            centreWithSize(getWidth(), getHeight());
            setVisible(true);
        }
        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
    };
    void fail(const juce::String& message)
    {
        std::cerr << "FAIL: " << message << '\n';
        setApplicationReturnValue(1);
        quit();
    }
    void startSource()
    {
        if (tone)
        {
            tone->stopThread(2000);
            processor->releaseResources();
        }
        processor->setRateAndBufferSizeDetails(rate, block);
        processor->prepareToPlay(rate, block);
        tone = std::make_unique<InstalledToneSource>(*processor, rate, block, gain);
        const auto options = juce::Thread::RealtimeOptions{}.withPeriodMs(1000.0 * block / rate)
            .withProcessingTimeMs(0.05).withMaximumProcessingTimeMs(0.5);
        if (!tone->startRealtimeThread(options)) tone->startThread(juce::Thread::Priority::high);
    }
    juce::String readQr()
    {
        @autoreleasepool
        {
            auto* view = static_cast<NSView*>(window->getWindowHandle());
            if (view == nil) return {};
            auto* bitmap = [view bitmapImageRepForCachingDisplayInRect:view.bounds];
            [view cacheDisplayInRect:view.bounds toBitmapImageRep:bitmap];
            auto* data = [bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
            auto* request = [[[VNDetectBarcodesRequest alloc] init] autorelease];
            request.symbologies = @[VNBarcodeSymbologyQR];
            auto* handler = [[[VNImageRequestHandler alloc] initWithData:data options:@{}] autorelease];
            NSError* error = nil;
            if (![handler performRequests:@[request] error:&error]) return {};
            for (VNBarcodeObservation* result in request.results)
            {
                const juce::String value(result.payloadStringValue.UTF8String);
                if (!value.startsWith("http://") || !value.containsChar('#')) continue;
                snapshot = info.withFileExtension("png");
                snapshot.replaceWithData(data.bytes, data.length);
                return value;
            }
        }
        return {};
    }
    void timerCallback() override
    {
        if (control.existsAsFile())
        {
            const auto command = juce::JSON::parse(control.loadFileAsString());
            control.deleteFile();
            if (auto* object = command.getDynamicObject())
            {
                if (static_cast<bool>(object->getProperty("quit"))) { quit(); return; }
                if (object->hasProperty("stream"))
                    for (auto* parameter : processor->getParameters())
                        if (parameter->getName(64) == "Stream")
                            parameter->setValueNotifyingHost(static_cast<bool>(object->getProperty("stream")) ? 1.0f : 0.0f);
                if (object->hasProperty("gain")) gain = juce::jlimit(0.0f, 0.25f, static_cast<float>(object->getProperty("gain")));
                if (object->hasProperty("rate") || object->hasProperty("block"))
                {
                    if (object->hasProperty("rate")) rate = juce::jlimit(32000.0, 192000.0, static_cast<double>(object->getProperty("rate")));
                    if (object->hasProperty("block")) block = juce::jlimit(1, 8192, static_cast<int>(object->getProperty("block")));
                    startSource();
                }
                tone->gain.store(gain);
                if (object->hasProperty("pause")) tone->paused.store(static_cast<bool>(object->getProperty("pause")));
            }
        }
        const auto now = juce::Time::getMillisecondCounterHiRes();
        if (now > deadline) { quit(); return; }
        if (url.isEmpty())
        {
            url = readQr();
            if (url.isEmpty())
            {
                if (now > pairingDeadline) fail("Cannot read the installed plugin's QR code");
                return;
            }
            if (hideAfterPairing) window->setVisible(false);
        }
        auto object = std::make_unique<juce::DynamicObject>();
        object->setProperty("url", url);
        object->setProperty("port", juce::URL(url).getPort());
        object->setProperty("sampleRate", rate);
        object->setProperty("blockFrames", block);
        object->setProperty("gain", gain);
        object->setProperty("pluginPath", plugin.getFullPathName());
        object->setProperty("processLocalAuRegistration", auTestIdentifier.isNotEmpty());
        object->setProperty("loadedBinary", loadedBinary);
        object->setProperty("format", description.pluginFormatName);
        object->setProperty("pluginVersion", description.version);
        object->setProperty("snapshot", snapshot.getFullPathName());
        object->setProperty("processedBlocks", static_cast<juce::int64>(tone->blocks.load()));
        info.replaceWithText(juce::JSON::toString(juce::var(object.release())));
    }
    juce::AudioPluginFormatManager formats;
    juce::PluginDescription description;
    std::unique_ptr<juce::AudioPluginInstance> processor;
    std::unique_ptr<InstalledToneSource> tone;
    std::unique_ptr<Window> window;
    juce::File plugin, info, control, snapshot;
    juce::String url, loadedBinary, auTestIdentifier;
    CFBundleRef testAuBundle = nullptr;
    double rate = 48000, deadline = 0, pairingDeadline = 0;
    int block = 256;
    float gain = 0.25f;
    bool hideAfterPairing = false;
    id activity = nil;
};

START_JUCE_APPLICATION(InstalledHarness)
