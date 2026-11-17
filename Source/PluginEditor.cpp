#include "PluginEditor.h"
#include <cmath>
#include <algorithm>
#include <cstring>

namespace
{
    const juce::Colour kWinSilver    { 0xFFC0C0C0 };
    const juce::Colour kWinLight     { 0xFFFFFFFF };
    const juce::Colour kWinHighlight { 0xFFDFDFDF };
    const juce::Colour kWinShadow    { 0xFF808080 };
    const juce::Colour kWinDark      { 0xFF000000 };
    const juce::Colour kWinNavy      { 0xFF000080 };
    const juce::Colour kSpecFill     { 0xFF9C8CFF };
    const juce::Colour kSpecStroke   { 0xFF5647B8 };

    std::vector<juce::Image> decodeGifFrames(const juce::File& file)
    {
        std::vector<juce::Image> outFrames;
        juce::MemoryBlock raw;
        if (!file.loadFileAsData(raw) || raw.getSize() < 14) return outFrames;
        const auto* data = static_cast<const uint8_t*>(raw.getData());
        const size_t size = raw.getSize();
        if (std::memcmp(data, "GIF87a", 6) != 0 && std::memcmp(data, "GIF89a", 6) != 0) return outFrames;

        const int canvasW = data[6] | (data[7] << 8);
        const int canvasH = data[8] | (data[9] << 8);
        if (canvasW <= 0 || canvasH <= 0 || canvasW > 4096 || canvasH > 4096) return outFrames;

        const uint8_t packed = data[10];
        const bool hasGlobal = (packed & 0x80) != 0;
        const int globalCount = hasGlobal ? (1 << ((packed & 0x07) + 1)) : 0;
        size_t pos = 13;
        std::vector<juce::Colour> globalPal(256, juce::Colours::white);
        if (hasGlobal)
        {
            if (pos + static_cast<size_t>(globalCount) * 3 > size) return outFrames;
            for (int i = 0; i < globalCount; ++i, pos += 3)
                globalPal[static_cast<size_t>(i)] = juce::Colour(data[pos], data[pos + 1], data[pos + 2]);
        }

        juce::Image composite(juce::Image::ARGB, canvasW, canvasH, true);
        { juce::Graphics cg(composite); cg.fillAll(juce::Colours::white); }

        int transIdx = -1, disposal = 0;
        auto readSubBlocks = [&](size_t& p, std::vector<uint8_t>& out)
        {
            out.clear();
            while (p < size)
            {
                const uint8_t len = data[p++];
                if (len == 0) break;
                const size_t avail = juce::jmin(static_cast<size_t>(len), size - p);
                out.insert(out.end(), data + p, data + p + avail);
                p += avail;
            }
        };

        while (pos < size && outFrames.size() < 32)
        {
            const uint8_t intro = data[pos++];
            if (intro == 0x3B) break;
            if (intro == 0x21)
            {
                if (pos >= size) break;
                const uint8_t lbl = data[pos++];
                if (lbl == 0xF9 && pos + 5 < size)
                {
                    const uint8_t bsz = data[pos++];
                    if (bsz >= 4 && pos + bsz <= size)
                    {
                        disposal = (data[pos] >> 2) & 0x07;
                        transIdx = (data[pos] & 0x01) ? static_cast<int>(data[pos + 3]) : -1;
                        pos += bsz;
                        if (pos < size && data[pos] == 0) ++pos;
                    }
                    else { std::vector<uint8_t> skip; readSubBlocks(pos, skip); }
                }
                else { std::vector<uint8_t> skip; readSubBlocks(pos, skip); }
            }
            else if (intro == 0x2C)
            {
                if (pos + 9 > size) break;
                const int ix = data[pos] | (data[pos + 1] << 8);
                const int iy = data[pos + 2] | (data[pos + 3] << 8);
                const int iw = data[pos + 4] | (data[pos + 5] << 8);
                const int ih = data[pos + 6] | (data[pos + 7] << 8);
                const uint8_t ip = data[pos + 8];
                pos += 9;

                const bool hasLocal = (ip & 0x80) != 0, interlaced = (ip & 0x40) != 0;
                const int localCount = hasLocal ? (1 << ((ip & 0x07) + 1)) : 0;
                auto pal = globalPal;
                if (hasLocal)
                {
                    if (pos + static_cast<size_t>(localCount) * 3 > size) break;
                    for (int i = 0; i < localCount; ++i, pos += 3)
                        pal[static_cast<size_t>(i)] = juce::Colour(data[pos], data[pos + 1], data[pos + 2]);
                }
                if (pos >= size) break;
                const int minBits = data[pos++];
                std::vector<uint8_t> comp;
                readSubBlocks(pos, comp);

                if (minBits >= 2 && minBits <= 8 && iw > 0 && ih > 0)
                {
                    const int clearC = 1 << minBits, endC = clearC + 1;
                    int codeSz = minBits + 1, nextC = endC + 1;
                    struct Entry { int prefix; uint8_t first, suffix; };
                    std::vector<Entry> dict(4096);
                    auto resetD = [&]()
                    {
                        codeSz = minBits + 1; nextC = endC + 1;
                        for (int i = 0; i < clearC; ++i)
                            dict[static_cast<size_t>(i)] = { -1, static_cast<uint8_t>(i), static_cast<uint8_t>(i) };
                    };
                    resetD();

                    std::vector<uint8_t> indices;
                    indices.reserve(static_cast<size_t>(iw * ih));
                    size_t bitPos = 0, totalBits = comp.size() * 8;
                    auto readBits = [&](int n) -> int
                    {
                        if (bitPos + static_cast<size_t>(n) > totalBits) return -1;
                        int val = 0;
                        for (int b = 0; b < n; ++b)
                        {
                            const size_t bp = bitPos + static_cast<size_t>(b);
                            val |= (((comp[bp >> 3] >> (bp & 7)) & 1) << b);
                        }
                        bitPos += static_cast<size_t>(n);
                        return val;
                    };

                    int prevC = -1;
                    std::vector<uint8_t> stk;
                    while (indices.size() < static_cast<size_t>(iw * ih))
                    {
                        const int code = readBits(codeSz);
                        if (code < 0 || code == endC) break;
                        if (code == clearC) { resetD(); prevC = -1; continue; }
                        if (prevC == -1)
                        {
                            if (code < clearC) { indices.push_back(static_cast<uint8_t>(code)); prevC = code; }
                            continue;
                        }
                        int cur = code;
                        stk.clear();
                        if (cur == nextC) { stk.push_back(dict[static_cast<size_t>(prevC)].first); cur = prevC; }
                        else if (cur > nextC) break;
                        int guard = 0;
                        while (cur >= clearC && guard++ < 4096)
                        {
                            stk.push_back(dict[static_cast<size_t>(cur)].suffix);
                            cur = dict[static_cast<size_t>(cur)].prefix;
                        }
                        if (cur < 0 || cur >= clearC) break;
                        const uint8_t firstChar = static_cast<uint8_t>(cur);
                        stk.push_back(firstChar);
                        for (auto it = stk.rbegin(); it != stk.rend(); ++it) indices.push_back(*it);
                        if (nextC < 4096)
                        {
                            dict[static_cast<size_t>(nextC++)] = { prevC, dict[static_cast<size_t>(prevC)].first, firstChar };
                            if (nextC == (1 << codeSz) && codeSz < 12) ++codeSz;
                        }
                        prevC = code;
                    }

                    juce::Image saved;
                    if (disposal == 3) saved = composite.createCopy();
                    auto mapRow = [&](int r) -> int
                    {
                        if (!interlaced) return r;
                        const int p0 = (ih + 7) / 8, p1 = (ih + 3) / 8, p2 = (ih + 1) / 4;
                        if (r < p0) return r * 8;
                        r -= p0; if (r < p1) return r * 8 + 4;
                        r -= p1; if (r < p2) return r * 4 + 2;
                        return (r - p2) * 2 + 1;
                    };

                    const int pxCount = juce::jmin(iw * ih, static_cast<int>(indices.size()));
                    for (int p = 0; p < pxCount; ++p)
                    {
                        const int gx = ix + (p % iw), gy = iy + mapRow(p / iw);
                        if (gx >= 0 && gx < canvasW && gy >= 0 && gy < canvasH)
                        {
                            const int ci = static_cast<int>(indices[static_cast<size_t>(p)]);
                            if (ci != transIdx) composite.setPixelAt(gx, gy, pal[static_cast<size_t>(ci)]);
                        }
                    }
                    outFrames.push_back(composite.createCopy());
                    if (disposal == 2) { juce::Graphics cg(composite); cg.setColour(juce::Colours::white); cg.fillRect(ix, iy, iw, ih); }
                    else if (disposal == 3 && saved.isValid()) composite = saved;
                }
                transIdx = -1; disposal = 0;
            }
            else break;
        }
        return outFrames;
    }
}

ClassicWinLookAndFeel::ClassicWinLookAndFeel()
{
    setColour(juce::Label::textColourId, kWinDark);
    setColour(juce::ComboBox::backgroundColourId, kWinLight);
    setColour(juce::ComboBox::textColourId, kWinDark);
    setColour(juce::ComboBox::outlineColourId, kWinShadow);
    setColour(juce::PopupMenu::backgroundColourId, kWinSilver);
    setColour(juce::PopupMenu::textColourId, kWinDark);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, kWinNavy);
    setColour(juce::PopupMenu::highlightedTextColourId, kWinLight);
}

