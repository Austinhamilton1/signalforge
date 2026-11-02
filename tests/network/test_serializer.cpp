#include <catch2/catch_test_macros.hpp>

#include <iostream>

#include "network/serializer.hpp"

TEST_CASE("Serializer can be constructed", "[serializer] [construction]") {
    REQUIRE_NOTHROW(sf::serializer{});
}

TEST_CASE("Serializer can serialize and deserialize an audio packet", "[serializer] [serialization]") {
    sf::audio_packet_t original_packet{};
    original_packet.header.seq = 42;
    original_packet.header.timestamp = 123456789;
    original_packet.header.frame_count = 4;
    original_packet.header.flags = 0x01;
    original_packet.samples[0] = 1.0f;
    original_packet.samples[1] = 2.0f;
    original_packet.samples[2] = 3.0f;
    original_packet.samples[3] = 4.0f;

    uint8_t buffer[sf::detail::max_packet_size]{};
    sf::serializer::serialize(buffer, original_packet, sizeof(buffer));

    sf::audio_packet_t deserialized_packet{};
    sf::serializer::deserialize(deserialized_packet, buffer, sizeof(buffer));

    REQUIRE(deserialized_packet.header.seq == original_packet.header.seq);
    REQUIRE(deserialized_packet.header.timestamp == original_packet.header.timestamp);
    REQUIRE(deserialized_packet.header.frame_count == original_packet.header.frame_count);
    REQUIRE(deserialized_packet.header.flags == original_packet.header.flags);
    for (size_t i = 0; i < deserialized_packet.header.frame_count; ++i) {
        REQUIRE(deserialized_packet.samples[i] == original_packet.samples[i]);
    }
}

TEST_CASE("Serializer can handle empty audio packets", "[serializer] [serialization]") {
    sf::audio_packet_t original_packet{};
    original_packet.header.seq = 0;
    original_packet.header.timestamp = 0;
    original_packet.header.frame_count = 0;
    original_packet.header.flags = 0;

    uint8_t buffer[sf::detail::max_packet_size]{};
    sf::serializer::serialize(buffer, original_packet, sizeof(buffer));

    sf::audio_packet_t deserialized_packet{};
    sf::serializer::deserialize(deserialized_packet, buffer, sizeof(buffer));

    REQUIRE(deserialized_packet.header.seq == original_packet.header.seq);
    REQUIRE(deserialized_packet.header.timestamp == original_packet.header.timestamp);
    REQUIRE(deserialized_packet.header.frame_count == original_packet.header.frame_count);
    REQUIRE(deserialized_packet.header.flags == original_packet.header.flags);
}

TEST_CASE("Serializer can handle maximum frame count", "[serializer] [serialization]") {
    sf::audio_packet_t original_packet{};
    original_packet.header.seq = 1;
    original_packet.header.timestamp = 987654321;
    original_packet.header.frame_count = sf::detail::frames_per_packet;
    original_packet.header.flags = 0xFF;
    for (size_t i = 0; i < sf::detail::frames_per_packet; ++i) {
        original_packet.samples[i] = static_cast<float>(i);
    }

    uint8_t buffer[sf::detail::max_packet_size]{};
    sf::audio_packet_t deserialized_packet{};

    REQUIRE(deserialized_packet.header.seq == original_packet.header.seq);
    REQUIRE(deserialized_packet.header.timestamp == original_packet.header.timestamp);
    REQUIRE(deserialized_packet.header.frame_count == original_packet.header.frame_count);
    REQUIRE(deserialized_packet.header.flags == original_packet.header.flags);
    for (size_t i = 0; i < deserialized_packet.header.frame_count; ++i) {
        REQUIRE(deserialized_packet.samples[i] == original_packet.samples[i]);
    }
}

TEST_CASE("Serializer can handle negative sample values", "[serializer] [serialization]") {
    sf::audio_packet_t original_packet{};
    original_packet.header.seq = 2;
    original_packet.header.timestamp = 555555555;
    original_packet.header.frame_count = 4;
    original_packet.header.flags = 0xAA;
    original_packet.samples[0] = -1.0f;
    original_packet.samples[1] = -2.0f;
    original_packet.samples[2] = -3.0f;
    original_packet.samples[3] = -4.0f;

    uint8_t buffer[sf::detail::max_packet_size]{};
    sf::serializer::serialize(buffer, original_packet, sizeof(buffer));

    sf::audio_packet_t deserialized_packet{};
    sf::serializer::deserialize(deserialized_packet, buffer, sizeof(buffer));

    REQUIRE(deserialized_packet.header.seq == original_packet.header.seq);
    REQUIRE(deserialized_packet.header.timestamp == original_packet.header.timestamp);
    REQUIRE(deserialized_packet.header.frame_count == original_packet.header.frame_count);
    REQUIRE(deserialized_packet.header.flags == original_packet.header.flags);
    for (size_t i = 0; i < deserialized_packet.header.frame_count; ++i) {
        REQUIRE(deserialized_packet.samples[i] == original_packet.samples[i]);
    }
}

TEST_CASE("Serializer can handle mixed positive and negative sample values", "[serializer] [serialization]") {
    sf::audio_packet_t original_packet{};
    original_packet.header.seq = 3;
    original_packet.header.timestamp = 111111111;
    original_packet.header.frame_count = 4;
    original_packet.header.flags = 0x55;
    original_packet.samples[0] = -1.0f;
    original_packet.samples[1] = 2.0f;
    original_packet.samples[2] = -3.0f;
    original_packet.samples[3] = 4.0f;

    uint8_t buffer[sf::detail::max_packet_size]{};
    sf::serializer::serialize(buffer, original_packet, sizeof(buffer));

    sf::audio_packet_t deserialized_packet{};
    sf::serializer::deserialize(deserialized_packet, buffer, sizeof(buffer));

    REQUIRE(deserialized_packet.header.seq == original_packet.header.seq);
    REQUIRE(deserialized_packet.header.timestamp == original_packet.header.timestamp);
    REQUIRE(deserialized_packet.header.frame_count == original_packet.header.frame_count);
    REQUIRE(deserialized_packet.header.flags == original_packet.header.flags);
    for (size_t i = 0; i < deserialized_packet.header.frame_count; ++i) {
        REQUIRE(deserialized_packet.samples[i] == original_packet.samples[i]);
    }
}

TEST_CASE("Serializer can handle zero and maximum float values", "[serializer] [serialization]") {
    sf::audio_packet_t original_packet{};
    original_packet.header.seq = 4;
    original_packet.header.timestamp = 222222222;
    original_packet.header.frame_count = 4;
    original_packet.header.flags = 0x00;
    original_packet.samples[0] = 0.0f;
    original_packet.samples[1] = std::numeric_limits<float>::max();
    original_packet.samples[2] = std::numeric_limits<float>::lowest();
    original_packet.samples[3] = -0.0f;

    uint8_t buffer[sf::detail::max_packet_size]{};
    sf::serializer::serialize(buffer, original_packet, sizeof(buffer));

    sf::audio_packet_t deserialized_packet{};
    sf::serializer::deserialize(deserialized_packet, buffer, sizeof(buffer));

    REQUIRE(deserialized_packet.header.seq == original_packet.header.seq);
    REQUIRE(deserialized_packet.header.timestamp == original_packet.header.timestamp);
    REQUIRE(deserialized_packet.header.frame_count == original_packet.header.frame_count);
    REQUIRE(deserialized_packet.header.flags == original_packet.header.flags);
    for (size_t i = 0; i < deserialized_packet.header.frame_count; ++i) {
        REQUIRE(deserialized_packet.samples[i] == original_packet.samples[i]);
    }
}