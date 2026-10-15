// Test the released updater/editor as an older client against a real release.
// These two objects replace their static-library counterparts in this test only.
// No version override, test transport or test app enters a shipping bundle.
#undef MCONNECT_VERSION
#define MCONNECT_VERSION "0.1.2"
#define fetch observedFetch
#include "../src/PluginUpdates.h"
#undef fetch
#include "../src/PluginUpdates.cpp"
#include "../src/PluginEditor.cpp"

namespace
{
juce::File flowLog;
std::mutex logMutex;
void writeFlow(const juce::String& message)
{
    const std::lock_guard lock(logMutex);
    if (flowLog != juce::File())
        flowLog.appendText(juce::Time::getCurrentTime().toISO8601(true) + " " + message + "\n");
}
}
namespace mconnect::updates
{
Reply fetch(const Request&, const Cancelled&, const Progress&);
Reply observedFetch(const Request& request, const Cancelled& cancelled, const Progress& progress)
{
    auto reply = fetch(request, cancelled, progress);
    writeFlow("native status=" + juce::String(reply.status) + "; error=" + reply.error
        + "; sent_resume_bytes=" + juce::String(request.resumeData.getSize())
        + "; received_resume_bytes=" + juce::String(reply.resumeData.getSize()));
    return reply;
}
}

class UpdateFlow final : public juce::JUCEApplication, private juce::Timer
{
public:
    const juce::String getApplicationName() override { return "MConnect update flow test"; }
    const juce::String getApplicationVersion() override { return MCONNECT_VERSION; }
    bool moreThanOneInstanceAllowed() override { return false; }
    void initialise(const juce::String& commandLine) override
    {
        const auto args = juce::StringArray::fromTokens(commandLine, true);
        for (int i = 0; i + 1 < args.size(); ++i)
            if (args[i] == "--log") log = juce::File(args[++i].unquoted());
        if (log == juce::File()) { setApplicationReturnValue(2); quit(); return; }
        log.getParentDirectory().createDirectory();
        flowLog = log;
        record("start; updater/editor version override=0.1.2; real HTTPS; no audio device or DAW");
        processor = std::make_unique<mconnect::MConnectAudioProcessor>();
        // Monitoring is not needed for the update flow. No device is opened.
        processor->parameters.getParameter(mconnect::ParamIDs::streamEnabled)->setValueNotifyingHost(0);
        window = std::make_unique<Window>(processor->createEditor());
        updater = std::make_unique<juce::SharedResourcePointer<mconnect::updates::Service>>();
        deadline = juce::Time::getMillisecondCounterHiRes() + 40 * 60 * 1000;
        startTimer(100);
    }
    void shutdown() override
    {
        stopTimer(); window.reset(); updater.reset(); processor.reset();
        record("host closed; editor and native transport drained");
    }
    void systemRequestedQuit() override { quit(); }
    void anotherInstanceStarted(const juce::String&) override {}
private:
    class Window final : public juce::DocumentWindow
    {
    public:
        explicit Window(juce::AudioProcessorEditor* editor)
            : DocumentWindow("MConnect | live update flow test", juce::Colour(0xff101016), allButtons)
        {
            setUsingNativeTitleBar(true); setContentOwned(editor, true);
            centreWithSize(getWidth(), getHeight()); setVisible(true);
        }
        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
    };
    void record(const juce::String& message)
    {
        writeFlow(message);
    }
    void timerCallback() override
    {
        const auto state = (*updater)->snapshot();
        juce::String label;
        if (window)
            for (auto* child : window->getContentComponent()->getChildren())
                if (auto* button = dynamic_cast<juce::Button*>(child); button && button->getY() == 503)
                    label = button->getButtonText();
        const auto message = "phase=" + juce::String(static_cast<int>(state.phase))
            + "; button=" + label + "; error=" + state.error;
        if (message != previous) { record(message); previous = message; }
        if (state.phase == mconnect::updates::Phase::ready && !verified)
        {
            verified = state.release && mconnect::updates::verifyInstaller(state.installer, *state.release);
            record("verified=" + juce::String(verified ? "true" : "false")
                + "; package=" + state.installer.getFullPathName()
                + "; bytes=" + juce::String(state.installer.getSize())
                + "; sha256=" + juce::SHA256(state.installer).toHexString());
        }
        if (juce::Time::getMillisecondCounterHiRes() > deadline)
        { record("timeout"); setApplicationReturnValue(3); quit(); }
    }
    juce::File log;
    juce::String previous;
    double deadline = 0;
    bool verified = false;
    std::unique_ptr<mconnect::MConnectAudioProcessor> processor;
    std::unique_ptr<Window> window;
    std::unique_ptr<juce::SharedResourcePointer<mconnect::updates::Service>> updater;
};

START_JUCE_APPLICATION(UpdateFlow)
int main(int argc, char** argv)
{
    juce::JUCEApplicationBase::createInstance = &juce_CreateApplication;
    return juce::JUCEApplicationBase::main(argc, const_cast<const char**>(argv));
}
