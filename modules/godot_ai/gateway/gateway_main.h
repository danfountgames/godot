/**************************************************************************/
/*  gateway_main.h                                                        */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

// `godot --godot-ai-stdio`: the MCP stdio gateway that used to be a separate
// godot-ai-relay binary, now a mode of the one binary the user already has (DEC-0015).
//
// The platform main functions call this BEFORE any engine initialisation, which is the
// entire trick: nothing has printed to stdout yet and nothing ever will, so the stdio
// protocol stream stays clean - the same property the separate process bought, without
// the separate process. Everything below main() is engine-free C++17.

#ifndef GODOT_AI_GATEWAY_MAIN_H
#define GODOT_AI_GATEWAY_MAIN_H

// True when argv asks for the gateway; the caller should then return this function's
// result from main() without touching the engine.
bool godot_ai_gateway_requested(int argc, char **argv);
int godot_ai_gateway_main(int argc, char **argv);

#endif // GODOT_AI_GATEWAY_MAIN_H
