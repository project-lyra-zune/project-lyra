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
 * Program.cs
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

using System.Runtime.InteropServices;

namespace ZuneBoards.DevelopmentFront.NativeAppLauncher.HD
{
    static class Program
    {
        const string _payload = @"\gametitle\584E07D1\Content\nativeapp.exe";

        static void Main(string[] args)
        {
            PayloadRunner.Launch(_payload);
        }
    }
}