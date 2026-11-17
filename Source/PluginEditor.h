#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include <array>
#include <vector>
#include <memory>

class ClassicWinLookAndFeel : public juce::LookAndFeel_V4
{
public:
    ClassicWinLookAndFeel();

    static void drawRaisedBevel(juce::Graphics& g, juce::Rectangle<int> bounds, bool depressed = false);
    static void drawSunkenBevel(juce::Graphics& g, juce::Rectangle<int> bounds);
    static juce::Font getClassicFont(float size = 11.0f, bool bold = false);

    void drawButtonBackground(juce::Graphics& g, juce::Button& button,
                              const juce::Colour& backgroundColour,
                              bool shouldDrawButtonAsHighlighted,
                              bool shouldDrawButtonAsDown) override;

    void drawButtonText(juce::Graphics& g, juce::TextButton& button,
                        bool shouldDrawButtonAsHighlighted,
                        bool shouldDrawButtonAsDown) override;

    void drawToggleButton(juce::Graphics& g, juce::ToggleButton& button,
                          bool shouldDrawButtonAsHighlighted,
                          bool shouldDrawButtonAsDown) override;

    void drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height,
                          float sliderPos, float minSliderPos, float maxSliderPos,
                          juce::Slider::SliderStyle style, juce::Slider& slider) override;

    void drawComboBox(juce::Graphics& g, int width, int height, bool isButtonDown,
                      int buttonX, int buttonY, int buttonW, int buttonH,
                      juce::ComboBox& box) override;

    void drawPopupMenuBackground(juce::Graphics& g, int width, int height) override;
    void drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area,
                           bool isSeparator, bool isActive, bool isHighlighted,
                           bool isTicked, bool hasSubMenu,
                           const juce::String& text, const juce::String& shortcutKeyText,
                           const juce::Drawable* icon, const juce::Colour* textColour) override;

    juce::Font getComboBoxFont(juce::ComboBox&) override { return getClassicFont(11.0f, false); }
    juce::Font getPopupMenuFont() override { return getClassicFont(11.0f, false); }
};

class EchoplasmAudioProcessorEditor : public juce::AudioProcessorEditor,
                                     private juce::Timer
{
public:
    enum class DrawTool
    {
        Pencil = 0,
        Brush,
        Eraser,
        Bucket,
        Eyedropper,
        Rectangle,
        Line
    };

    enum class BrushShape
    {
        Square = 0,
        Circle,
        Diamond
    };

    explicit EchoplasmAudioProcessorEditor(EchoplasmAudioProcessor&);
    ~EchoplasmAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress& key) override;

