#ifndef OSMIUM_IO_DETAIL_PBF_INPUT_FORMAT_HPP
#define OSMIUM_IO_DETAIL_PBF_INPUT_FORMAT_HPP

/*

This file is part of Osmium (http://osmcode.org/libosmium).

Copyright 2013-2015 Jochen Topf <jochen@topf.org> and others (see README).

Boost Software License - Version 1.0 - August 17th, 2003

Permission is hereby granted, free of charge, to any person or organization
obtaining a copy of the software and accompanying documentation covered by
this license (the "Software") to use, reproduce, display, distribute,
execute, and transmit the Software, and to prepare derivative works of the
Software, and to permit third-parties to whom the Software is furnished to
do so, all subject to the following:

The copyright notices in the Software and this entire statement, including
the above license grant, this restriction and the following disclaimer,
must be included in all copies of the Software, in whole or in part, and
all derivative works of the Software, unless such copies or derivative
works are solely in the form of machine-executable object code generated by
a source language processor.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.

*/

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <future>
#include <memory>
#include <ratio>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>

#include <protozero/pbf_message.hpp>

#include <osmium/io/detail/input_format.hpp>
#include <osmium/io/detail/pbf.hpp> // IWYU pragma: export
#include <osmium/io/detail/pbf_decoder.hpp>
#include <osmium/io/detail/protobuf_tags.hpp>
#include <osmium/io/error.hpp>
#include <osmium/io/file.hpp>
#include <osmium/io/file_format.hpp>
#include <osmium/memory/buffer.hpp>
#include <osmium/osm.hpp>
#include <osmium/osm/box.hpp>
#include <osmium/osm/entity_bits.hpp>
#include <osmium/osm/location.hpp>
#include <osmium/osm/object.hpp>
#include <osmium/osm/timestamp.hpp>
#include <osmium/thread/pool.hpp>
#include <osmium/thread/queue.hpp>
#include <osmium/thread/util.hpp>
#include <osmium/util/cast.hpp>
#include <osmium/util/config.hpp>

namespace osmium {

    namespace io {

        class File;

        namespace detail {

            /**
             * Class for parsing PBF files.
             */
            class PBFInputFormat : public osmium::io::detail::InputFormat {

                typedef osmium::thread::Queue<std::future<osmium::memory::Buffer>> queue_type;

                bool m_use_thread_pool;
                bool m_eof { false };
                queue_type m_queue;
                std::atomic<bool> m_quit_input_thread;
                std::thread m_reader;
                osmium::thread::Queue<std::string>& m_input_queue;
                std::string m_input_buffer;

                /**
                 * Read the given number of bytes from the input queue.
                 *
                 * @param size Number of bytes to read
                 * @returns String with the data
                 * @throws osmium::pbf_error If size bytes can't be read
                 */
                std::string read_from_input_queue(size_t size) {
                    while (m_input_buffer.size() < size) {
                        std::string new_data;
                        m_input_queue.wait_and_pop(new_data);
                        if (new_data.empty()) {
                            throw osmium::pbf_error("truncated data (EOF encountered)");
                        }
                        m_input_buffer += new_data;
                    }

                    std::string output { m_input_buffer.substr(size) };
                    m_input_buffer.resize(size);

                    using std::swap;
                    swap(output, m_input_buffer);

                    return output;
                }

                /**
                 * Read 4 bytes in network byte order from file. They contain
                 * the length of the following BlobHeader.
                 */
                uint32_t read_blob_header_size_from_file() {
                    uint32_t size_in_network_byte_order;

                    try {
                        std::string input_data = read_from_input_queue(sizeof(size_in_network_byte_order));
                        size_in_network_byte_order = *reinterpret_cast<const uint32_t*>(input_data.data());
                    } catch (osmium::pbf_error&) {
                        return 0; // EOF
                    }

                    const uint32_t size = ntohl(size_in_network_byte_order);
                    if (size > static_cast<uint32_t>(max_blob_header_size)) {
                        throw osmium::pbf_error("invalid BlobHeader size (> max_blob_header_size)");
                    }

                    return size;
                }

                /**
                 * Decode the BlobHeader. Make sure it contains the expected
                 * type. Return the size of the following Blob.
                 */
                size_t decode_blob_header(protozero::pbf_message<FileFormat::BlobHeader>&& pbf_blob_header, const char* expected_type) {
                    std::pair<const char*, size_t> blob_header_type;
                    size_t blob_header_datasize = 0;

                    while (pbf_blob_header.next()) {
                        switch (pbf_blob_header.tag()) {
                            case FileFormat::BlobHeader::required_string_type:
                                blob_header_type = pbf_blob_header.get_data();
                                break;
                            case FileFormat::BlobHeader::required_int32_datasize:
                                blob_header_datasize = pbf_blob_header.get_int32();
                                break;
                            default:
                                pbf_blob_header.skip();
                        }
                    }

                    if (blob_header_datasize == 0) {
                        throw osmium::pbf_error("PBF format error: BlobHeader.datasize missing or zero.");
                    }

                    if (strncmp(expected_type, blob_header_type.first, blob_header_type.second)) {
                        throw osmium::pbf_error("blob does not have expected type (OSMHeader in first blob, OSMData in following blobs)");
                    }

                    return blob_header_datasize;
                }

