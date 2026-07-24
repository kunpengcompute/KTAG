/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#ifndef KTAG_DEMO_CAPTURE_ADAPTER_H
#define KTAG_DEMO_CAPTURE_ADAPTER_H

#include <sys/time.h>

#include "ktag_capture.h"

inline ktag_capture_config make_offline_capture_config(
    const char *pcap_file,
    ktag_capture_packet_mode packet_mode = KTAG_CAPTURE_PACKET_BORROWED)
{
    ktag_capture_config config{};
    config.struct_size = sizeof(config);
    config.api_version = KTAG_API_VERSION;
    config.source_type = KTAG_CAPTURE_SOURCE_PCAP_FILE;
    config.source = pcap_file;
    config.packet_mode = packet_mode;
    config.snapshot_length = 65535U;
    config.packet_buffer_capacity = 1024U;
    return config;
}

inline ktag_capture_config make_live_capture_config(
    const char *interface,
    ktag_capture_packet_mode packet_mode = KTAG_CAPTURE_PACKET_BORROWED)
{
    ktag_capture_config config{};
    config.struct_size = sizeof(config);
    config.api_version = KTAG_API_VERSION;
    config.source_type = KTAG_CAPTURE_SOURCE_LIVE_INTERFACE;
    config.source = interface;
    config.packet_mode = packet_mode;
    config.snapshot_length = 65535U;
    config.promiscuous = 1U;
    config.read_timeout_ms = 10U;
    config.live_buffer_size = 1024U * 1024U * 1024U;
    config.packet_buffer_capacity = 1024U;
    return config;
}

inline timeval packet_timeval(const ktag_packet &packet)
{
    timeval timestamp{};
    timestamp.tv_sec = static_cast<decltype(timestamp.tv_sec)>(packet.timestamp.seconds);
    timestamp.tv_usec = static_cast<decltype(timestamp.tv_usec)>(packet.timestamp.microseconds);
    return timestamp;
}

#endif /* KTAG_DEMO_CAPTURE_ADAPTER_H */