juce::Font ClassicWinLookAndFeel::getClassicFont(float size, bool bold)
{
    return juce::Font(juce::FontOptions("Tahoma", size, bold ? juce::Font::bold : juce::Font::plain));
}

void ClassicWinLookAndFeel::drawRaisedBevel(juce::Graphics& g, juce::Rectangle<int> b, bool depressed)
{
    if (b.getWidth() < 4 || b.getHeight() < 4) return;
    const auto tlOut = depressed ? kWinDark : kWinLight, tlIn = depressed ? kWinShadow : kWinHighlight;
    const auto brOut = depressed ? kWinLight : kWinDark, brIn = depressed ? kWinHighlight : kWinShadow;

    g.setColour(tlOut); g.fillRect(b.getX(), b.getY(), b.getWidth() - 1, 1); g.fillRect(b.getX(), b.getY(), 1, b.getHeight() - 1);
    g.setColour(brOut); g.fillRect(b.getX(), b.getBottom() - 1, b.getWidth(), 1); g.fillRect(b.getRight() - 1, b.getY(), 1, b.getHeight());
    g.setColour(tlIn);  g.fillRect(b.getX() + 1, b.getY() + 1, b.getWidth() - 3, 1); g.fillRect(b.getX() + 1, b.getY() + 1, 1, b.getHeight() - 3);
    g.setColour(brIn);  g.fillRect(b.getX() + 1, b.getBottom() - 2, b.getWidth() - 2, 1); g.fillRect(b.getRight() - 2, b.getY() + 1, 1, b.getHeight() - 2);
}

void ClassicWinLookAndFeel::drawSunkenBevel(juce::Graphics& g, juce::Rectangle<int> b)
{
    if (b.getWidth() < 4 || b.getHeight() < 4) return;
    g.setColour(kWinShadow);    g.fillRect(b.getX(), b.getY(), b.getWidth() - 1, 1); g.fillRect(b.getX(), b.getY(), 1, b.getHeight() - 1);
    g.setColour(kWinLight);     g.fillRect(b.getX(), b.getBottom() - 1, b.getWidth(), 1); g.fillRect(b.getRight() - 1, b.getY(), 1, b.getHeight());
    g.setColour(kWinDark);      g.fillRect(b.getX() + 1, b.getY() + 1, b.getWidth() - 3, 1); g.fillRect(b.getX() + 1, b.getY() + 1, 1, b.getHeight() - 3);
    g.setColour(kWinHighlight); g.fillRect(b.getX() + 1, b.getBottom() - 2, b.getWidth() - 2, 1); g.fillRect(b.getRight() - 2, b.getY() + 1, 1, b.getHeight() - 2);
}

void ClassicWinLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button, const juce::Colour&, bool hl, bool down)
{
    const auto b = button.getLocalBounds();
    const bool tog = button.getToggleState();
    if (button.getProperties().getWithDefault("flatMenu", false))
    {
        g.setColour(kWinSilver); g.fillRect(b);
        if (down) drawSunkenBevel(g, b); else if (hl) drawRaisedBevel(g, b, false);
        return;
    }
    g.setColour(tog ? juce::Colour(0xFFD6D6D6) : kWinSilver);
    g.fillRect(b);
    drawRaisedBevel(g, b, down || tog);
}

void ClassicWinLookAndFeel::drawButtonText(juce::Graphics& g, juce::TextButton& button, bool, bool down)
{
    g.setFont(getClassicFont(11.0f, button.getProperties().getWithDefault("boldText", false)));
    g.setColour(button.isEnabled() ? kWinDark : kWinShadow);
    auto area = button.getLocalBounds();
    if (down || button.getToggleState()) area.translate(1, 1);
    g.drawText(button.getButtonText(), area, juce::Justification::centred, false);
}

void ClassicWinLookAndFeel::drawToggleButton(juce::Graphics& g, juce::ToggleButton& button, bool, bool)
{
    const auto b = button.getLocalBounds();
    const juce::Rectangle<int> box(2, (b.getHeight() - 13) / 2, 13, 13);
    g.setColour(kWinLight); g.fillRect(box);
    drawSunkenBevel(g, box);
    if (button.getToggleState())
    {
        g.setColour(kWinDark);
        juce::Path chk;
        const float cx = static_cast<float>(box.getX()), cy = static_cast<float>(box.getY());
        chk.startNewSubPath(cx + 3.0f, cy + 6.5f); chk.lineTo(cx + 5.5f, cy + 9.5f); chk.lineTo(cx + 10.0f, cy + 3.5f);
        g.strokePath(chk, juce::PathStrokeType(1.8f));
    }
    g.setFont(getClassicFont(11.0f, false));
    g.setColour(kWinDark);
    g.drawText(button.getButtonText(), box.getRight() + 5, 0, b.getWidth() - box.getRight() - 5, b.getHeight(), juce::Justification::centredLeft, false);
}

void ClassicWinLookAndFeel::drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height,
                                             float sliderPos, float, float, juce::Slider::SliderStyle, juce::Slider&)
{
    const juce::Rectangle<int> track(x + 4, y + (height - 4) / 2, juce::jmax(8, width - 8), 4);
    g.setColour(kWinSilver); g.fillRect(track); drawSunkenBevel(g, track);
    const int tx = juce::jlimit(x, x + width - 10, static_cast<int>(sliderPos) - 5);
    const juce::Rectangle<int> thumb(tx, y + (height - 16) / 2, 10, 16);
    g.setColour(kWinSilver); g.fillRect(thumb); drawRaisedBevel(g, thumb, false);
}

void ClassicWinLookAndFeel::drawComboBox(juce::Graphics& g, int w, int h, bool down, int bx, int by, int bw, int bh, juce::ComboBox&)
{
    g.setColour(kWinLight); g.fillRect(0, 0, w, h); drawSunkenBevel(g, { 0, 0, w, h });
    const juce::Rectangle<int> arr(bx + 1, by + 2, bw - 3, bh - 4);
    g.setColour(kWinSilver); g.fillRect(arr); drawRaisedBevel(g, arr, down);
    const float cx = static_cast<float>(arr.getCentreX()), cy = static_cast<float>(arr.getCentreY());
    juce::Path tri; tri.addTriangle(cx - 3.5f, cy - 1.5f, cx + 3.5f, cy - 1.5f, cx, cy + 2.5f);
    g.setColour(kWinDark); g.fillPath(tri);
}

void ClassicWinLookAndFeel::drawPopupMenuBackground(juce::Graphics& g, int w, int h)
{
    g.setColour(kWinSilver); g.fillRect(0, 0, w, h); drawRaisedBevel(g, { 0, 0, w, h }, false);
}

void ClassicWinLookAndFeel::drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area, bool isSep, bool isAct,
                                              bool isHl, bool isTick, bool hasSub, const juce::String& text,
                                              const juce::String& keyText, const juce::Drawable*, const juce::Colour*)
{
    if (isSep)
    {
        auto s = area.reduced(4, 0);
        g.setColour(kWinShadow); g.fillRect(s.getX(), s.getCentreY(), s.getWidth(), 1);
        g.setColour(kWinLight);  g.fillRect(s.getX(), s.getCentreY() + 1, s.getWidth(), 1);
        return;
    }
    auto r = area.reduced(2, 1);
    if (isHl && isAct) { g.setColour(kWinNavy); g.fillRect(r); g.setColour(kWinLight); }
    else g.setColour(isAct ? kWinDark : kWinShadow);
    g.setFont(getClassicFont(11.0f, false));
    if (isTick)
    {
        juce::Path tick;
        const float tx = static_cast<float>(r.getX() + 5), ty = static_cast<float>(r.getCentreY());
        tick.startNewSubPath(tx, ty); tick.lineTo(tx + 2.5f, ty + 3.0f); tick.lineTo(tx + 6.5f, ty - 3.0f);
        g.strokePath(tick, juce::PathStrokeType(1.6f));
    }
    g.drawText(text, r.withTrimmedLeft(18).withTrimmedRight(8), juce::Justification::centredLeft, true);
    if (keyText.isNotEmpty()) g.drawText(keyText, r.withTrimmedRight(8), juce::Justification::centredRight, false);
    if (hasSub)
    {
        const float ax = static_cast<float>(r.getRight() - 8), ay = static_cast<float>(r.getCentreY());
        juce::Path arr; arr.addTriangle(ax - 3.0f, ay - 3.5f, ax - 3.0f, ay + 3.5f, ax + 1.5f, ay); g.fillPath(arr);
    }
}

EchoplasmAudioProcessorEditor::ToolIconButton::ToolIconButton(IconKind kind, const juce::String& tip)
    : juce::Button(tip), iconKind(kind) { setTooltip(tip); }

