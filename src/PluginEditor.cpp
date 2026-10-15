#include "PluginEditor.h"
#include <qrcodegen.hpp>
namespace mconnect
{
namespace
{
const juce::Colour background {0xff101016}, panel {0xff1b1b25}, muted {0xff9c9cab}, accent {0xffbcabff};
void text(juce::Graphics& g, const juce::String& value, juce::Rectangle<int> bounds, float size, juce::Colour colour, int style = juce::Font::plain, juce::Justification align = juce::Justification::centred)
{
    g.setColour(colour); g.setFont(juce::FontOptions(size, style)); g.drawFittedText(value, bounds, align, 2);
}
}
MConnectAudioProcessorEditor::MConnectAudioProcessorEditor(MConnectAudioProcessor& p) : AudioProcessorEditor(p), owner(p)
{
    owner.editorOpened();
    look.setColour(juce::TextButton::buttonColourId, panel);
    look.setColour(juce::TextButton::textColourOffId, accent);
    setLookAndFeel(&look);
    addAndMakeVisible(stream); addAndMakeVisible(copy); addAndMakeVisible(update);
    stream.onClick = [this]
    {
        auto* parameter = owner.parameters.getParameter(ParamIDs::streamEnabled);
        parameter->beginChangeGesture();
        parameter->setValueNotifyingHost(parameter->getValue() >= 0.5f ? 0.0f : 1.0f);
        parameter->endChangeGesture();
    };
    copy.onClick = [this] { juce::SystemClipboard::copyTextToClipboard(pairingUrl); copy.setButtonText("Copied"); copyTicks = 10; };
    update.onClick = [this]
    {
        const auto status = updater->snapshot();
        if (status.phase == updates::Phase::ready)
        {
            // Recheck on the worker: macOS may purge cache files while a window
            // stays open. Missing/changed packages are downloaded again.
            installWhenReady = true;
            updater->download();
        }
        else if (status.release) updater->download();
        else updater->check(true);
    };
    stream.setTooltip("Stop or start phone monitoring. Your DAW audio stays unchanged.");
    copy.setTooltip("Alternative to scanning the QR. The link includes the pairing key.");
    update.setTooltip("Checks GitHub Releases once a day. Audio stays on your local network. Updates are installed after closing the DAW.");
    setSize(420, 540); updater->check(); timerCallback(); startTimerHz(5);
}
MConnectAudioProcessorEditor::~MConnectAudioProcessorEditor()
{
    stopTimer(); setLookAndFeel(nullptr); owner.editorClosed();
}
void MConnectAudioProcessorEditor::resized()
{
    stream.setBounds(307, 26, 83, 30);
    copy.setBounds(151, 420, 118, 28);
    update.setBounds(30, 503, 360, 24);
}
void MConnectAudioProcessorEditor::timerCallback()
{
    const auto stats = owner.getStreamStats();
    const bool enabled = owner.parameters.getRawParameterValue(ParamIDs::streamEnabled)->load() >= 0.5f;
    stream.setButtonText(enabled ? "Stop" : "Start");
    auto nextStatus = stats.serverRunning
        ? (stats.webrtcOpenTracks > 0 ? juce::String(stats.webrtcOpenTracks) + " listening" : "Ready to connect")
        : (enabled ? "Starting..." : "Stream paused");
    const auto url = owner.getPairingUrl();
    if (url != pairingUrl)
    {
        pairingUrl = url; qr = {};
        if (url.isNotEmpty()) try
        {
            const auto code = qrcodegen::QrCode::encodeText(url.toRawUTF8(), qrcodegen::QrCode::Ecc::MEDIUM);
            const int size = code.getSize() + 8, scale = juce::jmax(1, 224 / size);
            qr = juce::Image(juce::Image::RGB, size * scale, size * scale, true);
            juce::Graphics imageGraphics(qr); imageGraphics.fillAll(juce::Colours::white); imageGraphics.setColour(juce::Colours::black);
            for (int y = 0; y < code.getSize(); ++y)
                for (int x = 0; x < code.getSize(); ++x)
                    if (code.getModule(x, y)) imageGraphics.fillRect((x + 4) * scale, (y + 4) * scale, scale, scale);
        }
        catch (const std::exception&) { nextStatus = "Copy link to connect"; }
        copy.setEnabled(url.isNotEmpty());
        repaint();
    }
    if (nextStatus != currentStatus || running != stats.serverRunning)
    {
        currentStatus = nextStatus; running = stats.serverRunning; repaint(30, 76, 360, 24);
    }
    for (int i = 0; i < 2; ++i) displayedPeaks[i] = juce::jmax(owner.getPeak(i), displayedPeaks[i] * 0.65f);
    repaint(28, 470, 364, 18);
    if (copyTicks > 0 && --copyTicks == 0) copy.setButtonText("Copy link");
    const auto status = updater->snapshot();
    if (installWhenReady && status.phase == updates::Phase::ready)
    {
        installWhenReady = false;
        const auto installer = status.installer;
        installPrompt = juce::AlertWindow::showScopedAsync(juce::MessageBoxOptions()
            .withIconType(juce::MessageBoxIconType::InfoIcon).withTitle("MConnect update")
            .withMessage("The installer is ready. Save your project and close the DAW before continuing the installation. The new version will load next time you open the DAW.")
            .withButton("Open installer").withButton("Later").withAssociatedComponent(this),
            [installer](int result) { if (result == 1 && installer.existsAsFile()) installer.startAsProcess(); });
    }
    if (status.phase == updates::Phase::available || status.phase == updates::Phase::failed)
        installWhenReady = false;
    auto label = juce::String("v") + MCONNECT_VERSION + "  ·  Check updates";
    if (status.phase == updates::Phase::checking) label = "Checking for updates...";
    if (status.release)
    {
        label = "Update " + status.release->version + " available  ·  Download";
        if (status.error.isNotEmpty()) label = "Update " + status.release->version + "  ·  Retry download";
        if (status.error.contains("disk space")) label = "Free disk space  ·  Retry update";
    }
    if (status.phase == updates::Phase::downloading)
        label = installWhenReady ? "Preparing installer..." : "Downloading update... " + juce::String(status.percent) + "%";
    if (status.phase == updates::Phase::ready) label = "Update " + status.release->version + " ready  ·  Open installer";
    if (status.phase == updates::Phase::current) label = juce::String("v") + MCONNECT_VERSION + "  ·  Up to date";
    if (label != updateText) { updateText = label; update.setButtonText(label); }
    update.setTooltip(status.error.isNotEmpty() ? status.error
        : "Checks GitHub Releases once a day. Audio stays on your local network. Updates are installed after closing the DAW.");
    update.setEnabled(status.phase != updates::Phase::checking && status.phase != updates::Phase::downloading);
    if (++updateTicks >= 300) { updateTicks = 0; updater->check(); }
}
void MConnectAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(background);
    g.setColour(accent); g.fillRoundedRectangle(28, 24, 34, 34, 10);
    text(g, "M", {28, 25, 34, 30}, 23, background, juce::Font::bold);
    text(g, "MConnect", {75, 22, 210, 38}, 24, juce::Colours::white, juce::Font::bold, juce::Justification::centredLeft);
    text(g, currentStatus, {30, 76, 360, 24}, 13, running ? juce::Colour(0xff91e1b1) : muted);
    g.setColour(panel); g.fillRoundedRectangle(70, 117, 280, 254, 18);
    g.setColour(juce::Colours::white); g.fillRoundedRectangle(90, 132, 240, 224, 12);
    if (qr.isValid()) g.drawImageAt(qr, 210 - qr.getWidth()/2, 244 - qr.getHeight()/2);
    else text(g, "Start the stream\nto connect your phone", {112, 204, 196, 80}, 17, background);
    text(g, "Scan. Tap Listen. That's it.", {35, 384, 350, 24}, 16, juce::Colours::white, juce::Font::bold);
    for (int c = 0; c < 2; ++c)
    {
        const int y = 473 + c*10;
        g.setColour(panel); g.fillRoundedRectangle(30, static_cast<float>(y), 360, 3, 1);
        const float fill = juce::jlimit(0.0f, 1.0f, (juce::Decibels::gainToDecibels(displayedPeaks[c], -60.0f) + 60) / 60);
        g.setColour(displayedPeaks[c] >= 1 ? juce::Colour(0xffff9b91) : accent);
        g.fillRoundedRectangle(30, static_cast<float>(y), 360*fill, 3, 1);
    }
}
}
