/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef DUALBOY_MELONDS_PLATFORM_BRIDGE_HPP
#define DUALBOY_MELONDS_PLATFORM_BRIDGE_HPP

#include "frontend/engine.h"

#include <Platform.h>
#include <SPI_Firmware.h>

#include <cstdint>
#include <string>

namespace dualboy_melonds_platform {

void Configure(dualboy_log_fn log,
               void *log_context,
               const char *system_directory);
void ClearConfiguration();
std::string SystemDirectory();
void Log(melonDS::Platform::LogLevel level, const char *message);

void SignalStop(melonDS::Platform::StopReason reason, void *userdata);
void WriteNDSSave(const std::uint8_t *data,
                  std::uint32_t size,
                  std::uint32_t offset,
                  std::uint32_t length,
                  void *userdata);
void WriteGBASave(const std::uint8_t *data,
                  std::uint32_t size,
                  std::uint32_t offset,
                  std::uint32_t length,
                  void *userdata);
void WriteFirmware(const melonDS::Firmware &firmware,
                   std::uint32_t offset,
                   std::uint32_t length,
                   void *userdata);
void WriteDateTime(int year,
                   int month,
                   int day,
                   int hour,
                   int minute,
                   int second,
                   void *userdata);

void MPBegin(void *userdata);
void MPEnd(void *userdata);
int MPSendPacket(std::uint8_t *data,
                 int length,
                 std::uint64_t timestamp,
                 void *userdata);
int MPRecvPacket(std::uint8_t *data,
                 std::uint64_t *timestamp,
                 void *userdata);
int MPSendCmd(std::uint8_t *data,
              int length,
              std::uint64_t timestamp,
              void *userdata);
int MPSendReply(std::uint8_t *data,
                int length,
                std::uint64_t timestamp,
                std::uint16_t aid,
                void *userdata);
int MPSendAck(std::uint8_t *data,
              int length,
              std::uint64_t timestamp,
              void *userdata);
int MPRecvHostPacket(std::uint8_t *data,
                     std::uint64_t *timestamp,
                     void *userdata);
std::uint16_t MPRecvReplies(std::uint8_t *data,
                            std::uint64_t timestamp,
                            std::uint16_t aidmask,
                            void *userdata);

} // namespace dualboy_melonds_platform

#endif
