#pragma once
// Input module (issue #4): controller schemes, virtual cursor, touchscreen/mouse/keyboard,
// original .CUR cursors. Header-only; depends only on the standard library and libretro.h.
//
//   gromada::input::Mapper     RawInput -> PcState (mouse/keyboard state of the PC game)
//   gromada::input::pollRaw    libretro devices -> RawInput
//   gromada::input::CursorSet  cursores/*.CUR, drawn with drawCursor()
// Usage per frame:  state = mapper.update(pollRaw(input_state_cb, settings), frameInfo);
// See docs/controls.md for the mappings and their rationale.

#include "input/cursor_file.hpp"
#include "input/libretro_input.hpp"
#include "input/mapper.hpp"
#include "input/pc_state.hpp"
