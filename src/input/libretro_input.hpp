#pragma once
// Input module, part 3: libretro glue. Reads the frontend's devices into RawInput, defines
// the input core options and the input descriptors for each scheme.

#include "input/mapper.hpp"
#include "libretro/libretro.h"

#include <cstdlib>
#include <cstring>
#include <span>

namespace gromada::input {

// Core options (libretro v0 "Description; default|value|..."). The core splices these
// into its own SET_VARIABLES table.
inline constexpr retro_variable coreOptions[] = {
    {"gromada_input_scheme", "Schema di controllo; Cursore virtuale|Diretto (stick sinistro muove, destro mira)"},
    {"gromada_cursor_speed", "Velocita' cursore (pixel/s); 500|600|700|800|1000|300|400"},
    {"gromada_cursor_accel", "Accelerazione cursore (stick in fondo); 2x|3x|4x|1x (nessuna)|1.5x"},
    {"gromada_cursor_precision", "Precisione stick; Normale|Alta|Lineare"},
    {"gromada_pointer", "Touchscreen e mouse; Entrambi|Solo touchscreen|Solo mouse|Disattivati"},
    {"gromada_keyboard", "Tastiera fisica; Disattivata|Attiva"},
    {"gromada_camera", "Telecamera; Automatica (segue il giocatore nello schema diretto e col touch)|Segue sempre il giocatore|Originale (scorrimento ai bordi)"},
    {"gromada_aim_assist", "Mira assistita (schema diretto); Attiva|Disattivata"},
};

// getVar(key) -> value or nullptr (the core's RETRO_ENVIRONMENT_GET_VARIABLE wrapper).
template <class GetVar>
Settings readSettings(GetVar&& getVar) {
    Settings s;
    auto is = [](const char* v, const char* prefix) { return std::strncmp(v, prefix, std::strlen(prefix)) == 0; };
    if (const char* v = getVar("gromada_input_scheme")) s.scheme = is(v, "Diretto") ? Scheme::Direct : Scheme::VirtualCursor;
    if (const char* v = getVar("gromada_cursor_speed")) s.cursorSpeed = std::clamp(static_cast<float>(std::atof(v)), 100.0f, 2000.0f);
    if (const char* v = getVar("gromada_cursor_accel")) s.acceleration = std::clamp(static_cast<float>(std::atof(v)), 1.0f, 8.0f);
    if (const char* v = getVar("gromada_cursor_precision")) s.curve = is(v, "Alta") ? 3.0f : is(v, "Lineare") ? 1.0f : 2.0f;
    if (const char* v = getVar("gromada_pointer")) {
        s.touch = is(v, "Entrambi") || is(v, "Solo touch");
        s.mouse = is(v, "Entrambi") || is(v, "Solo mouse");
    }
    if (const char* v = getVar("gromada_keyboard")) s.keyboard = is(v, "Attiva");
    if (const char* v = getVar("gromada_camera"))
        s.camera = is(v, "Segue sempre") ? CameraFollow::Always : is(v, "Originale") ? CameraFollow::Off : CameraFollow::Auto;
    if (const char* v = getVar("gromada_aim_assist")) s.aimAssist = !is(v, "Disattivata");
    return s;
}

// Input descriptors (labels shown by RetroArch in Controls/Remap), per scheme.
// Labels say what the action is in the game; the PC equivalent is in brackets.
inline std::span<const retro_input_descriptor> inputDescriptors(Scheme scheme) {
#define GROMADA_PAD(id, text) {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_##id, text}
#define GROMADA_STICK(side, axis, text) \
    {0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_##side, RETRO_DEVICE_ID_ANALOG_##axis, text}
#define GROMADA_COMMON                                                                  \
    GROMADA_PAD(L, "Arma precedente (tasti 1-4)"),                                      \
    GROMADA_PAD(R, "Arma successiva (tasti 1-4)"),                                      \
    GROMADA_PAD(X, "Invio"),                                                            \
    GROMADA_PAD(START, "Menu (Esc)"),                                                   \
    GROMADA_PAD(SELECT, "Schermo informazioni (TAB)"),                                  \
    GROMADA_PAD(UP, "Avanti (freccia su)"), GROMADA_PAD(DOWN, "Indietro (freccia giu')"), \
    GROMADA_PAD(LEFT, "Gira a sinistra (freccia)"), GROMADA_PAD(RIGHT, "Gira a destra (freccia)")
    static constexpr retro_input_descriptor cursor[] = {
        GROMADA_COMMON,
        GROMADA_PAD(A, "Muovi / conferma (click sinistro)"),
        GROMADA_PAD(B, "Attacca (click destro)"),
        GROMADA_STICK(LEFT, X, "Cursore (orizzontale)"),
        GROMADA_STICK(LEFT, Y, "Cursore (verticale)"),
        GROMADA_STICK(RIGHT, X, "Cursore lento, preciso (orizzontale)"),
        GROMADA_STICK(RIGHT, Y, "Cursore lento, preciso (verticale)"),
    };
    static constexpr retro_input_descriptor direct[] = {
        GROMADA_COMMON,
        GROMADA_PAD(A, "Vai al punto mirato / conferma (click sinistro)"),
        GROMADA_PAD(B, "Spara al punto mirato (click destro)"),
        GROMADA_STICK(LEFT, X, "Guida (ordini di movimento, orizzontale)"),
        GROMADA_STICK(LEFT, Y, "Guida (ordini di movimento, verticale)"),
        GROMADA_STICK(RIGHT, X, "Mira, in fondo spara (orizzontale)"),
        GROMADA_STICK(RIGHT, Y, "Mira, in fondo spara (verticale)"),
    };
#undef GROMADA_COMMON
#undef GROMADA_STICK
#undef GROMADA_PAD
    if (scheme == Scheme::Direct) return direct;
    return cursor;
}

// Reads every device the settings enable. Port 0 only (single player game).
inline RawInput pollRaw(retro_input_state_t state, const Settings& s) {
    RawInput in;
    if (!state)
        return in;
    for (unsigned id = 0; id < pad::Count; ++id)
        in.set(id, state(0, RETRO_DEVICE_JOYPAD, 0, id) != 0);

    auto axis = [&](unsigned index, unsigned id) {
        return std::clamp(state(0, RETRO_DEVICE_ANALOG, index, id) / 32767.0f, -1.0f, 1.0f);
    };
    in.leftStick = {axis(RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X),
                    axis(RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y)};
    in.rightStick = {axis(RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X),
                     axis(RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y)};

    if (s.touch) {
        // [-0x7fff, 0x7fff] across the game image (RetroArch maps the viewport, so the black
        // bars of a 4:3 picture on a 16:9 screen are outside). A finger off the picture reads
        // as not pressed (x/y = -0x8000 on Android): skip it, the others still count.
        for (unsigned i = 0; i < in.touches.size(); ++i) {
            if (!state(0, RETRO_DEVICE_POINTER, i, RETRO_DEVICE_ID_POINTER_PRESSED))
                continue;
            const int x = state(0, RETRO_DEVICE_POINTER, i, RETRO_DEVICE_ID_POINTER_X);
            const int y = state(0, RETRO_DEVICE_POINTER, i, RETRO_DEVICE_ID_POINTER_Y);
            if (x == -0x8000 || y == -0x8000)
                continue;
            auto norm = [](int v) { return std::clamp((v + 0x7fff) / float(2 * 0x7fff), 0.0f, 1.0f); };
            in.touches[static_cast<std::size_t>(in.touchCount++)] = {norm(x), norm(y)};
        }
    }
    if (s.mouse) {
        in.mouseDelta = {static_cast<float>(state(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X)),
                         static_cast<float>(state(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y))};
        in.mouseLeft = state(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT) != 0;
        in.mouseRight = state(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_RIGHT) != 0;
        in.mouseWheel = (state(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_WHEELUP) ? 1 : 0)
                      - (state(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_WHEELDOWN) ? 1 : 0);
    }
    if (s.keyboard) {
        auto key = [&](unsigned k) { return state(0, RETRO_DEVICE_KEYBOARD, 0, k) != 0; };
        static constexpr unsigned keyCodes[KeyCount] = {RETROK_1,  RETROK_2,     RETROK_3,   RETROK_4,
                                                       RETROK_LEFT, RETROK_RIGHT, RETROK_UP, RETROK_DOWN,
                                                       RETROK_TAB, RETROK_ESCAPE, RETROK_RETURN, RETROK_BACKSPACE};
        for (std::size_t k = 0; k < KeyCount; ++k)
            in.keys[k] = key(keyCodes[k]);
        in.keys[static_cast<std::size_t>(Key::Enter)] |= key(RETROK_KP_ENTER);
        for (unsigned c = 'a'; c <= 'z'; ++c) in.chars[c] = key(c);
        for (unsigned c = '0'; c <= '9'; ++c) in.chars[c] = key(c);
        in.chars[' '] = key(RETROK_SPACE);
        in.chars['-'] = key(RETROK_MINUS);
        in.shift = key(RETROK_LSHIFT) || key(RETROK_RSHIFT);
    }
    return in;
}

} // namespace gromada::input
