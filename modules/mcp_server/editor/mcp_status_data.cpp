/**************************************************************************/
/*  mcp_status_data.cpp                                                   */
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

#include "mcp_status_data.h"

#include "core/os/os.h"

// =========================================================================
// MCPEventBuffer Implementation
// =========================================================================

MCPEventBuffer::MCPEventBuffer() {
	events.resize(CAPACITY);
}

void MCPEventBuffer::push(const MCPRequestEvent &p_event) {
	MutexLock lock(mutex);

	MCPRequestEvent &entry = events.write[write_pos];
	entry = p_event;
	entry.seq = next_seq++;

	write_pos = (write_pos + 1) % CAPACITY;
	if (count < CAPACITY) {
		count++;
	}
}

Vector<MCPRequestEvent> MCPEventBuffer::read_since(uint64_t p_cursor, int p_limit) const {
	MutexLock lock(mutex);

	Vector<MCPRequestEvent> result;

	if (count == 0) {
		return result;
	}

	// Find the oldest entry's position.
	int oldest_pos = (count < CAPACITY) ? 0 : write_pos;

	// Scan entries from oldest to newest, collecting those with seq > p_cursor.
	int collected = 0;
	for (int i = 0; i < count && collected < p_limit; i++) {
		int idx = (oldest_pos + i) % CAPACITY;
		const MCPRequestEvent &entry = events[idx];
		if (entry.seq > p_cursor) {
			result.push_back(entry);
			collected++;
		}
	}

	return result;
}

uint64_t MCPEventBuffer::latest_seq() const {
	MutexLock lock(mutex);
	if (next_seq <= 1) {
		return 0;
	}
	return next_seq - 1;
}

void MCPEventBuffer::clear() {
	MutexLock lock(mutex);
	write_pos = 0;
	count = 0;
	next_seq = 1;
}

// =========================================================================
// MCPTestEventBuffer Implementation
// =========================================================================

MCPTestEventBuffer::MCPTestEventBuffer() {
	events.resize(CAPACITY);
}

void MCPTestEventBuffer::push(const MCPTestEvent &p_event) {
	MutexLock lock(mutex);

	MCPTestEvent &entry = events.write[write_pos];
	entry = p_event;
	entry.seq = next_seq++;
	entry.timestamp_usec = OS::get_singleton()->get_ticks_usec();

	write_pos = (write_pos + 1) % CAPACITY;
	if (count < CAPACITY) {
		count++;
	}
}

Vector<MCPTestEvent> MCPTestEventBuffer::read_since(uint64_t p_cursor, int p_limit) const {
	MutexLock lock(mutex);

	Vector<MCPTestEvent> result;

	if (count == 0) {
		return result;
	}

	// Find the oldest entry's position.
	int oldest_pos = (count < CAPACITY) ? 0 : write_pos;

	// Scan entries from oldest to newest, collecting those with seq > p_cursor.
	int collected = 0;
	for (int i = 0; i < count && collected < p_limit; i++) {
		int idx = (oldest_pos + i) % CAPACITY;
		const MCPTestEvent &entry = events[idx];
		if (entry.seq > p_cursor) {
			result.push_back(entry);
			collected++;
		}
	}

	return result;
}

uint64_t MCPTestEventBuffer::latest_seq() const {
	MutexLock lock(mutex);
	if (next_seq <= 1) {
		return 0;
	}
	return next_seq - 1;
}

void MCPTestEventBuffer::clear() {
	MutexLock lock(mutex);
	write_pos = 0;
	count = 0;
	next_seq = 1;
}