void EchoplasmAudioProcessorEditor::ToolIconButton::paintButton(juce::Graphics& g, bool, bool down)
{
    const auto b = getLocalBounds();
    const bool act = getToggleState();
    g.setColour(act ? juce::Colour(0xFFE2E2E2) : kWinSilver); g.fillRect(b);
    if (act)
    {
        g.setColour(juce::Colour(0xFFC8C8C8));
        for (int py = b.getY() + 2; py < b.getBottom() - 2; py += 2)
            for (int px = b.getX() + 2 + ((py / 2) % 2); px < b.getRight() - 2; px += 2)
                g.fillRect(px, py, 1, 1);
    }
    ClassicWinLookAndFeel::drawRaisedBevel(g, b, act || down);

    auto ia = b.reduced(5).toFloat();
    if (act || down) ia.translate(1.0f, 1.0f);
    const float x = ia.getX(), y = ia.getY(), w = ia.getWidth(), h = ia.getHeight();
    g.setColour(kWinDark);

    switch (iconKind)
    {
        case IconKind::Pencil:
        {
            juce::Path p; p.startNewSubPath(x + w * 0.15f, y + h * 0.85f); p.lineTo(x + w * 0.25f, y + h * 0.58f);
            p.lineTo(x + w * 0.72f, y + h * 0.12f); p.lineTo(x + w * 0.88f, y + h * 0.28f); p.lineTo(x + w * 0.42f, y + h * 0.75f); p.closeSubPath();
            g.setColour(juce::Colour(0xFFFFD43B)); g.fillPath(p);
            g.setColour(kWinDark); g.strokePath(p, juce::PathStrokeType(1.2f)); g.fillRect(x + w * 0.12f, y + h * 0.82f, 2.5f, 2.5f);
            break;
        }
        case IconKind::Brush:
            g.setColour(juce::Colour(0xFF8C5A2B)); g.drawLine(x + w * 0.85f, y + h * 0.15f, x + w * 0.42f, y + h * 0.58f, 2.8f);
            g.setColour(kWinDark); g.fillEllipse(x + w * 0.15f, y + h * 0.55f, w * 0.34f, h * 0.32f);
            break;
        case IconKind::Eraser:
        {
            juce::Path p; p.startNewSubPath(x + w * 0.12f, y + h * 0.65f); p.lineTo(x + w * 0.55f, y + h * 0.20f);
            p.lineTo(x + w * 0.88f, y + h * 0.42f); p.lineTo(x + w * 0.45f, y + h * 0.87f); p.closeSubPath();
            g.setColour(juce::Colour(0xFFFFB3C1)); g.fillPath(p);
            g.setColour(kWinDark); g.strokePath(p, juce::PathStrokeType(1.2f)); g.drawLine(x + w * 0.34f, y + h * 0.42f, x + w * 0.66f, y + h * 0.64f, 1.0f);
            break;
        }
        case IconKind::Bucket:
        {
            juce::Path cup; cup.startNewSubPath(x + w * 0.28f, y + h * 0.35f); cup.lineTo(x + w * 0.65f, y + h * 0.18f);
            cup.lineTo(x + w * 0.85f, y + h * 0.56f); cup.lineTo(x + w * 0.48f, y + h * 0.74f); cup.closeSubPath();
            g.setColour(juce::Colour(0xFFA8B5C8)); g.fillPath(cup);
            g.setColour(kWinDark); g.strokePath(cup, juce::PathStrokeType(1.2f));
            juce::Path spill; spill.startNewSubPath(x + w * 0.28f, y + h * 0.36f); spill.lineTo(x + w * 0.15f, y + h * 0.52f);
            spill.lineTo(x + w * 0.15f, y + h * 0.88f); spill.lineTo(x + w * 0.26f, y + h * 0.88f); spill.lineTo(x + w * 0.30f, y + h * 0.46f); spill.closeSubPath();
            g.setColour(juce::Colour(0xFF2874F0)); g.fillPath(spill);
            break;
        }
        case IconKind::Eyedropper:
            g.drawLine(x + w * 0.20f, y + h * 0.80f, x + w * 0.62f, y + h * 0.38f, 2.2f);
            g.setColour(juce::Colour(0xFF404040)); g.fillEllipse(x + w * 0.56f, y + h * 0.12f, w * 0.32f, h * 0.32f);
            g.setColour(kWinDark); g.fillRect(x + w * 0.12f, y + h * 0.82f, 2.5f, 2.5f);
            break;
        case IconKind::Rectangle:
            g.drawRect(x + w * 0.14f, y + h * 0.22f, w * 0.72f, h * 0.56f, 1.4f);
            break;
        case IconKind::Line:
            g.drawLine(x + w * 0.15f, y + h * 0.82f, x + w * 0.85f, y + h * 0.18f, 1.8f);
            break;
        case IconKind::Trash:
            g.drawRect(x + w * 0.25f, y + h * 0.32f, w * 0.50f, h * 0.55f, 1.2f);
            g.fillRect(x + w * 0.18f, y + h * 0.22f, w * 0.64f, 2.0f);
            g.fillRect(x + w * 0.38f, y + h * 0.12f, w * 0.24f, 2.0f);
            g.drawLine(x + w * 0.40f, y + h * 0.42f, x + w * 0.40f, y + h * 0.78f, 1.0f);
            g.drawLine(x + w * 0.60f, y + h * 0.42f, x + w * 0.60f, y + h * 0.78f, 1.0f);
            break;
    }
}

EchoplasmAudioProcessorEditor::SwatchButton::SwatchButton(int idx)
    : juce::Button("Swatch " + juce::String(idx)), swatchIndex(idx) {}

void EchoplasmAudioProcessorEditor::SwatchButton::paintButton(juce::Graphics& g, bool, bool)
{
    const auto b = getLocalBounds();
    g.setColour(EchoplasmAudioProcessor::swatchToColour(swatchIndex));
    g.fillRect(b.reduced(2));
    if (getToggleState()) { g.setColour(kWinDark); g.drawRect(b, 2); g.setColour(kWinLight); g.drawRect(b.reduced(2), 1); }
    else ClassicWinLookAndFeel::drawSunkenBevel(g, b);
}

EchoplasmAudioProcessorEditor::EchoCanvasComponent::EchoCanvasComponent(EchoplasmAudioProcessorEditor& o)
    : owner(o) { setRepaintsOnMouseActivity(true); }

juce::Rectangle<int> EchoplasmAudioProcessorEditor::EchoCanvasComponent::getGridBounds() const
{
    const int lw = 62, th = 34;
    const int gw = (juce::jmax(64, getWidth() - lw - 8) / 16) * 16;
    const int gh = (juce::jmax(64, getHeight() - th - 6) / 16) * 16;
    return { lw, th, gw, gh };
}

bool EchoplasmAudioProcessorEditor::EchoCanvasComponent::hitTestCell(juce::Point<int> pos, int& outB, int& outT) const
{
    const auto gb = getGridBounds();
    if (!gb.contains(pos)) return false;
    outT = juce::jlimit(0, 15, static_cast<int>(static_cast<float>(pos.x - gb.getX()) / (gb.getWidth() / 16.0f)));
    outB = juce::jlimit(0, 15, static_cast<int>(static_cast<float>(pos.y - gb.getY()) / (gb.getHeight() / 16.0f)));
    return true;
}

int EchoplasmAudioProcessorEditor::EchoCanvasComponent::quantizeCoord(int idx) const
{
    const int step = juce::jmax(1, owner.resolutionStep);
    return (idx / step) * step;
}

void EchoplasmAudioProcessorEditor::EchoCanvasComponent::applyBrushAt(
    EchoplasmAudioProcessor::FrameGrid& grid, int cb, int ct, int sw) const
{
    const int eff = (owner.currentTool == DrawTool::Brush || owner.currentTool == DrawTool::Eraser)
                        ? juce::jmax(1, owner.brushSize) : (owner.currentTool == DrawTool::Pencil ? 1 : owner.brushSize);
    const int step = juce::jmax(1, owner.resolutionStep), rad = eff - 1;
    for (int db = -rad; db <= rad; ++db)
    {
        for (int dt = -rad; dt <= rad; ++dt)
        {
            if (owner.brushShape == BrushShape::Circle && (db * db + dt * dt) > (rad * rad + 1)) continue;
            if (owner.brushShape == BrushShape::Diamond && (std::abs(db) + std::abs(dt)) > rad) continue;
            const int b = cb + db, t = ct + dt;
            if (b >= 0 && b < 16 && t >= 0 && t < 16)
            {
                const int qb = quantizeCoord(b), qt = quantizeCoord(t);
                for (int sb = qb; sb < juce::jmin(16, qb + step); ++sb)
                    for (int st = qt; st < juce::jmin(16, qt + step); ++st)
                        grid[static_cast<size_t>(sb)][static_cast<size_t>(st)] = sw;
            }
        }
    }
}

