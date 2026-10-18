#include "PluginProcessor.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <iostream>
#include <stdexcept>

namespace
{
void require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}
}

int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    try
    {
        mconnect::MConnectAudioProcessor processor;
        double longestClose = 0;
        for (int i = 0; i < 8; ++i)
        {
            std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
            require(editor->getWidth() == 420 && editor->getHeight() == 540, "unexpected editor size");
            for (auto* child : editor->getChildren())
                require(editor->getLocalBounds().contains(child->getBounds()), "control outside editor");
            if (i == 0)
            {
                auto image = editor->createComponentSnapshot(editor->getLocalBounds(), true, 2.0f);
                const auto file = juce::File::getCurrentWorkingDirectory().getChildFile("output/editor/editor.png");
                require(file.getParentDirectory().createDirectory().wasOk(), "cannot create snapshot directory");
                auto output = file.createOutputStream();
                require(output != nullptr && juce::PNGImageFormat().writeImageToStream(image, *output),
                        "editor screenshot failed");
            }
            const auto start = juce::Time::getMillisecondCounterHiRes();
            editor.reset();
            longestClose = juce::jmax(longestClose, juce::Time::getMillisecondCounterHiRes() - start);
        }
        require(longestClose < 1000, "editor close blocked");
        std::cout << "PASS: eight editor opens/closes, layout and snapshot; max close " << longestClose << " ms\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
