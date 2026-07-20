/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#include "capture_internal.h"

#include <arpa/inet.h>
#include <netinet/in.h>

#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <utility>

namespace {

thread_local std::string capture_error;

constexpr uint32_t DEFAULT_SNAPSHOT_LENGTH = 65535U;
constexpr uint32_t DEFAULT_READ_TIMEOUT_MS = 10U;
constexpr uint32_t DEFAULT_PACKET_BUFFER_CAPACITY = 1024U;

bool valid_source(const char *source)
{
    return source != nullptr && source[0] != '\0';
}

bool configure_live_handle(pcap_t *handle, const ktag_capture_config &config)
{
    const int snapshot_length = static_cast<int>(
        config.snapshot_length == 0 ? DEFAULT_SNAPSHOT_LENGTH : config.snapshot_length);
    const int timeout = static_cast<int>(
        config.read_timeout_ms == 0 ? DEFAULT_READ_TIMEOUT_MS : config.read_timeout_ms);
    if (pcap_set_snaplen(handle, snapshot_length) != 0 ||
        pcap_set_promisc(handle, config.promiscuous != 0 ? 1 : 0) != 0 ||
        pcap_set_timeout(handle, timeout) != 0 ||
        (config.live_buffer_size != 0 &&
         pcap_set_buffer_size(handle, static_cast<int>(config.live_buffer_size)) != 0)) {
        ktag_capture_set_error_internal(pcap_geterr(handle));
        return false;
    }
    return true;
}

} // namespace

void ktag_capture_set_error_internal(const std::string &message)
{
    capture_error = message;
}

