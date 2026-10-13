/*
    Copyright 2016-2026 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#ifndef NETPLAYINPUT_H
#define NETPLAYINPUT_H

#include "types.h"

namespace melonDS::RollbackNet
{

// One player's input for one frame, as exchanged between computers: 4 bytes.
//
//  bits  0-11  buttons, 1 = pressed, in melonDS's order:
//              A B Select Start Right Left Up Down R L X Y
//  bit  12     the stylus touches the screen
//  bits 16-23  touch X (0-255)
//  bits 24-31  touch Y (0-191)
//
// The lid, the microphone and the GBA slot are not part of the input: the lid
// stays open and the microphone hears silence, so they cannot desync.
struct PlayerInput
{
    u32 Bits = 0;

    static constexpr u32 kButtonMask = 0xFFF;
    static constexpr u32 kTouching = 1u << 12;

    static PlayerInput FromFrontend(u32 melonInputMask, bool touching, u16 x, u16 y)
    {
        // melonDS's input mask is active low (0 = pressed)
        PlayerInput in;
        in.Bits = (~melonInputMask) & kButtonMask;
        if (touching)
        {
            if (x > 255) x = 255;
            if (y > 191) y = 191;
            in.Bits |= kTouching | (u32(x) << 16) | (u32(y) << 24);
        }
        return in;
    }

    u32 KeyMask() const { return (~Bits) & kButtonMask; }
    bool Touching() const { return Bits & kTouching; }
    u16 TouchX() const { return (Bits >> 16) & 0xFF; }
    u16 TouchY() const { return (Bits >> 24) & 0xFF; }

    // drops anything outside the format, so that a peer cannot make two
    // computers read the same input differently
    PlayerInput Sanitized() const
    {
        PlayerInput in;
        in.Bits = Bits & (kButtonMask | kTouching);
        if (Touching())
        {
            u16 y = TouchY();
            if (y > 191) y = 191;
            in.Bits |= (u32(TouchX()) << 16) | (u32(y) << 24);
        }
        return in;
    }
};

}

#endif // NETPLAYINPUT_H