void EchoplasmAudioProcessorEditor::EchoCanvasComponent::applyLineToGrid(
    EchoplasmAudioProcessor::FrameGrid& grid, int b0, int t0, int b1, int t1, int sw) const
{
    int x0 = t0, y0 = b0;
    const int dx = std::abs(t1 - x0), sx = (x0 < t1) ? 1 : -1;
    const int dy = -std::abs(b1 - y0), sy = (y0 < b1) ? 1 : -1;
    int err = dx + dy;
    while (true)
    {
        applyBrushAt(grid, y0, x0, sw);
        if (x0 == t1 && y0 == b1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void EchoplasmAudioProcessorEditor::EchoCanvasComponent::applyRectToGrid(
    EchoplasmAudioProcessor::FrameGrid& grid, int b0, int t0, int b1, int t1, int sw) const
{
    const int minB = juce::jlimit(0, 15, juce::jmin(b0, b1)), maxB = juce::jlimit(0, 15, juce::jmax(b0, b1));
    const int minT = juce::jlimit(0, 15, juce::jmin(t0, t1)), maxT = juce::jlimit(0, 15, juce::jmax(t0, t1));
    for (int b = minB; b <= maxB; ++b)
        for (int t = minT; t <= maxT; ++t)
            grid[static_cast<size_t>(b)][static_cast<size_t>(t)] = sw;
}

void EchoplasmAudioProcessorEditor::EchoCanvasComponent::floodFillGrid(
    EchoplasmAudioProcessor::FrameGrid& grid, int b, int t, int target, int rep) const
{
    if (b < 0 || b >= 16 || t < 0 || t >= 16 || target == rep) return;
    if (grid[static_cast<size_t>(b)][static_cast<size_t>(t)] != target) return;
    grid[static_cast<size_t>(b)][static_cast<size_t>(t)] = rep;
    floodFillGrid(grid, b - 1, t, target, rep);
    floodFillGrid(grid, b + 1, t, target, rep);
    floodFillGrid(grid, b, t - 1, target, rep);
    floodFillGrid(grid, b, t + 1, target, rep);
}

void EchoplasmAudioProcessorEditor::EchoCanvasComponent::paint(juce::Graphics& g)
{
    const auto gb = getGridBounds();
    const auto& proc = owner.audioProcessor;
    auto grid = proc.getFrameCopy(proc.getActiveFrame());

    if (isDragging && (owner.currentTool == DrawTool::Rectangle || owner.currentTool == DrawTool::Line))
    {
        const int sw = erasingStroke ? EchoplasmAudioProcessor::SwatchWhite : owner.activeSwatch;
        grid = strokeSnapshot;
        if (owner.currentTool == DrawTool::Rectangle) applyRectToGrid(grid, dragStartBand, dragStartTap, dragCurrentBand, dragCurrentTap, sw);
        else applyLineToGrid(grid, dragStartBand, dragStartTap, dragCurrentBand, dragCurrentTap, sw);
    }

    g.setFont(ClassicWinLookAndFeel::getClassicFont(11.0f, true));
    g.setColour(kWinDark);
    juce::String statusLine = proc.isTempoSync()
        ? ("16 bands, 16 taps at 1/16 (" + juce::String(proc.getLastHostBpm(), 0) + " BPM)")
        : ("16 bands, 16 taps (Free " + juce::String(proc.getFreeTimeMs(), 0) + " ms)");
    if (hoverBand >= 0 && hoverTap >= 0)
    {
        const float pct = EchoplasmAudioProcessor::swatchToGain(grid[static_cast<size_t>(hoverBand)][static_cast<size_t>(hoverTap)]) * 100.0f;
        statusLine = EchoplasmAudioProcessor::getBandLabel(hoverBand) + " | Tap " + juce::String(hoverTap + 1) + "/16 | " + juce::String(pct, 0) + "%";
    }
    g.drawText(statusLine, gb.getX(), 2, gb.getWidth(), 14, juce::Justification::centredLeft, true);

    const float cw = static_cast<float>(gb.getWidth()) / 16.0f, ch = static_cast<float>(gb.getHeight()) / 16.0f;
    if (owner.showBeatMarkers)
    {
        g.setFont(ClassicWinLookAndFeel::getClassicFont(10.0f, false));
        const char* bl[4] = { "1 beat", "2 beats", "3 beats", "4 beats" };
        const int bc[4] = { 3, 7, 11, 15 };
        for (int i = 0; i < 4; ++i)
        {
            const int cx = gb.getX() + static_cast<int>(static_cast<float>(bc[i]) * cw);
            g.drawText(bl[i], cx - 18, gb.getY() - 16, static_cast<int>(cw) + 36, 14, juce::Justification::centred, false);
        }
    }

    g.setFont(ClassicWinLookAndFeel::getClassicFont(10.0f, false));
    for (int b = 0; b < 16; ++b)
        g.drawText(EchoplasmAudioProcessor::getBandLabel(b), 2, gb.getY() + static_cast<int>(b * ch), gb.getX() - 6, juce::jmax(10, static_cast<int>(ch)), juce::Justification::centredRight, false);

    ClassicWinLookAndFeel::drawSunkenBevel(g, gb.expanded(2));

    for (int b = 0; b < 16; ++b)
    {
        const int y0 = gb.getY() + static_cast<int>(std::round(b * ch)), y1 = gb.getY() + static_cast<int>(std::round((b + 1) * ch));
        for (int t = 0; t < 16; ++t)
        {
            const int x0 = gb.getX() + static_cast<int>(std::round(t * cw)), x1 = gb.getX() + static_cast<int>(std::round((t + 1) * cw));
            g.setColour(EchoplasmAudioProcessor::swatchToColour(grid[static_cast<size_t>(b)][static_cast<size_t>(t)]));
            g.fillRect(x0, y0, x1 - x0, y1 - y0);
        }
    }

    if (owner.showGridLines)
    {
        for (int i = 1; i < 16; ++i)
        {
            const int gx = gb.getX() + static_cast<int>(std::round(i * cw)), gy = gb.getY() + static_cast<int>(std::round(i * ch));
            g.setColour((i % 4 == 0) ? juce::Colour(0xFF9E9E9E) : juce::Colour(0xFFD6D6D6));
            g.fillRect(gx, gb.getY(), 1, gb.getHeight());
            g.setColour(juce::Colour(0xFFD6D6D6));
            g.fillRect(gb.getX(), gy, gb.getWidth(), 1);
        }
    }

    if (hoverBand >= 0 && hoverTap >= 0)
    {
        const int x0 = gb.getX() + static_cast<int>(std::round(hoverTap * cw)), x1 = gb.getX() + static_cast<int>(std::round((hoverTap + 1) * cw));
        const int y0 = gb.getY() + static_cast<int>(std::round(hoverBand * ch)), y1 = gb.getY() + static_cast<int>(std::round((hoverBand + 1) * ch));
        g.setColour(kWinNavy); g.drawRect(x0, y0, x1 - x0, y1 - y0, 1);
    }
}

void EchoplasmAudioProcessorEditor::EchoCanvasComponent::mouseDown(const juce::MouseEvent& e)
{
    int b = -1, t = -1;
    if (!hitTestCell(e.getPosition(), b, t)) return;
    const int cur = owner.audioProcessor.getActiveFrame();

    if (owner.currentTool == DrawTool::Eyedropper)
    {
        owner.activeSwatch = owner.audioProcessor.getCell(cur, b, t);
        for (auto& sw : owner.swatchButtons) sw->setToggleState(sw->getSwatchIndex() == owner.activeSwatch, juce::dontSendNotification);
        owner.repaint();
        return;
    }

    owner.pushUndoSnapshot();
    erasingStroke = e.mods.isRightButtonDown() || (owner.currentTool == DrawTool::Eraser);
    const int sw = erasingStroke ? EchoplasmAudioProcessor::SwatchWhite : owner.activeSwatch;

    if (owner.currentTool == DrawTool::Bucket)
    {
        auto grid = owner.audioProcessor.getFrameCopy(cur);
        floodFillGrid(grid, b, t, grid[static_cast<size_t>(b)][static_cast<size_t>(t)], sw);
        owner.audioProcessor.setFrameData(cur, grid);
        owner.repaint();
        return;
    }

    isDragging = true;
    dragStartBand = dragCurrentBand = b;
    dragStartTap = dragCurrentTap = t;
    strokeSnapshot = owner.audioProcessor.getFrameCopy(cur);

    if (owner.currentTool == DrawTool::Pencil || owner.currentTool == DrawTool::Brush || owner.currentTool == DrawTool::Eraser)
    {
        auto grid = strokeSnapshot;
        applyBrushAt(grid, b, t, sw);
        owner.audioProcessor.setFrameData(cur, grid);
    }
    owner.repaint();
}

void EchoplasmAudioProcessorEditor::EchoCanvasComponent::mouseDrag(const juce::MouseEvent& e)
{
    if (!isDragging) return;
    const auto gb = getGridBounds();
    const int t = juce::jlimit(0, 15, static_cast<int>(static_cast<float>(e.x - gb.getX()) / (gb.getWidth() / 16.0f)));
    const int b = juce::jlimit(0, 15, static_cast<int>(static_cast<float>(e.y - gb.getY()) / (gb.getHeight() / 16.0f)));
    hoverBand = dragCurrentBand = b;
    hoverTap = dragCurrentTap = t;

    const int cur = owner.audioProcessor.getActiveFrame();
    const int sw = erasingStroke ? EchoplasmAudioProcessor::SwatchWhite : owner.activeSwatch;

    if (owner.currentTool == DrawTool::Pencil || owner.currentTool == DrawTool::Brush || owner.currentTool == DrawTool::Eraser)
    {
        auto grid = owner.audioProcessor.getFrameCopy(cur);
        applyLineToGrid(grid, dragStartBand, dragStartTap, b, t, sw);
        owner.audioProcessor.setFrameData(cur, grid);
        dragStartBand = b; dragStartTap = t;
    }
    owner.repaint();
}

void EchoplasmAudioProcessorEditor::EchoCanvasComponent::mouseUp(const juce::MouseEvent&)
{
    if (!isDragging) return;
    const int cur = owner.audioProcessor.getActiveFrame();
    const int sw = erasingStroke ? EchoplasmAudioProcessor::SwatchWhite : owner.activeSwatch;
    if (owner.currentTool == DrawTool::Rectangle || owner.currentTool == DrawTool::Line)
    {
        auto grid = strokeSnapshot;
        if (owner.currentTool == DrawTool::Rectangle) applyRectToGrid(grid, dragStartBand, dragStartTap, dragCurrentBand, dragCurrentTap, sw);
        else applyLineToGrid(grid, dragStartBand, dragStartTap, dragCurrentBand, dragCurrentTap, sw);
        owner.audioProcessor.setFrameData(cur, grid);
    }
    isDragging = false;
    owner.repaint();
}

void EchoplasmAudioProcessorEditor::EchoCanvasComponent::mouseMove(const juce::MouseEvent& e)
{
    int b = -1, t = -1;
    if (hitTestCell(e.getPosition(), b, t)) { if (b != hoverBand || t != hoverTap) { hoverBand = b; hoverTap = t; repaint(); } }
    else if (hoverBand != -1 || hoverTap != -1) { hoverBand = hoverTap = -1; repaint(); }
}

void EchoplasmAudioProcessorEditor::EchoCanvasComponent::mouseExit(const juce::MouseEvent&)
{
    hoverBand = hoverTap = -1; repaint();
}

EchoplasmAudioProcessorEditor::PreviewThumbnailComponent::PreviewThumbnailComponent(EchoplasmAudioProcessor& proc) : processor(proc) {}

void EchoplasmAudioProcessorEditor::PreviewThumbnailComponent::paint(juce::Graphics& g)
{
    const auto b = getLocalBounds();
    g.setColour(kWinLight); g.fillRect(b);
    ClassicWinLookAndFeel::drawSunkenBevel(g, b);
    const auto inner = b.reduced(3);
    const auto grid = processor.getFrameCopy(processor.getActiveFrame());
    const float cw = static_cast<float>(inner.getWidth()) / 16.0f, ch = static_cast<float>(inner.getHeight()) / 16.0f;
    for (int r = 0; r < 16; ++r)
    {
        const int y0 = inner.getY() + static_cast<int>(std::round(r * ch)), y1 = inner.getY() + static_cast<int>(std::round((r + 1) * ch));
        for (int c = 0; c < 16; ++c)
        {
            const int x0 = inner.getX() + static_cast<int>(std::round(c * cw)), x1 = inner.getX() + static_cast<int>(std::round((c + 1) * cw));
            g.setColour(EchoplasmAudioProcessor::swatchToColour(grid[static_cast<size_t>(r)][static_cast<size_t>(c)]));
            g.fillRect(x0, y0, x1 - x0, y1 - y0);
        }
    }
}

EchoplasmAudioProcessorEditor::SpectrumVisualizerComponent::SpectrumVisualizerComponent(EchoplasmAudioProcessor& proc) : processor(proc)
{
    sampleRing.fill(0.0f); fftWorkspace.fill(0.0f); smoothedMagnitudes.fill(0.0f);
}

void EchoplasmAudioProcessorEditor::SpectrumVisualizerComponent::updateSpectrum()
{
    const int avail = processor.getNumFifoAvailable();
    if (avail > 0)
    {
        const int n = processor.pullSamplesForFFT(pullScratch.data(), juce::jmin(kFftSize, avail));
        for (int i = 0; i < n; ++i) { sampleRing[static_cast<size_t>(ringWritePos)] = pullScratch[static_cast<size_t>(i)]; ringWritePos = (ringWritePos + 1) % kFftSize; }
    }
    for (int i = 0; i < kFftSize; ++i) fftWorkspace[static_cast<size_t>(i)] = sampleRing[static_cast<size_t>((ringWritePos + i) % kFftSize)];
    std::fill(fftWorkspace.begin() + kFftSize, fftWorkspace.end(), 0.0f);
    window.multiplyWithWindowingTable(fftWorkspace.data(), kFftSize);
    forwardFFT.performFrequencyOnlyForwardTransform(fftWorkspace.data());

    for (int bin = 0; bin < kNumDisplayBins; ++bin)
    {
        const int f0 = juce::jlimit(1, kFftSize / 2 - 1, static_cast<int>(std::pow(static_cast<float>(kFftSize / 2), static_cast<float>(bin) / kNumDisplayBins)));
        const int f1 = juce::jlimit(f0 + 1, kFftSize / 2, static_cast<int>(std::pow(static_cast<float>(kFftSize / 2), static_cast<float>(bin + 1) / kNumDisplayBins)));
        float peak = 0.0f;
        for (int k = f0; k < f1; ++k) peak = juce::jmax(peak, fftWorkspace[static_cast<size_t>(k)]);
        const float db = juce::Decibels::gainToDecibels(peak / static_cast<float>(kFftSize * 0.25f), -68.0f);
        const float target = juce::jlimit(0.0f, 1.0f, (db + 68.0f) / 68.0f);
        const float cur = smoothedMagnitudes[static_cast<size_t>(bin)];
        smoothedMagnitudes[static_cast<size_t>(bin)] = cur + ((target > cur) ? 0.55f : 0.16f) * (target - cur);
    }
    repaint();
}

void EchoplasmAudioProcessorEditor::SpectrumVisualizerComponent::paint(juce::Graphics& g)
{
    const auto b = getLocalBounds();
    g.setColour(juce::Colour(0xFFF4F4FA)); g.fillRect(b);
    ClassicWinLookAndFeel::drawSunkenBevel(g, b);
    const auto inner = b.reduced(2);

    g.setColour(juce::Colour(0xFFE0DFF0));
    for (int gx = 1; gx < 8; ++gx) g.fillRect(inner.getX() + (inner.getWidth() * gx) / 8, inner.getY(), 1, inner.getHeight());
    for (int gy = 1; gy < 4; ++gy) g.fillRect(inner.getX(), inner.getY() + (inner.getHeight() * gy) / 4, inner.getWidth(), 1);

    juce::Path poly, stroke;
    poly.startNewSubPath(static_cast<float>(inner.getX()), static_cast<float>(inner.getBottom()));
    for (int i = 0; i < kNumDisplayBins; ++i)
    {
        const float fx = static_cast<float>(inner.getX()) + (static_cast<float>(i) / (kNumDisplayBins - 1)) * inner.getWidth();
        const float fy = static_cast<float>(inner.getBottom()) - smoothedMagnitudes[static_cast<size_t>(i)] * (inner.getHeight() - 3);
        poly.lineTo(fx, fy);
        if (i == 0) stroke.startNewSubPath(fx, fy); else stroke.lineTo(fx, fy);
    }
    poly.lineTo(static_cast<float>(inner.getRight()), static_cast<float>(inner.getBottom()));
    poly.closeSubPath();
    g.setColour(kSpecFill.withAlpha(0.75f)); g.fillPath(poly);
    g.setColour(kSpecStroke); g.strokePath(stroke, juce::PathStrokeType(1.4f));
}

EchoplasmAudioProcessorEditor::EchoplasmAudioProcessorEditor(EchoplasmAudioProcessor& p)
    : AudioProcessorEditor(&p), audioProcessor(p)
{
    setLookAndFeel(&classicLAF);
    setWantsKeyboardFocus(true);

    for (int i = 0; i < static_cast<int>(menuButtons.size()); ++i)
    {
        auto& btn = menuButtons[static_cast<size_t>(i)];
        btn.getProperties().set("flatMenu", true);
        btn.onClick = [this, i] { showMenuForIndex(i, &menuButtons[static_cast<size_t>(i)]); };
        addAndMakeVisible(btn);
    }

    opacityTitleLabel.setFont(ClassicWinLookAndFeel::getClassicFont(11.0f, false));
    addAndMakeVisible(opacityTitleLabel);
    opacitySlider.setSliderStyle(juce::Slider::LinearHorizontal);
    opacitySlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    addAndMakeVisible(opacitySlider);
    opacityAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(audioProcessor.apvts, "opacity", opacitySlider);
    opacityValueLabel.setFont(ClassicWinLookAndFeel::getClassicFont(11.0f, false));
    opacityValueLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(opacityValueLabel);

    sizeTitleLabel.setFont(ClassicWinLookAndFeel::getClassicFont(11.0f, false));
    addAndMakeVisible(sizeTitleLabel);
    for (int s = 1; s <= 4; ++s) sizeCombo.addItem(juce::String(s), s);
    sizeCombo.setSelectedId(1, juce::dontSendNotification);
    sizeCombo.onChange = [this] { brushSize = sizeCombo.getSelectedId(); };
    addAndMakeVisible(sizeCombo);

    shapeTitleLabel.setFont(ClassicWinLookAndFeel::getClassicFont(11.0f, false));
    addAndMakeVisible(shapeTitleLabel);
    shapeCombo.addItem("Square", 1); shapeCombo.addItem("Circle", 2); shapeCombo.addItem("Diamond", 3);
    shapeCombo.setSelectedId(1, juce::dontSendNotification);
    shapeCombo.onChange = [this] { brushShape = static_cast<BrushShape>(juce::jlimit(0, 2, shapeCombo.getSelectedId() - 1)); };
    addAndMakeVisible(shapeCombo);

    colorTitleLabel.setFont(ClassicWinLookAndFeel::getClassicFont(11.0f, false));
    addAndMakeVisible(colorTitleLabel);

    powerToggleButton.setClickingTogglesState(true);
    powerToggleButton.getProperties().set("boldText", true);
    powerToggleButton.onClick = [this]
    {
        if (auto* bp = audioProcessor.apvts.getParameter("bypass"))
        {
            bp->beginChangeGesture();
            bp->setValueNotifyingHost(powerToggleButton.getToggleState() ? 0.0f : 1.0f);
            bp->endChangeGesture();
        }
        updateReadoutsAndStates();
    };
    addAndMakeVisible(powerToggleButton);

    const std::array<std::pair<ToolIconButton::IconKind, const char*>, 7> defs = {{
        { ToolIconButton::IconKind::Pencil, "Pencil" }, { ToolIconButton::IconKind::Brush, "Brush" },
        { ToolIconButton::IconKind::Eraser, "Eraser" }, { ToolIconButton::IconKind::Bucket, "Paint Bucket (Flood Fill)" },
        { ToolIconButton::IconKind::Eyedropper, "Eyedropper" }, { ToolIconButton::IconKind::Rectangle, "Rectangle" },
        { ToolIconButton::IconKind::Line, "Line" }
    }};
    for (int i = 0; i < 7; ++i)
    {
        auto btn = std::make_unique<ToolIconButton>(defs[static_cast<size_t>(i)].first, defs[static_cast<size_t>(i)].second);
        btn->onClick = [this, i]
        {
            currentTool = static_cast<DrawTool>(i);
            for (int j = 0; j < 7; ++j) toolButtons[static_cast<size_t>(j)]->setToggleState(j == i, juce::dontSendNotification);
        };
        addAndMakeVisible(*btn);
        toolButtons[static_cast<size_t>(i)] = std::move(btn);
    }
    toolButtons[0]->setToggleState(true, juce::dontSendNotification);

    trashButton = std::make_unique<ToolIconButton>(ToolIconButton::IconKind::Trash, "Clear Active Frame");
    trashButton->onClick = [this] { pushUndoSnapshot(); audioProcessor.clearFrame(audioProcessor.getActiveFrame()); repaint(); };
    addAndMakeVisible(*trashButton);

    addAndMakeVisible(echoCanvas);
    for (int s = 0; s < EchoplasmAudioProcessor::kNumSwatches; ++s)
    {
        auto sw = std::make_unique<SwatchButton>(s);
        sw->onClick = [this, s]
        {
            activeSwatch = s;
            for (auto& b : swatchButtons) b->setToggleState(b->getSwatchIndex() == activeSwatch, juce::dontSendNotification);
            repaint();
        };
        addAndMakeVisible(*sw);
        swatchButtons[static_cast<size_t>(s)] = std::move(sw);
    }
    swatchButtons[static_cast<size_t>(activeSwatch)]->setToggleState(true, juce::dontSendNotification);

    auto setupSlider = [this](juce::Label& lbl, juce::Slider& sl, juce::Label& ro, const juce::String& pid,
                              std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>& att)
    {
        lbl.setFont(ClassicWinLookAndFeel::getClassicFont(11.0f, false)); addAndMakeVisible(lbl);
        sl.setSliderStyle(juce::Slider::LinearHorizontal); sl.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0); addAndMakeVisible(sl);
        att = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(audioProcessor.apvts, pid, sl);
        ro.setFont(ClassicWinLookAndFeel::getClassicFont(11.0f, false)); ro.setJustificationType(juce::Justification::centredRight); addAndMakeVisible(ro);
    };
    setupSlider(mixTitleLabel, mixSlider, mixValueLabel, "mix", mixAttachment);
    setupSlider(feedbackTitleLabel, feedbackSlider, feedbackValueLabel, "feedback", feedbackAttachment);
    setupSlider(outputTitleLabel, outputSlider, outputValueLabel, "output", outputAttachment);

    addAndMakeVisible(freezeCheckbox);
    freezeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(audioProcessor.apvts, "freeze", freezeCheckbox);
    addAndMakeVisible(previewThumbnail);

    addFrameButton.onClick = [this] { audioProcessor.addFrame(false); rebuildFrameButtons(); repaint(); };
    addAndMakeVisible(addFrameButton);
    removeFrameButton.onClick = [this] { audioProcessor.removeFrame(audioProcessor.getActiveFrame()); rebuildFrameButtons(); repaint(); };
    addAndMakeVisible(removeFrameButton);

    sequenceButton.setClickingTogglesState(true);
    sequenceButton.onClick = [this] { audioProcessor.setSequenceActive(sequenceButton.getToggleState()); repaint(); };
    addAndMakeVisible(sequenceButton);

    loopCheckbox.onClick = [this] { audioProcessor.setLoopActive(loopCheckbox.getToggleState()); };
    addAndMakeVisible(loopCheckbox);

    timeLabel.setFont(ClassicWinLookAndFeel::getClassicFont(11.0f, false));
    addAndMakeVisible(timeLabel);
    for (int t : { 60, 100, 150, 250, 500, 750, 1000 }) frameTimeCombo.addItem(juce::String(t) + " ms", t);
    frameTimeCombo.setSelectedId(250, juce::dontSendNotification);
    frameTimeCombo.onChange = [this] { if (frameTimeCombo.getSelectedId() > 0) audioProcessor.setFrameTimeMs(static_cast<float>(frameTimeCombo.getSelectedId())); };
    addAndMakeVisible(frameTimeCombo);

    addAndMakeVisible(spectrumVisualizer);
    rebuildFrameButtons();
    updateReadoutsAndStates();

    setSize(740, 540);
    setResizable(true, true);
    setResizeLimits(680, 500, 1200, 900);
    startTimerHz(30);
}

EchoplasmAudioProcessorEditor::~EchoplasmAudioProcessorEditor() { stopTimer(); setLookAndFeel(nullptr); }

void EchoplasmAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(kWinSilver);
    const int w = getWidth(), h = getHeight(), rightW = 228, rx = w - rightW - 6;

    g.setColour(kWinShadow); g.fillRect(0, 23, w, 1); g.fillRect(0, 52, w, 1);
    g.setColour(kWinLight);  g.fillRect(0, 24, w, 1); g.fillRect(0, 53, w, 1);

    const juce::Rectangle<int> activeCol(colorTitleLabel.getRight() + 4, 30, 18, 16);
    g.setColour(EchoplasmAudioProcessor::swatchToColour(activeSwatch)); g.fillRect(activeCol);
    ClassicWinLookAndFeel::drawSunkenBevel(g, activeCol);

    ClassicWinLookAndFeel::drawRaisedBevel(g, { 6, 58, 70, h - 144 }, false);
    ClassicWinLookAndFeel::drawRaisedBevel(g, { 82, 58, w - 82 - rightW - 12, h - 144 }, false);
    ClassicWinLookAndFeel::drawRaisedBevel(g, { rx, 58, rightW, 136 }, false);
    ClassicWinLookAndFeel::drawRaisedBevel(g, { rx, 200, rightW, h - 144 - 142 }, false);

    g.setFont(ClassicWinLookAndFeel::getClassicFont(11.0f, true)); g.setColour(kWinDark);
    g.drawText("Preview", rx + 8, 204, 80, 16, juce::Justification::centredLeft, false);
    g.setFont(ClassicWinLookAndFeel::getClassicFont(11.0f, false));
    g.drawText("Frame " + juce::String(audioProcessor.getActiveFrame() + 1) + " of " + juce::String(audioProcessor.getNumFrames()),
               rx + 80, 204, rightW - 88, 16, juce::Justification::centredRight, false);
}

void EchoplasmAudioProcessorEditor::resized()
{
    const int w = getWidth(), h = getHeight();
    int mx = 4;
    for (auto& btn : menuButtons) { const int bw = btn.getButtonText().length() * 7 + 16; btn.setBounds(mx, 2, bw, 20); mx += bw + 2; }

    opacityTitleLabel.setBounds(8, 29, 46, 18); opacitySlider.setBounds(56, 28, 92, 20); opacityValueLabel.setBounds(148, 29, 36, 18);
    sizeTitleLabel.setBounds(192, 29, 32, 18); sizeCombo.setBounds(226, 28, 46, 20);
    shapeTitleLabel.setBounds(280, 29, 40, 18); shapeCombo.setBounds(322, 28, 76, 20);
    colorTitleLabel.setBounds(406, 29, 36, 18); powerToggleButton.setBounds(w - 58, 28, 50, 20);

    for (int i = 0; i < 7; ++i) toolButtons[static_cast<size_t>(i)]->setBounds(12 + (i % 2) * 30, 66 + (i / 2) * 30, 28, 28);

    const int rightW = 228, centerX = 82, centerW = w - centerX - rightW - 12, bottomH = 78, bodyH = h - 58 - bottomH - 8;
    echoCanvas.setBounds(centerX + 4, 62, centerW - 8, bodyH - 36);

    const int swY = 62 + bodyH - 30;
    for (int s = 0; s < EchoplasmAudioProcessor::kNumSwatches; ++s)
        swatchButtons[static_cast<size_t>(s)]->setBounds(centerX + 10 + s * 24, swY + 2, 22, 22);
    trashButton->setBounds(centerX + centerW - 34, swY, 26, 26);

    const int rx = w - rightW - 6, slX = rx + 62, slW = rightW - 120, valX = rx + rightW - 56;
    mixTitleLabel.setBounds(rx + 8, 66, 52, 20); mixSlider.setBounds(slX, 66, slW, 20); mixValueLabel.setBounds(valX, 66, 50, 20);
    feedbackTitleLabel.setBounds(rx + 8, 94, 52, 20); feedbackSlider.setBounds(slX, 94, slW, 20); feedbackValueLabel.setBounds(valX, 94, 50, 20);
    outputTitleLabel.setBounds(rx + 8, 122, 52, 20); outputSlider.setBounds(slX, 122, slW, 20); outputValueLabel.setBounds(valX, 122, 50, 20);
    freezeCheckbox.setBounds(rx + 8, 156, 86, 20);

    const int pY = 200, pH = bodyH - 142, ctrlH = 66;
    const int tSz = juce::jmin(rightW - 24, juce::jmax(60, pH - 28 - ctrlH));
    previewThumbnail.setBounds(rx + (rightW - tSz) / 2, pY + 24, tSz, tSz);

    const int fbY1 = pY + pH - ctrlH + 4;
    int fx = rx + 8;
    for (auto& fb : frameSelectButtons)
    {
        if (fx + 20 <= rx + rightW - 54) { fb->setBounds(fx, fbY1, 18, 20); fb->setVisible(true); fx += 20; }
        else fb->setVisible(false);
    }
    addFrameButton.setBounds(rx + rightW - 46, fbY1, 18, 20);
    removeFrameButton.setBounds(rx + rightW - 26, fbY1, 18, 20);

    const int fbY2 = fbY1 + 26;
    sequenceButton.setBounds(rx + 8, fbY2, 62, 20);
    loopCheckbox.setBounds(rx + 74, fbY2, 50, 20);
    timeLabel.setBounds(rx + 126, fbY2, 30, 20);
    frameTimeCombo.setBounds(rx + 156, fbY2, 64, 20);
    spectrumVisualizer.setBounds(6, h - bottomH - 4, w - 12, bottomH);
}

bool EchoplasmAudioProcessorEditor::keyPressed(const juce::KeyPress& key)
{
    if (key.getModifiers().isCommandDown() || key.getModifiers().isCtrlDown())
    {
        if (key.getKeyCode() == 'Z' || key.getKeyCode() == 'z')
        {
            if (key.getModifiers().isShiftDown()) performRedo(); else performUndo();
            return true;
        }
        if (key.getKeyCode() == 'Y' || key.getKeyCode() == 'y') { performRedo(); return true; }
    }
    return false;
}

void EchoplasmAudioProcessorEditor::timerCallback()
{
    spectrumVisualizer.updateSpectrum();
    updateReadoutsAndStates();
    const int c = audioProcessor.getNumFrames(), a = audioProcessor.getActiveFrame();
    if (c != lastSyncedNumFrames) { rebuildFrameButtons(); repaint(); }
    else if (a != lastSyncedActiveFrame)
    {
        lastSyncedActiveFrame = a;
        for (int i = 0; i < static_cast<int>(frameSelectButtons.size()); ++i)
            frameSelectButtons[static_cast<size_t>(i)]->setToggleState(i == a, juce::dontSendNotification);
        repaint();
    }
}

void EchoplasmAudioProcessorEditor::updateReadoutsAndStates()
{
    opacityValueLabel.setText(juce::String(juce::roundToInt(opacitySlider.getValue())) + "%", juce::dontSendNotification);
    mixValueLabel.setText(juce::String(mixSlider.getValue(), 1) + " %", juce::dontSendNotification);
    feedbackValueLabel.setText(juce::String(feedbackSlider.getValue(), 1) + " %", juce::dontSendNotification);
    outputValueLabel.setText(juce::String(outputSlider.getValue(), 1) + " dB", juce::dontSendNotification);

    const bool isOn = !(audioProcessor.apvts.getRawParameterValue("bypass")->load() > 0.5f);
    powerToggleButton.setToggleState(isOn, juce::dontSendNotification);
    powerToggleButton.setButtonText(isOn ? "On" : "Off");
    sequenceButton.setToggleState(audioProcessor.isSequenceActive(), juce::dontSendNotification);
    loopCheckbox.setToggleState(audioProcessor.isLoopActive(), juce::dontSendNotification);
}

void EchoplasmAudioProcessorEditor::rebuildFrameButtons()
{
    const int count = audioProcessor.getNumFrames(), active = audioProcessor.getActiveFrame();
    for (auto& b : frameSelectButtons) removeChildComponent(b.get());
    frameSelectButtons.clear();
    for (int i = 0; i < count; ++i)
    {
        auto btn = std::make_unique<juce::TextButton>(juce::String(i + 1));
        btn->setToggleState(i == active, juce::dontSendNotification);
        btn->onClick = [this, i]
        {
            audioProcessor.setActiveFrame(i);
            for (int j = 0; j < static_cast<int>(frameSelectButtons.size()); ++j)
                frameSelectButtons[static_cast<size_t>(j)]->setToggleState(j == i, juce::dontSendNotification);
            lastSyncedActiveFrame = i; repaint();
        };
        addAndMakeVisible(*btn);
        frameSelectButtons.push_back(std::move(btn));
    }
    lastSyncedNumFrames = count; lastSyncedActiveFrame = active; resized();
}

void EchoplasmAudioProcessorEditor::pushUndoSnapshot()
{
    const int cur = audioProcessor.getActiveFrame();
    undoStack.push_back({ cur, audioProcessor.getFrameCopy(cur) });
    if (undoStack.size() > 40) undoStack.erase(undoStack.begin());
    redoStack.clear();
}

void EchoplasmAudioProcessorEditor::performUndo()
{
    if (undoStack.empty()) return;
    const auto e = undoStack.back(); undoStack.pop_back();
    redoStack.push_back({ e.frameIndex, audioProcessor.getFrameCopy(e.frameIndex) });
    audioProcessor.setFrameData(e.frameIndex, e.grid); audioProcessor.setActiveFrame(e.frameIndex); repaint();
}

void EchoplasmAudioProcessorEditor::performRedo()
{
    if (redoStack.empty()) return;
    const auto e = redoStack.back(); redoStack.pop_back();
    undoStack.push_back({ e.frameIndex, audioProcessor.getFrameCopy(e.frameIndex) });
    audioProcessor.setFrameData(e.frameIndex, e.grid); audioProcessor.setActiveFrame(e.frameIndex); repaint();
}

void EchoplasmAudioProcessorEditor::showMenuForIndex(int menuIndex, juce::Component* anchor)
{
    juce::PopupMenu m;
    if (menuIndex == 0) { m.addItem(101, "Import Image / GIF..."); m.addSeparator(); m.addItem(102, "Clear All Frames"); }
    else if (menuIndex == 1)
    {
        m.addItem(201, "Undo", !undoStack.empty()); m.addItem(202, "Redo", !redoStack.empty()); m.addSeparator();
        m.addItem(203, "Copy Frame"); m.addItem(204, "Paste Frame", hasClipboardGrid); m.addSeparator(); m.addItem(205, "Clear Canvas");
    }
    else if (menuIndex == 2)
    {
        m.addItem(301, "Invert Swatches"); m.addItem(302, "Flip Horizontal"); m.addItem(303, "Flip Vertical"); m.addItem(304, "Shift Right");
        m.addSeparator(); m.addItem(305, "Preset: Diagonal Echo"); m.addItem(306, "Preset: Spectral Cascade");
    }
    else if (menuIndex == 3)
    {
        m.addItem(401, "Sync to Host BPM (" + juce::String(audioProcessor.getLastHostBpm(), 0) + " BPM)", true, audioProcessor.isTempoSync());
        m.addSeparator();
        m.addItem(402, "Free Mode (1000 ms)", true, !audioProcessor.isTempoSync() && std::abs(audioProcessor.getFreeTimeMs() - 1000.0f) < 10.0f);
        m.addItem(403, "Free Mode (2000 ms)", true, !audioProcessor.isTempoSync() && std::abs(audioProcessor.getFreeTimeMs() - 2000.0f) < 10.0f);
        m.addItem(404, "Free Mode (4000 ms)", true, !audioProcessor.isTempoSync() && std::abs(audioProcessor.getFreeTimeMs() - 4000.0f) < 10.0f);
    }
    else if (menuIndex == 4)
    {
        m.addItem(501, "16x16 (1/16 beat)", true, resolutionStep == 1);
        m.addItem(502, "8x8 (1/8 beat)", true, resolutionStep == 2);
        m.addItem(503, "4x4 (Quarter beat)", true, resolutionStep == 4);
    }
    else if (menuIndex == 5)
    {
        m.addItem(601, "Show Grid Lines", true, showGridLines);
        m.addItem(602, "Show Beat Markers", true, showBeatMarkers);
        m.addSeparator(); m.addItem(603, "Reset Window Size");
    }
    else if (menuIndex == 6) m.addItem(701, "About Echoplasm...");

    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(anchor), [this](int res)
    {
        const int cur = audioProcessor.getActiveFrame();
        if (res == 101) openImageImportChooser();
        else if (res == 102) { pushUndoSnapshot(); audioProcessor.clearAllFrames(); rebuildFrameButtons(); repaint(); }
        else if (res == 201) performUndo();
        else if (res == 202) performRedo();
        else if (res == 203) { clipboardGrid = audioProcessor.getFrameCopy(cur); hasClipboardGrid = true; }
        else if (res == 204 && hasClipboardGrid) { pushUndoSnapshot(); audioProcessor.setFrameData(cur, clipboardGrid); repaint(); }
        else if (res == 205) { pushUndoSnapshot(); audioProcessor.clearFrame(cur); repaint(); }
        else if (res == 301)
        {
            pushUndoSnapshot(); auto g = audioProcessor.getFrameCopy(cur);
            for (auto& r : g) for (auto& c : r) c = (c == EchoplasmAudioProcessor::SwatchWhite) ? EchoplasmAudioProcessor::SwatchBlack : EchoplasmAudioProcessor::SwatchWhite;
            audioProcessor.setFrameData(cur, g); repaint();
        }
        else if (res == 302) { pushUndoSnapshot(); auto g = audioProcessor.getFrameCopy(cur); for (auto& r : g) std::reverse(r.begin(), r.end()); audioProcessor.setFrameData(cur, g); repaint(); }
        else if (res == 303) { pushUndoSnapshot(); auto g = audioProcessor.getFrameCopy(cur); std::reverse(g.begin(), g.end()); audioProcessor.setFrameData(cur, g); repaint(); }
        else if (res == 304) { pushUndoSnapshot(); auto g = audioProcessor.getFrameCopy(cur); for (auto& r : g) std::rotate(r.rbegin(), r.rbegin() + 1, r.rend()); audioProcessor.setFrameData(cur, g); repaint(); }
        else if (res == 305)
        {
            pushUndoSnapshot(); EchoplasmAudioProcessor::FrameGrid g {};
            for (auto& r : g) r.fill(EchoplasmAudioProcessor::SwatchWhite);
            for (int i = 0; i < 16; ++i)
                g[static_cast<size_t>(15 - i)][static_cast<size_t>(i)] = (i < 4) ? EchoplasmAudioProcessor::SwatchBlack : (i < 8) ? EchoplasmAudioProcessor::SwatchRed : (i < 12) ? EchoplasmAudioProcessor::SwatchOrange : EchoplasmAudioProcessor::SwatchYellow;
            audioProcessor.setFrameData(cur, g); repaint();
        }
        else if (res == 306)
        {
            pushUndoSnapshot(); EchoplasmAudioProcessor::FrameGrid g {};
            for (auto& r : g) r.fill(EchoplasmAudioProcessor::SwatchWhite);
            for (int b = 0; b < 16; ++b) for (int t = 0; t < 16; ++t) if ((b + t) % 4 == 0)
                g[static_cast<size_t>(b)][static_cast<size_t>(t)] = (t < 4) ? EchoplasmAudioProcessor::SwatchRed : (t < 10) ? EchoplasmAudioProcessor::SwatchOrange : EchoplasmAudioProcessor::SwatchYellow;
            audioProcessor.setFrameData(cur, g); repaint();
        }
        else if (res == 401) { audioProcessor.setTempoSync(true); repaint(); }
        else if (res == 402 || res == 403 || res == 404) { audioProcessor.setTempoSync(false); audioProcessor.setFreeTimeMs(res == 402 ? 1000.0f : (res == 403 ? 2000.0f : 4000.0f)); repaint(); }
        else if (res == 501 || res == 502 || res == 503) resolutionStep = (res == 501 ? 1 : (res == 502 ? 2 : 4));
        else if (res == 601) { showGridLines = !showGridLines; repaint(); }
        else if (res == 602) { showBeatMarkers = !showBeatMarkers; repaint(); }
        else if (res == 603) setSize(740, 540);
        else if (res == 701)
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "About Echoplasm",
                "Echoplasm Spectral Flipbook Delay (AU)\n16-Band Logarithmic SVF Filterbank x 16-Tap Delay Matrix", "OK", this);
    });
}

