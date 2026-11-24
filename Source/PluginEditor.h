#pragma once
#include "PluginProcessor.h"
#include <juce_gui_extra/juce_gui_extra.h>

class GoaRiserMachineEditor : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit GoaRiserMachineEditor(GoaRiserMachineProcessor&);
    ~GoaRiserMachineEditor() override = default;
    void resized() override;
private:
    void timerCallback() override;
    GoaRiserMachineProcessor& processor;
    std::unique_ptr<juce::WebBrowserComponent> webView;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GoaRiserMachineEditor)
};