extern "C" {

KTAG_API ktag_capture_context *ktag_capture_open(
    const ktag_capture_config *config)
{
    capture_error.clear();
    if (config == nullptr || config->struct_size < sizeof(*config) ||
        config->api_version != KTAG_API_VERSION || !valid_source(config->source) ||
        (config->source_type != KTAG_CAPTURE_SOURCE_PCAP_FILE &&
         config->source_type != KTAG_CAPTURE_SOURCE_LIVE_INTERFACE) ||
        (config->packet_mode != KTAG_CAPTURE_PACKET_BORROWED &&
         config->packet_mode != KTAG_CAPTURE_PACKET_OWNED) ||
        config->snapshot_length > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
        config->read_timeout_ms > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
        config->live_buffer_size > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
        capture_error = "invalid capture configuration";
        return nullptr;
    }

    char error_buffer[PCAP_ERRBUF_SIZE] = {};
    pcap_t *handle = nullptr;
    if (config->source_type == KTAG_CAPTURE_SOURCE_PCAP_FILE) {
        handle = pcap_open_offline(config->source, error_buffer);
    } else {
        handle = pcap_create(config->source, error_buffer);
        if (handle != nullptr && !configure_live_handle(handle, *config)) {
            pcap_close(handle);
            return nullptr;
        }
        if (handle != nullptr) {
            const int activate_status = pcap_activate(handle);
            if (activate_status < 0) {
                capture_error = pcap_geterr(handle);
                pcap_close(handle);
                return nullptr;
            }
            if (activate_status > 0) {
                capture_error = pcap_statustostr(activate_status);
            }
        }
    }
    if (handle == nullptr) {
        capture_error = error_buffer[0] == '\0' ? "failed to open capture source" : error_buffer;
        return nullptr;
    }

    std::unique_ptr<ktag_capture_context> context(
        new (std::nothrow) ktag_capture_context());
    if (!context) {
        capture_error = "out of memory creating capture context";
        pcap_close(handle);
        return nullptr;
    }
    context->handle = handle;
    context->packet_mode = config->packet_mode;
    const int link_type = pcap_datalink(handle);
    if (link_type < 0) {
        capture_error = pcap_geterr(handle);
        pcap_close(handle);
        return nullptr;
    }
    context->link_type = static_cast<uint32_t>(link_type);

    if (config->packet_mode == KTAG_CAPTURE_PACKET_OWNED) {
        const uint32_t capacity = config->packet_buffer_capacity == 0 ?
            DEFAULT_PACKET_BUFFER_CAPACITY : config->packet_buffer_capacity;
        context->pool = ktag_packet_pool_create_internal(capacity);
        if (context->pool == nullptr) {
            capture_error = "out of memory creating packet buffer pool";
            pcap_close(handle);
            return nullptr;
        }
    }
    return context.release();
}

KTAG_API ktag_status ktag_capture_next(
    ktag_capture_context *context,
    ktag_packet *packet)
{
    capture_error.clear();
    if (context == nullptr || context->handle == nullptr || packet == nullptr ||
        packet->struct_size < sizeof(*packet) ||
        packet->api_version != KTAG_API_VERSION) {
        capture_error = "invalid capture context or packet output";
        return KTAG_ERROR_INVALID_ARGUMENT;
    }
    if (packet->owner != nullptr) {
        capture_error = "owned packet must be released before capture_next";
        return KTAG_ERROR_INVALID_STATE;
    }

    ktag_packet_buffer *buffer = nullptr;
    if (context->packet_mode == KTAG_CAPTURE_PACKET_OWNED) {
        const ktag_status acquire_status =
            ktag_packet_pool_acquire_internal(context->pool, &buffer);
        if (acquire_status != KTAG_SUCCESS) {
            capture_error = acquire_status == KTAG_ERROR_CAPACITY ?
                "packet buffer pool exhausted" : "failed to acquire packet buffer";
            return acquire_status;
        }
    }

    struct pcap_pkthdr *header = nullptr;
    const u_char *data = nullptr;
    const int status = pcap_next_ex(context->handle, &header, &data);
    if (status != 1) {
        if (buffer != nullptr) {
            ktag_packet_buffer_release_internal(buffer);
        }
        ktag_packet_init(packet);
        if (status == 0) {
            return KTAG_STATUS_TIMEOUT;
        }
        if (status == PCAP_ERROR_BREAK) {
            return KTAG_STATUS_END_OF_STREAM;
        }
        capture_error = pcap_geterr(context->handle);
        return KTAG_ERROR_IO;
    }
    if (header == nullptr || (data == nullptr && header->caplen != 0)) {
        if (buffer != nullptr) {
            ktag_packet_buffer_release_internal(buffer);
        }
        ktag_packet_init(packet);
        capture_error = "capture source returned an invalid packet";
        return KTAG_ERROR_IO;
    }
    if (context->next_sequence == 0) {
        if (buffer != nullptr) {
            ktag_packet_buffer_release_internal(buffer);
        }
        ktag_packet_init(packet);
        capture_error = "packet sequence exhausted";
        return KTAG_ERROR_CAPACITY;
    }

    ktag_packet_init(packet);
    if (buffer != nullptr) {
        try {
            if (header->caplen == 0U) {
                buffer->bytes.clear();
            } else {
                buffer->bytes.assign(data, data + header->caplen);
            }
        } catch (const std::bad_alloc &) {
            ktag_packet_buffer_release_internal(buffer);
            capture_error = "out of memory copying captured packet";
            return KTAG_ERROR_OUT_OF_MEMORY;
        } catch (...) {
            ktag_packet_buffer_release_internal(buffer);
            capture_error = "failed to copy captured packet";
            return KTAG_ERROR_INTERNAL;
        }
        packet->data = buffer->bytes.empty() ? nullptr : buffer->bytes.data();
        packet->owner = buffer;
    } else {
        packet->data = data;
    }
    packet->captured_length = header->caplen;
    packet->original_length = header->len;
    packet->link_type = context->link_type;
    packet->sequence = context->next_sequence++;
    packet->timestamp.seconds = header->ts.tv_sec;
    packet->timestamp.microseconds = static_cast<int32_t>(header->ts.tv_usec);
    return KTAG_SUCCESS;
}

KTAG_API uint32_t ktag_capture_get_link_type(
    const ktag_capture_context *context)
{
    return context == nullptr ? 0U : context->link_type;
}

KTAG_API void ktag_capture_interrupt(ktag_capture_context *context)
{
    if (context != nullptr && context->handle != nullptr) {
        pcap_breakloop(context->handle);
    }
}

KTAG_API void ktag_capture_close(ktag_capture_context *context)
{
    if (context == nullptr) {
        return;
    }
    if (context->handle != nullptr) {
        pcap_close(context->handle);
        context->handle = nullptr;
    }
    ktag_packet_pool_close_internal(context->pool);
    context->pool = nullptr;
    delete context;
}

KTAG_API const char *ktag_capture_get_last_error(void)
{
    return capture_error.c_str();
}

KTAG_API ktag_capture_device_list *ktag_capture_list_devices(void)
{
    capture_error.clear();
    char error_buffer[PCAP_ERRBUF_SIZE] = {};
    pcap_if_t *all_devices = nullptr;
    if (pcap_findalldevs(&all_devices, error_buffer) != 0) {
        capture_error = error_buffer;
        return nullptr;
    }

    std::unique_ptr<ktag_capture_device_list> result(
        new (std::nothrow) ktag_capture_device_list());
    if (!result) {
        pcap_freealldevs(all_devices);
        capture_error = "out of memory listing capture devices";
        return nullptr;
    }

    try {
        for (pcap_if_t *device = all_devices; device != nullptr; device = device->next) {
            ktag_capture_device_storage stored;
            stored.name = device->name == nullptr ? "" : device->name;
            stored.description = device->description == nullptr ? "" : device->description;
            for (pcap_addr_t *address = device->addresses;
                 address != nullptr; address = address->next) {
                if (address->addr == nullptr || address->addr->sa_family != AF_INET) {
                    continue;
                }
                const auto *ipv4 = reinterpret_cast<const sockaddr_in *>(address->addr);
                char text[INET_ADDRSTRLEN] = {};
                if (inet_ntop(AF_INET, &ipv4->sin_addr, text, sizeof(text)) != nullptr) {
                    stored.ipv4_addresses.emplace_back(text);
                }
            }
            result->devices.push_back(std::move(stored));
        }
    } catch (const std::bad_alloc &) {
        pcap_freealldevs(all_devices);
        capture_error = "out of memory listing capture devices";
        return nullptr;
    } catch (...) {
        pcap_freealldevs(all_devices);
        capture_error = "failed to list capture devices";
        return nullptr;
    }
    pcap_freealldevs(all_devices);
    return result.release();
}

KTAG_API void ktag_capture_device_list_destroy(
    ktag_capture_device_list *devices)
{
    delete devices;
}

KTAG_API uint32_t ktag_capture_device_list_get_count(
    const ktag_capture_device_list *devices)
{
    return devices == nullptr ? 0U : static_cast<uint32_t>(devices->devices.size());
}

KTAG_API const char *ktag_capture_device_list_get_name(
    const ktag_capture_device_list *devices,
    uint32_t index)
{
    return devices == nullptr || index >= devices->devices.size() ?
        nullptr : devices->devices[index].name.c_str();
}

KTAG_API const char *ktag_capture_device_list_get_description(
    const ktag_capture_device_list *devices,
    uint32_t index)
{
    if (devices == nullptr || index >= devices->devices.size() ||
        devices->devices[index].description.empty()) {
        return nullptr;
    }
    return devices->devices[index].description.c_str();
}

KTAG_API uint32_t ktag_capture_device_list_get_ipv4_count(
    const ktag_capture_device_list *devices,
    uint32_t index)
{
    return devices == nullptr || index >= devices->devices.size() ? 0U :
        static_cast<uint32_t>(devices->devices[index].ipv4_addresses.size());
}

KTAG_API const char *ktag_capture_device_list_get_ipv4(
    const ktag_capture_device_list *devices,
    uint32_t device_index,
    uint32_t address_index)
{
    if (devices == nullptr || device_index >= devices->devices.size() ||
        address_index >= devices->devices[device_index].ipv4_addresses.size()) {
        return nullptr;
    }
    return devices->devices[device_index].ipv4_addresses[address_index].c_str();
}

} // extern "C"
