#include "PluginEditor.h"
#include "BinaryData.h"

namespace {
class BridgeWebView : public juce::WebBrowserComponent {
public:
    BridgeWebView(const Options& opts, std::function<void(const juce::String&)> cb)
        : juce::WebBrowserComponent(opts), onBridge(std::move(cb)) {}
    bool pageAboutToLoad(const juce::String& newURL) override {
        if (newURL.startsWith("https://juce.bridge/")) {
            if (onBridge) onBridge(newURL);
            return false;
        }
        return true;
    }
private:
    std::function<void(const juce::String&)> onBridge;
};
}

GoaRiserMachineEditor::GoaRiserMachineEditor(GoaRiserMachineProcessor& p)
    : AudioProcessorEditor(&p), processor(p) {
    auto opts = juce::WebBrowserComponent::Options{}
        .withBackend(juce::WebBrowserComponent::Options::Backend::webview2)
        .withWinWebView2Options(
            juce::WebBrowserComponent::Options::WinWebView2{}
                .withUserDataFolder(juce::File::getSpecialLocation(juce::File::tempDirectory))
        );
    webView = std::make_unique<BridgeWebView>(opts, [this](const juce::String& url) {
        auto jsonStr = juce::URL::removeEscapeChars(url.fromFirstOccurrenceOf("data=", false, false));
        processor.setParamsFromJson(jsonStr);
    });
    addAndMakeVisible(*webView);
    auto html = juce::String::fromUTF8(BinaryData::ui_html, BinaryData::ui_htmlSize);
    auto tempHtml = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("goa_riser_ui.html");
    tempHtml.replaceWithText(html);
    webView->goToURL(juce::URL(tempHtml).toString(false));
    setResizable(true, true);
    setSize(960, 520);
    startTimerHz(25);
}

void GoaRiserMachineEditor::resized() {
    if (webView) webView->setBounds(getLocalBounds());
}

void GoaRiserMachineEditor::timerCallback() {
    if (webView)
        webView->evaluateJavascript("if(window.updateRiseProgress) window.updateRiseProgress(" + juce::String(processor.getProgress(), 3) + ");");
}
