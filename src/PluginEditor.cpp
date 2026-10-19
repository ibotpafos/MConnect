#include "PluginEditor.h"
#include <qrcodegen.hpp>
namespace mconnect
{
namespace
{
const auto theme = gdkit::palette(gdkit::Product::mconnect);
const auto background = theme.background, panel = theme.surface, muted = theme.muted;
void text(juce::Graphics& g, const juce::String& value, juce::Rectangle<int> bounds, float size, juce::Colour colour, int style = juce::Font::plain, juce::Justification align = juce::Justification::centred)
{
    g.setColour(colour); g.setFont(gdkit::font(size, (style & juce::Font::bold) != 0)); g.drawFittedText(value, bounds, align, 2);
}
}
MConnectAudioProcessorEditor::MConnectAudioProcessorEditor(MConnectAudioProcessor& p) : AudioProcessorEditor(p), owner(p)
{
    owner.editorOpened();
    setLookAndFeel(&look);
    addAndMakeVisible(stream); addAndMakeVisible(copy); addAndMakeVisible(version);
    stream.onClick = [this]
    {
        auto* parameter = owner.parameters.getParameter(ParamIDs::streamEnabled);
        parameter->beginChangeGesture();
        parameter->setValueNotifyingHost(parameter->getValue() >= 0.5f ? 0.0f : 1.0f);
        parameter->endChangeGesture();
    };
    copy.onClick = [this] { juce::SystemClipboard::copyTextToClipboard(pairingUrl); copy.setButtonText("Copied"); copyTicks = 10; };
    stream.setTooltip("Stop or start phone monitoring. Your DAW audio stays unchanged.");
    copy.setTooltip("Alternative to scanning the QR. The link includes the pairing key.");
    version.setText(juce::String("v") + MCONNECT_VERSION, juce::dontSendNotification);
    version.setJustificationType(juce::Justification::centred);
    version.setFont(gdkit::font(11.0f));
    version.setColour(juce::Label::textColourId, muted);
    version.setTooltip("Manage plugin updates in GD Connect.");
    setSize(420, 540); timerCallback(); startTimerHz(5);
}
MConnectAudioProcessorEditor::~MConnectAudioProcessorEditor()
{
    stopTimer(); setLookAndFeel(nullptr); owner.editorClosed();
}
void MConnectAudioProcessorEditor::resized()
{
    stream.setBounds(307, 26, 83, 30);
    copy.setBounds(151, 420, 118, 28);
    version.setBounds(30, 503, 360, 24);
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
}
void MConnectAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(background);
    gdkit::paintHeader(g, {28, 20, 260, 48}, theme, "Link", "Phone monitoring");
    text(g, currentStatus, {30, 76, 360, 24}, 13, running ? theme.success : muted);
    g.setColour(panel); g.fillRoundedRectangle(70, 117, 280, 254, 18);
    g.setColour(juce::Colours::white); g.fillRoundedRectangle(90, 132, 240, 224, 12);
    if (qr.isValid()) g.drawImageAt(qr, 210 - qr.getWidth()/2, 244 - qr.getHeight()/2);
    else text(g, "Start the stream\nto connect your phone", {112, 204, 196, 80}, 17, background);
    text(g, "Scan. Tap Listen. That's it.", {35, 384, 350, 24}, 16, theme.text, juce::Font::bold);
    for (int c = 0; c < 2; ++c)
    {
        const int y = 473 + c*10;
        gdkit::paintMeter(g, {30.0f, static_cast<float>(y), 360.0f, 3.0f}, displayedPeaks[c], theme);
    }
}
}
