/*  _____                         ________   ____    __  __
 * /\  __`\                      /\_____  \ /\  _`\ /\ \/\ \
 * \ \ \/\ \  _____     __    ___\/____//'/'\ \ \/\ \ \ \/'/'
 *  \ \ \ \ \/\ '__`\ /'__`\/' _ `\   //'/'  \ \ \ \ \ \ , <
 *   \ \ \_\ \ \ \L\ \\  __//\ \/\ \ //'/'___ \ \ \_\ \ \ \\`\
 *    \ \_____\ \ ,__/ \____\ \_\ \_\/\_______\\ \____/\ \_\ \_\
 *     \/_____/\ \ \/ \/____/\/_/\/_/\/_______/ \/___/  \/_/\/_/
 *              \ \_\
 *               \/_/ OpenZDK Release 1 | 2010-04-14
 *
 * PayloadRunner.cs
 * Copyright (c) 2010 itsnotabigtruck.
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

using System;
using System.Runtime.InteropServices;

namespace ZuneBoards.DevelopmentFront.NativeAppLauncher.HD
{
    unsafe static class PayloadRunner
    {
        static readonly uint[] _shellcode = {
            0xE92D4030, 0xE24DD014, 0xE59F5060, 0xE3A01000,
            0xE3A02000, 0xE3A03000, 0xE3A04000, 0xE58D4000,
            0xE58D4004, 0xE58D4008, 0xE58D400C, 0xE58D4010,
            0xE24D4014, 0xE58D4014, 0xE1A0E00F, 0xE12FFF15,
            0xE59F502C, 0xE3A00001, 0xE24D1014, 0xE3A02000,
            0xE3E03000, 0xE24D4004, 0xE58D4000, 0xE1A0E00F,
            0xE12FFF15, 0xE3A00000, 0xE28DD014, 0xE8BD8030,
            0xF101FFF4, 0xF101FF98
        };

        public static void Launch(string payload)
        {
            // gotta validate those args before you hack
            if (string.IsNullOrEmpty(payload))
                throw new ArgumentException("A payload must be specified");
            // launch shellcode
            void* target = (void*)GCHandle.Alloc(_shellcode, GCHandleType.Pinned).AddrOfPinnedObject();
            void* arg = (char*)GCHandle.Alloc(payload, GCHandleType.Pinned).AddrOfPinnedObject() + 4;
            Engine.Execute(target, arg);
        }
    }
}