void EchoplasmAudioProcessorEditor::openImageImportChooser()
{
    activeFileChooser = std::make_unique<juce::FileChooser>(
        "Import Image or Animated GIF",
        juce::File::getSpecialLocation(juce::File::userPicturesDirectory),
        "*.png;*.jpg;*.jpeg;*.gif;*.bmp");

    activeFileChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this](const juce::FileChooser& chooser)
        {
            const auto file = chooser.getResult();
            if (file.existsAsFile()) importImageFile(file);
        });
}

int EchoplasmAudioProcessorEditor::mapColourToSwatch(juce::Colour c, bool invert) const
{
    if (c.getAlpha() < 40) return invert ? EchoplasmAudioProcessor::SwatchBlack : EchoplasmAudioProcessor::SwatchWhite;
    if (invert) c = juce::Colour::fromFloatRGBA(1.0f - c.getFloatRed(), 1.0f - c.getFloatGreen(), 1.0f - c.getFloatBlue(), 1.0f);
    if (c.getPerceivedBrightness() > 0.90f && c.getSaturation() < 0.18f) return EchoplasmAudioProcessor::SwatchWhite;

    int best = EchoplasmAudioProcessor::SwatchWhite;
    float bestDist = 1e9f;
    for (int s = 0; s < EchoplasmAudioProcessor::kNumSwatches; ++s)
    {
        const auto ref = EchoplasmAudioProcessor::swatchToColour(s);
        const float dr = c.getFloatRed() - ref.getFloatRed(), dg = c.getFloatGreen() - ref.getFloatGreen(), db = c.getFloatBlue() - ref.getFloatBlue();
        const float d = dr * dr * 0.30f + dg * dg * 0.59f + db * db * 0.11f;
        if (d < bestDist) { bestDist = d; best = s; }
    }
    return best;
}

