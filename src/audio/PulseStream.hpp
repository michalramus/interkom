#pragma once

#include <cstddef>
#include <pulse/pulseaudio.h>

// A stream that asks the sound server for one chunk per period. PulseAudio
// takes that from the buffer attributes; PipeWire's pulse layer schedules by
// its graph quantum instead, which defaults to ~21 ms and would hand out (or
// ask for) several chunks at once, so the quantum is requested explicitly via
// node.latency. Other servers ignore the property.
pa_stream* new_chunk_stream(pa_context* context, const char* name, const pa_sample_spec& spec,
                            std::size_t framesPerChunk);

// Prints what the server actually granted, once the stream is ready: if the
// period is larger than a chunk, callbacks come in bursts of several chunks.
void print_buffer_attr(pa_stream* stream, const char* name, const pa_sample_spec& spec);