private:
    class ToolIconButton : public juce::Button
    {
    public:
        enum class IconKind
        {
            Pencil = 0,
            Brush,
            Eraser,
            Bucket,
            Eyedropper,
            Rectangle,
            Line,
            Trash
        };

        ToolIconButton(IconKind kind, const juce::String& tooltipText);
        void paintButton(juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

    private:
        IconKind iconKind;
    };

    class SwatchButton : public juce::Button
    {
    public:
        explicit SwatchButton(int swatchIdx);
        void paintButton(juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;
        int getSwatchIndex() const noexcept { return swatchIndex; }

    private:
        int swatchIndex;
    };

    class EchoCanvasComponent : public juce::Component
    {
    public:
        explicit EchoCanvasComponent(EchoplasmAudioProcessorEditor& ownerEditor);
        void paint(juce::Graphics& g) override;
        void mouseDown(const juce::MouseEvent& e) override;
        void mouseDrag(const juce::MouseEvent& e) override;
        void mouseUp(const juce::MouseEvent& e) override;
        void mouseMove(const juce::MouseEvent& e) override;
        void mouseExit(const juce::MouseEvent& e) override;

        juce::Rectangle<int> getGridBounds() const;

    private:
        EchoplasmAudioProcessorEditor& owner;
        bool isDragging = false;
        bool erasingStroke = false;
        int dragStartBand = -1;
        int dragStartTap = -1;
        int dragCurrentBand = -1;
        int dragCurrentTap = -1;
        int hoverBand = -1;
        int hoverTap = -1;
        EchoplasmAudioProcessor::FrameGrid strokeSnapshot {};

        bool hitTestCell(juce::Point<int> pos, int& outBand, int& outTap) const;
        void applyBrushAt(EchoplasmAudioProcessor::FrameGrid& grid, int centerBand, int centerTap, int swatchIdx) const;
        void applyLineToGrid(EchoplasmAudioProcessor::FrameGrid& grid, int b0, int t0, int b1, int t1, int swatchIdx) const;
        void applyRectToGrid(EchoplasmAudioProcessor::FrameGrid& grid, int b0, int t0, int b1, int t1, int swatchIdx) const;
        void floodFillGrid(EchoplasmAudioProcessor::FrameGrid& grid, int band, int tap, int targetSwatch, int replacementSwatch) const;
        int quantizeCoord(int idx) const;
    };

    class PreviewThumbnailComponent : public juce::Component
    {
    public:
        explicit PreviewThumbnailComponent(EchoplasmAudioProcessor& proc);
        void paint(juce::Graphics& g) override;

    private:
        EchoplasmAudioProcessor& processor;
    };

    class SpectrumVisualizerComponent : public juce::Component
    {
    public:
        explicit SpectrumVisualizerComponent(EchoplasmAudioProcessor& proc);
        void paint(juce::Graphics& g) override;
        void updateSpectrum();

    private:
        static constexpr int kFftOrder = 10;
        static constexpr int kFftSize = 1 << kFftOrder;
        static constexpr int kNumDisplayBins = 64;

        EchoplasmAudioProcessor& processor;
        juce::dsp::FFT forwardFFT { kFftOrder };
        juce::dsp::WindowingFunction<float> window { kFftSize, juce::dsp::WindowingFunction<float>::hann };

        std::array<float, kFftSize> sampleRing {};
        int ringWritePos = 0;
        std::array<float, kFftSize * 2> fftWorkspace {};
        std::array<float, kNumDisplayBins> smoothedMagnitudes {};
        std::array<float, kFftSize> pullScratch {};
    };

    void timerCallback() override;

    // Undo / Redo helpers
    void pushUndoSnapshot();
    void performUndo();
    void performRedo();

    // Menu actions
    void showMenuForIndex(int menuIndex, juce::Component* anchor);
    void openImageImportChooser();
    void importImageFile(const juce::File& file);
    int mapColourToSwatch(juce::Colour c, bool invert) const;
    EchoplasmAudioProcessor::FrameGrid downsampleImageToGrid(const juce::Image& img, bool invert) const;

    // Flipbook UI sync
    void rebuildFrameButtons();
    void updateReadoutsAndStates();

    EchoplasmAudioProcessor& audioProcessor;
    ClassicWinLookAndFeel classicLAF;

    // Top menu bar buttons
    std::array<juce::TextButton, 7> menuButtons {
        juce::TextButton("File"),
        juce::TextButton("Edit"),
        juce::TextButton("Canvas"),
        juce::TextButton("Tempo"),
        juce::TextButton("Resolution"),
        juce::TextButton("View"),
        juce::TextButton("Help")
    };

    // Context strip controls
    juce::Label opacityTitleLabel { {}, "Opacity:" };
    juce::Slider opacitySlider;
    juce::Label opacityValueLabel { {}, "100%" };

    juce::Label sizeTitleLabel { {}, "Size:" };
    juce::ComboBox sizeCombo;

    juce::Label shapeTitleLabel { {}, "Shape:" };
    juce::ComboBox shapeCombo;

    juce::Label colorTitleLabel { {}, "Color:" };
    juce::TextButton powerToggleButton { "On" };

    // Left tool dock buttons
    std::array<std::unique_ptr<ToolIconButton>, 7> toolButtons;
    std::unique_ptr<ToolIconButton> trashButton;

    // Center canvas + swatches
    EchoCanvasComponent echoCanvas { *this };
    std::array<std::unique_ptr<SwatchButton>, EchoplasmAudioProcessor::kNumSwatches> swatchButtons;

    // Right inspector controls
    juce::Label mixTitleLabel { {}, "Mix" };
    juce::Slider mixSlider;
    juce::Label mixValueLabel { {}, "50.0 %" };

    juce::Label feedbackTitleLabel { {}, "Feedback" };
    juce::Slider feedbackSlider;
    juce::Label feedbackValueLabel { {}, "25.0 %" };

    juce::Label outputTitleLabel { {}, "Output" };
    juce::Slider outputSlider;
    juce::Label outputValueLabel { {}, "0.0 dB" };

    juce::ToggleButton freezeCheckbox { "Freeze" };

    // Preview + Flipbook controls
    PreviewThumbnailComponent previewThumbnail { audioProcessor };
    std::vector<std::unique_ptr<juce::TextButton>> frameSelectButtons;
    juce::TextButton addFrameButton { "+" };
    juce::TextButton removeFrameButton { "-" };
    juce::TextButton sequenceButton { "Sequence" };
    juce::ToggleButton loopCheckbox { "Loop" };
    juce::Label timeLabel { {}, "Time" };
    juce::ComboBox frameTimeCombo;

    // Bottom spectrum visualizer
    SpectrumVisualizerComponent spectrumVisualizer { audioProcessor };

    // APVTS attachments
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> opacityAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> mixAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> feedbackAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> outputAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> freezeAttachment;

    // Editor state
    DrawTool currentTool = DrawTool::Pencil;
    int brushSize = 1;
    BrushShape brushShape = BrushShape::Square;
    int activeSwatch = EchoplasmAudioProcessor::SwatchOrange;
    int resolutionStep = 1; // 1 = 16x16, 2 = 8x8, 4 = 4x4
    bool showGridLines = true;
    bool showBeatMarkers = true;

    struct UndoEntry
    {
        int frameIndex = 0;
        EchoplasmAudioProcessor::FrameGrid grid {};
    };
    std::vector<UndoEntry> undoStack;
    std::vector<UndoEntry> redoStack;

    EchoplasmAudioProcessor::FrameGrid clipboardGrid {};
    bool hasClipboardGrid = false;

    int lastSyncedNumFrames = -1;
    int lastSyncedActiveFrame = -1;

    std::unique_ptr<juce::FileChooser> activeFileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EchoplasmAudioProcessorEditor)
};
