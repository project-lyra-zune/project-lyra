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
 * Engine.cs
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
using System.Reflection;
using System.Runtime.InteropServices;

namespace ZuneBoards.DevelopmentFront.NativeAppLauncher.HD
{
    unsafe static class Engine
    {
        static readonly uint[] _ipl = new uint[] {
            0xE59F4010, 0xE59F0004, 0xE59FE004, 0xE12FFF14
        };

        delegate uint MarshalCopyDelegate(void* destination, void* source, int count);

        // ----------------- DO NOT ALTER THE FOLLOWING CODE -----------------
        // IT DEPENDS ON HARDCODED STACK AND INSTRUCTION OFFSETS, WHICH CAN BE
        // AFFECTED BY SUBTLE CODE CHANGES
        // -------------------------------------------------------------------
        public static void Execute(void* target, void* arg)
        {
            uint[] arr = new uint[7];
            Buffer.BlockCopy(_ipl, 0, arr, 0, 16);
            void*** ptr = (void***)GCHandle.Alloc(new void*[1], GCHandleType.Pinned).AddrOfPinnedObject();
#if DEBUG
            void** rvx = (void**)(&target - 15);
#else
            void** rvx = (void**)(&target - 16);
#endif
            void** ipl = (void**)GCHandle.Alloc(arr, GCHandleType.Pinned).AddrOfPinnedObject();
            memcpy(ptr, rvx, 4);
            ipl[4] = arg;
            ipl[5] = *ptr + 0x34;
            ipl[6] = target;
            *ptr = ipl;
            memcpy(rvx, ptr, 4);
            *ptr = *ptr;
        }
        static void* memcpy(void* dest, void* src, int count)
        {
            if (dest == null)
                throw new ArgumentNullException("dest");
            if (src == null)
                throw new ArgumentNullException("src");
            if (count == 0)
                throw new ArgumentOutOfRangeException("count");
            Type type = Type.GetType("Microsoft.Xna.Framework.GamerServices.ZuneKernelMethods+Marshal, Microsoft.Xna.Framework");
            MethodInfo method = type.GetMethod("MarhalCopy", BindingFlags.Static | BindingFlags.Public); // [sic]
            MarshalCopyDelegate dgate = (MarshalCopyDelegate)Delegate.CreateDelegate(typeof(MarshalCopyDelegate), null, method);
            dgate(dest, src, count);
            return dest;
        }
    }
}