EchoplasmAudioProcessor::FrameGrid EchoplasmAudioProcessorEditor::downsampleImageToGrid(const juce::Image& img, bool invert) const
{
    EchoplasmAudioProcessor::FrameGrid grid {};
    for (auto& r : grid) r.fill(EchoplasmAudioProcessor::SwatchWhite);
    if (!img.isValid() || img.getWidth() <= 0 || img.getHeight() <= 0) return grid;

    const int sw = img.getWidth(), sh = img.getHeight();
    for (int b = 0; b < 16; ++b)
    {
        const int y0 = (b * sh) / 16, y1 = juce::jmax(y0 + 1, ((b + 1) * sh) / 16);
        for (int t = 0; t < 16; ++t)
        {
            const int x0 = (t * sw) / 16, x1 = juce::jmax(x0 + 1, ((t + 1) * sw) / 16);
            float sr = 0, sg = 0, sb = 0, sa = 0; int cnt = 0;
            for (int py = y0; py < y1; ++py)
                for (int px = x0; px < x1; ++px)
                {
                    const auto c = img.getPixelAt(px, py);
                    sr += c.getFloatRed(); sg += c.getFloatGreen(); sb += c.getFloatBlue(); sa += c.getFloatAlpha(); ++cnt;
                }
            if (cnt > 0)
            {
                const float inv = 1.0f / static_cast<float>(cnt);
                grid[static_cast<size_t>(b)][static_cast<size_t>(t)] = mapColourToSwatch(juce::Colour::fromFloatRGBA(sr * inv, sg * inv, sb * inv, sa * inv), invert);
            }
        }
    }
    return grid;
}

