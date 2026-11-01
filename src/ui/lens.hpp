#pragma once
// Graphics format 7: the "lens" of the menu monitor (fw.res nvid 134, "Linza for interface
// monitor", 640x480, one frame). It carries no colours: it redraws what is already on screen,
// squeezing the screen into the oval monitor of gamemenu.men (fish-eye effect over the paused
// level). Decoded from gromada.exe 0x404330 (see docs/ui.md):
//
//   u16 firstRow, u16 rowCount                         (rows relative to the sprite top)
//   rowCount x { s16 centerX, s16 n, u8 step[n], u8 rowStep }
//
// For output row r (= top + firstRow + i) the exe builds a temporary line of 2n-1 pixels
// symmetric around its centre: with off_0 = 0 and off_{k+1} = off_k + step[k],
//   tmp[n-1-k] = src[srcRow][srcCX - off_k],  tmp[n-1+k] = src[srcRow][srcCX + off_k]
// and copies it at columns left + centerX - n ... left + centerX + n - 2 (the centre pixel lands
// on column centerX - 1). srcCX = left + width/2; srcRow starts at top + firstRow and grows by
// rowStep after each row. The last step[] value only moves past the line (never sampled).
// Source and destination are the same framebuffer: the exe works in place, which is safe
// because srcRow >= r for every row of the data (rows above are never read again).

#include "core/std.hpp"
#include "libgromada/libgromada_all.hpp"

namespace gromada::ui {

// Applies a format-7 frame whose sprite box has its top-left corner at (left, top).
// Returns false when the data is malformed (nothing past the error is drawn).
inline bool applyLensFrame(const VidGraphics::Frame& frame, int left, int top, FramebufferRef fb) {
    SpanStreamReader reader = frame.read();
    const auto bytesLeft = [&] { return reader.bytesRemaining(); };
    if (frame.data.size() < 4)
        return false;
    const int firstRow = reader.read<std::uint16_t>();
    const int rowCount = reader.read<std::uint16_t>();
    const int fbWidth = fb.extent(1);
    const int fbHeight = fb.extent(0);

    // exe: the source window starts at the first lens row; if the whole sprite does not fit
    // below it, it is pulled up (and falls back to firstRow when even that does not fit).
    int srcRow = top + firstRow;
    if (srcRow + frame.height() - 1 > fbHeight - 1) {
        const int pulled = fbHeight - 1 - frame.height() - 1;
        srcRow = pulled >= 0 ? pulled : firstRow;
    }
    const int srcCX = left + frame.width() / 2;

    std::vector<RGBA8> tmp;
    for (int i = 0; i < rowCount; ++i) {
        if (bytesLeft() < 4)
            return false;
        const int centerX = reader.read<std::int16_t>();
        const int n = reader.read<std::int16_t>();
        if (n < 0 || bytesLeft() < static_cast<std::size_t>(n) + 1)
            return false;
        const auto steps = reader.read_bytes(n);
        const int rowStep = reader.read<std::uint8_t>();
        const int dstRow = top + firstRow + i;

        if (n > 0 && dstRow >= 0 && dstRow < fbHeight && srcRow >= 0 && srcRow < fbHeight) {
            tmp.assign(2 * static_cast<std::size_t>(n) - 1, RGBA8{});
            std::vector<bool> valid(tmp.size(), false);
            int off = 0;
            for (int k = 0; k < n; ++k) {
                const int right = srcCX + off, leftX = srcCX - off;
                if (right >= 0 && right < fbWidth) { tmp[n - 1 + k] = fb[srcRow, right]; valid[n - 1 + k] = true; }
                if (leftX >= 0 && leftX < fbWidth) { tmp[n - 1 - k] = fb[srcRow, leftX]; valid[n - 1 - k] = true; }
                off += std::to_integer<int>(steps[k]);
            }
            const int dst0 = left + centerX - n;
            for (int j = 0; j < static_cast<int>(tmp.size()); ++j) {
                const int dx = dst0 + j;
                if (valid[j] && dx >= 0 && dx < fbWidth)
                    fb[dstRow, dx] = tmp[j];
            }
        }
        srcRow += rowStep;
    }
    return true;
}

} // namespace gromada::ui
