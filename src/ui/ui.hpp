#pragma once
// UI module: bitmap text with the game's fonts, on-screen texts with handles, menu screens
// (.men + MenuCreate objects, including the format-7 lens) and localized texts.
// Formats and hypotheses: docs/ui.md. Header-only; include once per binary (libgromada rule).

#include "ui/font.hpp"
#include "ui/game_texts.hpp"
#include "ui/lens.hpp"
#include "ui/menu.hpp"
#include "ui/text_encoding.hpp"
#include "ui/text_layout.hpp"
#include "ui/text_manager.hpp"

namespace gromada::ui {

// Draws menu objects and texts the way the exe interleaves them: layer by layer, each text in
// its font's z layer (14 for V_FONT, above the menu labels in 13). Texts whose font layer is
// not a menu layer are drawn last.
inline void drawUi(const MenuScreen& menu, const TextManager& texts, const FontSet& fonts,
                   FramebufferRef fb, int originX = 0, int originY = 0) {
    menu.draw(fb, originX, originY, [&](int layer) { texts.draw(fonts, fb, originX, originY, layer); });
    for (const auto& t : texts.texts()) {
        const Vid* v = fonts.vid(t.font);
        if (!v || v->z_layer >= MenuLayerCount)
            fonts.drawText(t.bytes, t.font, t.x + originX, t.y + originY, fb, {.maxBytes = t.visible});
    }
}

} // namespace gromada::ui
