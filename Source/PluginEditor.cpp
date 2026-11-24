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
        handleBridge(url);
    });
    addAndMakeVisible(*webView);
    auto html = juce::String::fromUTF8(BinaryData::ui_html, BinaryData::ui_htmlSize);
    auto tempHtml = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("goa_fx_lab_v3_ui.html");
    tempHtml.replaceWithText(html);
    webView->goToURL(juce::URL(tempHtml).toString(false));
    setResizable(true, true);
    setSize(1080, 620);
    startTimerHz(25);
}

void GoaRiserMachineEditor::resized() {
    if (webView) webView->setBounds(getLocalBounds());
}

void GoaRiserMachineEditor::timerCallback() {
    if (webView)
        webView->evaluateJavascript("if(window.updateRiseProgress) window.updateRiseProgress(" + juce::String(processor.getProgress(), 3) + ");");
}

void GoaRiserMachineEditor::handleBridge(const juce::String& url) {
    auto path = url.fromFirstOccurrenceOf("https://juce.bridge/", false, false).upToFirstOccurrenceOf("?", false, false);
    if (path == "params") {
        auto jsonStr = juce::URL::removeEscapeChars(url.fromFirstOccurrenceOf("data=", false, false));
        processor.setParamsFromJson(jsonStr);
    } else if (path == "dragwav") {
        auto fname = juce::URL::removeEscapeChars(url.fromFirstOccurrenceOf("name=", false, false));
        if (fname.isEmpty()) fname = "GoaFX_Sample.wav";
        auto outFile = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile(fname);
        processor.renderCurrentFxToWav(outFile);
        performExternalDragDropOfFiles({ outFile.getFullPathName() }, false);
    } else if (path == "savewav") {
        auto fname = juce::URL::removeEscapeChars(url.fromFirstOccurrenceOf("name=", false, false));
        if (fname.isEmpty()) fname = "GoaFX_Sample.wav";
        auto dir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("GoaFX_Exports");
        auto outFile = dir.getChildFile(fname);
        processor.renderCurrentFxToWav(outFile);
        if (webView)
            webView->evaluateJavascript("if(window.showExportToast) window.showExportToast('Saved to Documents/GoaFX_Exports/" + fname + "');");
    }
}