                size_t check_type_and_get_blob_size(const char* expected_type) {
                    assert(expected_type);

                    auto size = read_blob_header_size_from_file();
                    if (size == 0) { // EOF
                        return 0;
                    }

                    std::string blob_header = read_from_input_queue(size);

                    return decode_blob_header(protozero::pbf_message<FileFormat::BlobHeader>(blob_header), expected_type);
                }

                void parse_osm_data(osmium::osm_entity_bits::type read_types) {
                    osmium::thread::set_thread_name("_osmium_pbf_in");

                    while (auto size = check_type_and_get_blob_size("OSMData")) {
                        std::string input_buffer = read_from_input_queue(size);
                        if (input_buffer.size() > max_uncompressed_blob_size) {
                            throw osmium::pbf_error(std::string("invalid blob size: " + std::to_string(input_buffer.size())));
                        }

                        if (m_use_thread_pool) {
                            m_queue.push(osmium::thread::Pool::instance().submit(PBFDataBlobDecoder{ std::move(input_buffer), read_types }));
                        } else {
                            std::promise<osmium::memory::Buffer> promise;
                            m_queue.push(promise.get_future());
                            PBFDataBlobDecoder data_blob_parser{ std::move(input_buffer), read_types };
                            promise.set_value(data_blob_parser());
                        }

                        if (m_quit_input_thread) {
                            return;
                        }
                    }

                    // Send an empty buffer to signal the reader that we are
                    // done.
                    std::promise<osmium::memory::Buffer> promise;
                    m_queue.push(promise.get_future());
                    promise.set_value(osmium::memory::Buffer{});
                }

                void signal_input_thread_to_quit() {
                    m_quit_input_thread = true;
                }

            public:

                /**
                 * Instantiate PBF Parser
                 *
                 * @param file osmium::io::File instance describing file to be read from.
                 * @param read_which_entities Which types of OSM entities (nodes, ways, relations, changesets) should be parsed?
                 * @param input_queue String queue where data is read from.
                 */
                PBFInputFormat(const osmium::io::File& file, osmium::osm_entity_bits::type read_which_entities, osmium::thread::Queue<std::string>& input_queue) :
                    osmium::io::detail::InputFormat(file, read_which_entities),
                    m_use_thread_pool(osmium::config::use_pool_threads_for_pbf_parsing()),
                    m_queue(20, "pbf_parser_results"), // XXX
                    m_quit_input_thread(false),
                    m_input_queue(input_queue),
                    m_input_buffer() {

                    // handle OSMHeader
                    const auto size = check_type_and_get_blob_size("OSMHeader");
                    m_header = decode_header(read_from_input_queue(size));

                    if (m_read_which_entities != osmium::osm_entity_bits::nothing) {
                        m_reader = std::thread(&PBFInputFormat::parse_osm_data, this, m_read_which_entities);
                    }
                }

                ~PBFInputFormat() {
                    signal_input_thread_to_quit();
                    if (m_reader.joinable()) {
                        m_reader.join();
                    }
                }

                /**
                 * Returns the next buffer with OSM data read from the PBF
                 * file. Blocks if data is not available yet.
                 * Returns an empty buffer at end of input.
                 */
                osmium::memory::Buffer read() override {
                    osmium::memory::Buffer buffer;
                    if (m_eof) {
                        return buffer;
                    }

                    std::future<osmium::memory::Buffer> buffer_future;
                    m_queue.wait_and_pop(buffer_future);

                    try {
                        buffer = buffer_future.get();
                        if (!buffer) {
                            m_eof = true;
                        }
                        return buffer;
                    } catch (...) {
                        m_eof = true;
                        signal_input_thread_to_quit();
                        throw;
                    }
                }

            }; // class PBFInputFormat

            namespace {

// we want the register_input_format() function to run, setting the variable
// is only a side-effect, it will never be used
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
                const bool registered_pbf_input = osmium::io::detail::InputFormatFactory::instance().register_input_format(osmium::io::file_format::pbf,
                    [](const osmium::io::File& file, osmium::osm_entity_bits::type read_which_entities, osmium::thread::Queue<std::string>& input_queue) {
                        return new osmium::io::detail::PBFInputFormat(file, read_which_entities, input_queue);
                });
#pragma GCC diagnostic pop

            } // anonymous namespace

        } // namespace detail

    } // namespace io

} // namespace osmium

#endif // OSMIUM_IO_DETAIL_PBF_INPUT_FORMAT_HPP
