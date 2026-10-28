#include "audio/PulseStream.hpp"

#include <cstdint>
#include <iostream>
#include <string>

pa_stream* new_chunk_stream(pa_context* context, const char* name, const pa_sample_spec& spec,
                            std::size_t framesPerChunk) {
    pa_proplist* props = pa_proplist_new();
    const std::string latency = std::to_string(framesPerChunk) + "/" + std::to_string(spec.rate);
    pa_proplist_sets(props, "node.latency", latency.c_str());
    pa_stream* stream = pa_stream_new_with_proplist(context, name, &spec, nullptr, props);
    pa_proplist_free(props);
    return stream;
}

void print_buffer_attr(pa_stream* stream, const char* name, const pa_sample_spec& spec) {
    const pa_buffer_attr* attr = pa_stream_get_buffer_attr(stream);
    if (attr == nullptr) {
        return;
    }
    const auto ms = [&](std::uint32_t bytes) {
        return bytes == static_cast<std::uint32_t>(-1)
                   ? -1.0
                   : static_cast<double>(pa_bytes_to_usec(bytes, &spec)) / 1000.0;
    };
    std::cout << name << ": tlength " << ms(attr->tlength) << " ms, minreq " << ms(attr->minreq)
              << " ms, fragsize " << ms(attr->fragsize) << " ms\n";
}