void EchoplasmAudioProcessorEditor::importImageFile(const juce::File& file)
{
    std::vector<juce::Image> loaded;
    if (file.getFileExtension().equalsIgnoreCase(".gif")) loaded = decodeGifFrames(file);
    if (loaded.empty()) { const auto img = juce::ImageFileFormat::loadFrom(file); if (img.isValid()) loaded.push_back(img); }
    if (loaded.empty())
    {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Import Image", "Could not decode the selected image file.", "OK", this);
        return;
    }

    pushUndoSnapshot();
    if (loaded.size() == 1)
    {
        audioProcessor.setFrameData(audioProcessor.getActiveFrame(), downsampleImageToGrid(loaded[0], false));
    }
    else
    {
        const int target = juce::jmin(EchoplasmAudioProcessor::kMaxFrames, static_cast<int>(loaded.size()));
        audioProcessor.setNumFrames(target);
        for (int f = 0; f < target; ++f)
        {
            const size_t idx = (static_cast<size_t>(f) * loaded.size()) / static_cast<size_t>(target);
            audioProcessor.setFrameData(f, downsampleImageToGrid(loaded[idx], false));
        }
        audioProcessor.setActiveFrame(0);
        audioProcessor.setLoopActive(true);
        audioProcessor.setSequenceActive(true);
    }
    rebuildFrameButtons();
    repaint();
}
