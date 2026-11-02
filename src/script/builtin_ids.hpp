#pragma once
// IDs of the builtins declared by the game in maps/HEADER.INI (`extern Name(params) ID;`).
//
// The script's extern declarations decide which opcode a name compiles to; what an opcode does
// is fixed by the original's handler (0x412b30). This table gives the handlers' parameter
// counts (how many values the interpreter pops for a builtin opcode) and lets the engine
// dispatch on symbolic IDs. tests/script_data_test.cpp checks that it matches the header
// shipped with the game.
//
// Parameter signatures: one letter per parameter, 'i' int, 's' string, 'a' untyped
// (`FWrite(int file,variable)`). StopSFX is declared twice: StopSFX() 134 and StopSFX(int) 136;
// the first declaration wins, so 136 is unreachable from the game scripts.
#include <array>
#include <cstdint>
#include <string_view>

#include "script/builtins.hpp"

namespace gromada::script {

enum class BuiltinId : std::int32_t {
    Load = 64,
    SizeTo = 65,
    Print = 66,
    Action = 67,
    Create = 68,
    GetTime = 69,
    PlayAVI = 70,
    LoadMenu = 71,
    FreeMenu = 72,
    ScrollLock = 73,
    MenuVid = 74,
    MenuDir = 75,
    Exit = 76,
    PlayMidi = 77,
    StopMusic = 78,
    MaxUnit = 79,
    Effect = 80,
    SetGraphDetail = 81,
    SetSound = 82,
    SetMusic = 83,
    SetCursor = 84,
    Flagman = 85,
    GetObject = 86,
    AddCommand = 87,
    GetKey = 88,
    Pause = 89,
    Random = 90,
    PlaySFX = 91,
    SetGraphMode = 92,
    GetInputState = 93,
    GetUnitVid = 94,
    FirstUnit = 95,
    NextUnit = 96,
    PlayAudio = 97,
    E_InsertTerrain = 98,
    FileCopy = 99,
    Message = 100,
    MenuAction = 101,
    MenuCreate = 102,
    strlen = 103,
    StateBar = 104,
    MenuSelectVid = 105,
    SetGamma = 106,
    SetWind = 107,
    CreateLight = 108,
    GetX = 109,
    GetY = 110,
    FWrite = 111,
    FRead = 112,
    FOpen = 113,
    FClose = 114,
    FCreate = 115,
    PrintText = 116,
    PrintFile = 117,
    DeleteText = 118,
    SetFlagman = 119,
    Save = 120,
    GetCommands = 121,
    SetCommands = 122,
    AskPlace = 123,
    GetVidName = 124,
    SaveDemo = 125,
    SetPosition = 126,
    SetVid = 127,
    SetAutoScroll = 128,
    GetNetInfo = 129,
    Send = 130,
    Receive = 131,
    SetNet = 132,
    GetNetKey = 133,
    StopSFX = 134,
    mciSendString = 135,
    StopSFXOne = 136,
    PlayWAV = 137,
    GetNetInputState = 138,
    GetReg = 139,
};

struct KnownBuiltin {
    BuiltinId id;
    std::string_view name;
    std::string_view params;
};

inline constexpr std::array<KnownBuiltin, 76> kKnownBuiltins{{
    {BuiltinId::Load, "Load", "s"},
    {BuiltinId::SizeTo, "SizeTo", "iii"},
    {BuiltinId::Print, "Print", "i"},
    {BuiltinId::Action, "Action", "iiii"},
    {BuiltinId::Create, "Create", "iiii"},
    {BuiltinId::GetTime, "GetTime", ""},
    {BuiltinId::PlayAVI, "PlayAVI", "s"},
    {BuiltinId::LoadMenu, "LoadMenu", "s"},
    {BuiltinId::FreeMenu, "FreeMenu", ""},
    {BuiltinId::ScrollLock, "ScrollLock", "i"},
    {BuiltinId::MenuVid, "MenuVid", ""},
    {BuiltinId::MenuDir, "MenuDir", ""},
    {BuiltinId::Exit, "Exit", ""},
    {BuiltinId::PlayMidi, "PlayMidi", "s"},
    {BuiltinId::StopMusic, "StopMusic", ""},
    {BuiltinId::MaxUnit, "MaxUnit", "ii"},
    {BuiltinId::Effect, "Effect", "i"},
    {BuiltinId::SetGraphDetail, "SetGraphDetail", "i"},
    {BuiltinId::SetSound, "SetSound", "i"},
    {BuiltinId::SetMusic, "SetMusic", "i"},
    {BuiltinId::SetCursor, "SetCursor", "i"},
    {BuiltinId::Flagman, "Flagman", "i"},
    {BuiltinId::GetObject, "GetObject", "iii"},
    {BuiltinId::AddCommand, "AddCommand", "iiii"},
    {BuiltinId::GetKey, "GetKey", ""},
    {BuiltinId::Pause, "Pause", "i"},
    {BuiltinId::Random, "Random", "i"},
    {BuiltinId::PlaySFX, "PlaySFX", "i"},
    {BuiltinId::SetGraphMode, "SetGraphMode", "i"},
    {BuiltinId::GetInputState, "GetInputState", ""},
    {BuiltinId::GetUnitVid, "GetUnitVid", "i"},
    {BuiltinId::FirstUnit, "FirstUnit", "ii"},
    {BuiltinId::NextUnit, "NextUnit", ""},
    {BuiltinId::PlayAudio, "PlayAudio", "iiii"},
    {BuiltinId::E_InsertTerrain, "E_InsertTerrain", "iiii"},
    {BuiltinId::FileCopy, "FileCopy", "ss"},
    {BuiltinId::Message, "Message", "s"},
    {BuiltinId::MenuAction, "MenuAction", "iii"},
    {BuiltinId::MenuCreate, "MenuCreate", "iiii"},
    {BuiltinId::strlen, "strlen", "s"},
    {BuiltinId::StateBar, "StateBar", "i"},
    {BuiltinId::MenuSelectVid, "MenuSelectVid", ""},
    {BuiltinId::SetGamma, "SetGamma", "i"},
    {BuiltinId::SetWind, "SetWind", "ii"},
    {BuiltinId::CreateLight, "CreateLight", "iiii"},
    {BuiltinId::GetX, "GetX", "i"},
    {BuiltinId::GetY, "GetY", "i"},
    {BuiltinId::FWrite, "FWrite", "ia"},
    {BuiltinId::FRead, "FRead", "i"},
    {BuiltinId::FOpen, "FOpen", "s"},
    {BuiltinId::FClose, "FClose", "i"},
    {BuiltinId::FCreate, "FCreate", "s"},
    {BuiltinId::PrintText, "PrintText", "iisi"},
    {BuiltinId::PrintFile, "PrintFile", "iisi"},
    {BuiltinId::DeleteText, "DeleteText", "i"},
    {BuiltinId::SetFlagman, "SetFlagman", "ii"},
    {BuiltinId::Save, "Save", "s"},
    {BuiltinId::GetCommands, "GetCommands", "i"},
    {BuiltinId::SetCommands, "SetCommands", "is"},
    {BuiltinId::AskPlace, "AskPlace", "iiii"},
    {BuiltinId::GetVidName, "GetVidName", "i"},
    {BuiltinId::SaveDemo, "SaveDemo", "s"},
    {BuiltinId::SetPosition, "SetPosition", "ii"},
    {BuiltinId::SetVid, "SetVid", "iii"},
    {BuiltinId::SetAutoScroll, "SetAutoScroll", "i"},
    {BuiltinId::GetNetInfo, "GetNetInfo", "i"},
    {BuiltinId::Send, "Send", "i"},
    {BuiltinId::Receive, "Receive", ""},
    {BuiltinId::SetNet, "SetNet", "i"},
    {BuiltinId::GetNetKey, "GetNetKey", "i"},
    {BuiltinId::StopSFX, "StopSFX", ""},
    {BuiltinId::mciSendString, "mciSendString", "s"},
    {BuiltinId::StopSFXOne, "StopSFX", "i"},
    {BuiltinId::PlayWAV, "PlayWAV", "s"},
    {BuiltinId::GetNetInputState, "GetNetInputState", "i"},
    {BuiltinId::GetReg, "GetReg", "ss"},
}};

constexpr std::int32_t toInt(BuiltinId id) { return static_cast<std::int32_t>(id); }

// Opcodes with a handler in the original (64..139). Every other opcode that is not a VM
// instruction is an "Unknown command" that ends the frame.
constexpr bool isBuiltinOpcode(std::int32_t op) { return op >= 64 && op <= 139; }

constexpr const KnownBuiltin* findKnownBuiltin(std::int32_t id) {
    if (!isBuiltinOpcode(id)) return nullptr;
    const KnownBuiltin& k = kKnownBuiltins[std::size_t(id - 64)];
    return toInt(k.id) == id ? &k : nullptr;
}

// Number of values the original's handler pops for builtin opcode `id` (0 if unknown).
constexpr std::size_t knownBuiltinArity(std::int32_t id) {
    const KnownBuiltin* k = findKnownBuiltin(id);
    return k ? k->params.size() : 0;
}

// Registry holding every builtin of kKnownBuiltins (parameters unnamed).
inline BuiltinRegistry knownBuiltinRegistry() {
    BuiltinRegistry r;
    for (const auto& k : kKnownBuiltins) {
        BuiltinInfo info;
        info.id = toInt(k.id);
        info.name = std::string{k.name};
        for (const char c : k.params) {
            ParamDecl p;
            p.type = c == 'i' ? ParamType::Int : c == 's' ? ParamType::String : ParamType::Any;
            info.params.push_back(p);
        }
        r.add(std::move(info));
    }
    return r;
}

} // namespace gromada::script
