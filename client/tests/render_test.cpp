/**
 * Looking Glass
 * Copyright © 2017-2026 The Looking Glass Authors
 * https://looking-glass.io
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 59
 * Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include <gtest/gtest.h>

extern "C"
{
#include "common/types.h"
#include "interface/test_capture.h"
}

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace
{

constexpr unsigned kWidth       = 127;
constexpr unsigned kHeight      = 95;
constexpr unsigned kFrameSerial = 4;

struct FormatCase
{
  const char * name;
  FrameType type;
  bool hdr;
  bool pq;
};

struct DamageCase
{
  const char * name;
};

struct StrideCase
{
  const char * name;
  unsigned stride;
};

struct Capture
{
  LG_TestCaptureHeader header {};
  std::vector<uint8_t> data;
  std::filesystem::path directory;
  std::filesystem::path path;
  std::filesystem::path log;
};

struct RGB
{
  float r;
  float g;
  float b;
};

std::string readText(const std::filesystem::path & path)
{
  std::ifstream input(path);
  return std::string(
      std::istreambuf_iterator<char>(input),
      std::istreambuf_iterator<char>());
}

std::filesystem::path makeTempDirectory()
{
  std::array<char, 64> value {};
  std::snprintf(value.data(), value.size(), "/tmp/lg-render-case.XXXXXX");
  char * result = mkdtemp(value.data());
  if (!result)
    return {};
  return result;
}

int runClient(const FormatCase & format, const char * damage,
    const std::filesystem::path & capture,
    const std::filesystem::path & log, unsigned stride)
{
  const std::string size = std::to_string(kWidth) + "x" +
    std::to_string(kHeight);
  const std::string width = "test:width=" + std::to_string(kWidth);
  const std::string height = "test:height=" + std::to_string(kHeight);
  const std::string strideArg = "test:stride=" + std::to_string(stride);
  const std::string formatArg = std::string("test:format=") + format.name;
  const std::string damageArg = std::string("test:damage=") + damage;
  const std::string count = "test:frameCount=" +
    std::to_string(kFrameSerial);
  const std::string captureFile = "test:captureFile=" + capture.string();
  const std::string captureFrame = "test:captureFrame=" +
    std::to_string(kFrameSerial);

  std::vector<std::string> args = {
    LG_CLIENT_PATH,
    "app:transport=test",
    "app:renderer=EGL",
    width,
    height,
    strideArg,
    formatArg,
    damageArg,
    "test:frameRate=60",
    count,
    "test:holdLastFrame=yes",
    "test:realtime=no",
    captureFile,
    captureFrame,
    "test:captureDelay=1",
    "win:size=" + size,
    "win:autoResize=no",
    "win:allowResize=no",
    "win:quickSplash=yes",
    "win:alerts=no",
    "win:noScreensaver=no",
    "spice:enable=no",
    "egl:multisample=no",
    "egl:scale=1",
  };

  std::vector<char *> argv;
  argv.reserve(args.size() + 1);
  for (std::string & arg : args)
    argv.push_back(arg.data());
  argv.push_back(nullptr);

  const pid_t pid = fork();
  if (pid < 0)
    return -1;
  if (pid == 0)
  {
    setenv("XDG_CONFIG_HOME", capture.parent_path().c_str(), 1);
    setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1);
    const int logFd = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (logFd >= 0)
    {
      dup2(logFd, STDOUT_FILENO);
      dup2(logFd, STDERR_FILENO);
      close(logFd);
    }
    execv(LG_CLIENT_PATH, argv.data());
    _exit(127);
  }

  int status = 0;
  const auto deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (std::chrono::steady_clock::now() < deadline)
  {
    const pid_t result = waitpid(pid, &status, WNOHANG);
    if (result == pid)
      return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    if (result < 0)
      return -1;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  kill(pid, SIGTERM);
  if (waitpid(pid, &status, 0) < 0)
    return -1;
  return 124;
}

Capture produceCapture(const FormatCase & format, const char * damage,
    unsigned stride = 0)
{
  Capture result;
  result.directory = makeTempDirectory();
  if (result.directory.empty())
    return result;
  std::string artifactName = std::string(format.name) + "-" + damage;
  if (stride)
    artifactName += "-stride" + std::to_string(stride);
  result.path = result.directory / (artifactName + ".lgcapture");
  result.log  = result.directory / (artifactName + ".log");

  const int status =
    runClient(format, damage, result.path, result.log, stride);
  EXPECT_EQ(status, 0) << readText(result.log);
  if (status != 0)
    return result;

  std::ifstream input(result.path, std::ios::binary);
  EXPECT_TRUE(input.good()) << "capture missing; client log:\n" <<
    readText(result.log);
  if (!input)
    return result;

  input.read(reinterpret_cast<char *>(&result.header), sizeof(result.header));
  EXPECT_EQ(input.gcount(), static_cast<std::streamsize>(sizeof(result.header)));
  EXPECT_EQ(result.header.magic, LG_TEST_CAPTURE_MAGIC);
  EXPECT_EQ(result.header.version, LG_TEST_CAPTURE_VERSION);
  EXPECT_EQ(result.header.headerSize, sizeof(result.header));
  EXPECT_EQ(result.header.frameSerial, kFrameSerial);
  EXPECT_EQ(result.header.sourceType, static_cast<uint32_t>(format.type));
  EXPECT_EQ(result.header.width, kWidth);
  EXPECT_EQ(result.header.height, kHeight);
  EXPECT_EQ(result.header.flags & LG_TEST_CAPTURE_HDR,
      format.hdr ? static_cast<uint32_t>(LG_TEST_CAPTURE_HDR) : 0u);
  EXPECT_EQ(result.header.flags & LG_TEST_CAPTURE_HDR_PQ,
      format.pq ? static_cast<uint32_t>(LG_TEST_CAPTURE_HDR_PQ) : 0u);
  if (result.header.flags & LG_TEST_CAPTURE_NATIVE_HDR)
  {
    const std::string clientLog = readText(result.log);
    if (format.pq)
    {
      EXPECT_NE(clientLog.find(
          "HDR image description requested (PQ, BT.2020, "
          "referenceWhite:203 cd/m² maxLum:1000 cd/m²"),
          std::string::npos);
      EXPECT_NE(clientLog.find("maxCLL:1000 maxFALL:400"),
          std::string::npos);
    }
    else
      EXPECT_NE(clientLog.find(
          "HDR image description requested (scRGB, Windows-scRGB)"),
          std::string::npos);
  }

  result.data.resize(result.header.dataSize);
  input.read(reinterpret_cast<char *>(result.data.data()), result.data.size());
  EXPECT_EQ(input.gcount(), static_cast<std::streamsize>(result.data.size()));
  return result;
}

RGB generatedColor(unsigned x, unsigned y, unsigned serial)
{
  uint8_t r = static_cast<uint64_t>(x) * 255 / (kWidth - 1);
  uint8_t g = static_cast<uint64_t>(y) * 255 / (kHeight - 1);
  uint8_t b = ((x / 32) ^ (y / 32)) & 1 ? 0x30 : 0x90;

  const unsigned boxSize = std::min({kWidth, kHeight, 64u});
  const unsigned boxX = (serial * 7) % (kWidth - boxSize);
  const unsigned boxY = (serial * 5) % (kHeight - boxSize);
  if (x >= boxX && y >= boxY &&
      x < boxX + boxSize && y < boxY + boxSize)
  {
    const uint32_t color = serial * UINT32_C(2654435761);
    r = color >> 16;
    g = color >> 8;
    b = color;
  }

  return {
    static_cast<float>(r) / 255.0f,
    static_cast<float>(g) / 255.0f,
    static_cast<float>(b) / 255.0f,
  };
}

float linearToPQ(float linear)
{
  constexpr float m1 = 2610.0f / 16384.0f;
  constexpr float m2 = 2523.0f / 32.0f;
  constexpr float c1 = 3424.0f / 4096.0f;
  constexpr float c2 = 2413.0f / 128.0f;
  constexpr float c3 = 2392.0f / 128.0f;
  const float p = std::pow(std::max(linear, 0.0f), m1);
  return std::pow((c1 + c2 * p) / (1.0f + c3 * p), m2);
}

float pqToLinear(float pq)
{
  constexpr float m1inv = 16384.0f / 2610.0f;
  constexpr float m2inv = 32.0f / 2523.0f;
  constexpr float c1 = 3424.0f / 4096.0f;
  constexpr float c2 = 2413.0f / 128.0f;
  constexpr float c3 = 2392.0f / 128.0f;
  const float p = std::pow(std::max(pq, 0.0f), m2inv);
  const float d = std::max(p - c1, 0.0f) / (c2 - c3 * p);
  return std::pow(d, m1inv);
}

float quantizePQ(float scRGB)
{
  constexpr unsigned lutSize = 4096;
  const unsigned index = scRGB <= 0.0f ? 0 :
    scRGB >= 4.0f ? lutSize :
    static_cast<unsigned>(scRGB * (lutSize / 4.0f) + 0.5f);
  const float lutScRGB = static_cast<float>(index) * 4.0f / lutSize;
  const float pq = linearToPQ(lutScRGB / 125.0f);
  const unsigned code = static_cast<unsigned>(pq * 1023.0f + 0.5f);
  return static_cast<float>(code) / 1023.0f;
}

uint16_t floatToHalf(float value)
{
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  const uint16_t sign = (bits >> 16) & 0x8000;
  int exponent = ((bits >> 23) & 0xff) - 127 + 15;
  uint32_t mantissa = bits & 0x7fffff;
  if (exponent <= 0)
    return sign;
  if (exponent >= 31)
    return sign | 0x7c00;
  mantissa += 0x1000;
  if (mantissa & 0x800000)
  {
    mantissa = 0;
    if (++exponent >= 31)
      return sign | 0x7c00;
  }
  return sign | (static_cast<uint16_t>(exponent) << 10) | (mantissa >> 13);
}

float halfToFloat(uint16_t value)
{
  const uint32_t sign = static_cast<uint32_t>(value & 0x8000) << 16;
  uint32_t exponent = (value >> 10) & 0x1f;
  uint32_t mantissa = value & 0x3ff;
  uint32_t bits;
  if (exponent == 0)
    bits = sign;
  else if (exponent == 31)
    bits = sign | 0x7f800000 | (mantissa << 13);
  else
  {
    exponent = exponent - 15 + 127;
    bits = sign | (exponent << 23) | (mantissa << 13);
  }
  float result;
  std::memcpy(&result, &bits, sizeof(result));
  return result;
}

RGB toBT2020(RGB value)
{
  return {
    value.r * 0.6274039f + value.g * 0.3292830f + value.b * 0.0433131f,
    value.r * 0.0690973f + value.g * 0.9195404f + value.b * 0.0113623f,
    value.r * 0.0163914f + value.g * 0.0880133f + value.b * 0.8955953f,
  };
}

RGB toBT709(RGB value)
{
  return {
    value.r *  1.6604910f + value.g * -0.5876411f + value.b * -0.0728499f,
    value.r * -0.1245505f + value.g *  1.1328999f + value.b * -0.0083494f,
    value.r * -0.0181508f + value.g * -0.1005789f + value.b *  1.1187297f,
  };
}

float linearToSRGB(float value)
{
  return value >= 0.0031308f ?
    std::pow(std::max(value, 0.0f), 1.0f / 2.4f) * 1.055f - 0.055f :
    value * 12.92f;
}

RGB compressAndEncode(RGB value)
{
  value.r = std::max(value.r, 0.0f);
  value.g = std::max(value.g, 0.0f);
  value.b = std::max(value.b, 0.0f);
  const float peak = std::max({value.r, value.g, value.b});
  if (peak > 0.0f)
  {
    const float compressed = peak < 0.75f ?
      peak : 0.75f + (peak - 0.75f) * 0.25f;
    const float scale = compressed / peak;
    value.r *= scale;
    value.g *= scale;
    value.b *= scale;
  }
  value.r = linearToSRGB(std::clamp(value.r, 0.0f, 1.0f));
  value.g = linearToSRGB(std::clamp(value.g, 0.0f, 1.0f));
  value.b = linearToSRGB(std::clamp(value.b, 0.0f, 1.0f));
  return value;
}

RGB expectedColor(const FormatCase & format, const Capture & capture,
    unsigned x, unsigned y)
{
  const RGB generated = generatedColor(x, y, kFrameSerial);
  if (!format.hdr)
    return generated;

  if (format.type == FRAME_TYPE_RGBA16F)
  {
    RGB scRGB = {
      halfToFloat(floatToHalf(generated.r * 4.0f)),
      halfToFloat(floatToHalf(generated.g * 4.0f)),
      halfToFloat(floatToHalf(generated.b * 4.0f)),
    };
    if (capture.header.flags & LG_TEST_CAPTURE_NATIVE_HDR)
      return scRGB;
    scRGB.r *= 80.0f / 250.0f;
    scRGB.g *= 80.0f / 250.0f;
    scRGB.b *= 80.0f / 250.0f;
    return compressAndEncode(scRGB);
  }

  RGB linear709 = {
    generated.r * 4.0f,
    generated.g * 4.0f,
    generated.b * 4.0f,
  };
  RGB encoded2020 = toBT2020(linear709);
  encoded2020.r = quantizePQ(encoded2020.r);
  encoded2020.g = quantizePQ(encoded2020.g);
  encoded2020.b = quantizePQ(encoded2020.b);
  if (capture.header.flags & LG_TEST_CAPTURE_NATIVE_HDR)
    return encoded2020;

  RGB linear2020 = {
    pqToLinear(encoded2020.r) * (10000.0f / 250.0f),
    pqToLinear(encoded2020.g) * (10000.0f / 250.0f),
    pqToLinear(encoded2020.b) * (10000.0f / 250.0f),
  };
  return compressAndEncode(toBT709(linear2020));
}

RGB capturedColor(const Capture & capture, unsigned x, unsigned y)
{
  const bool bottomUp =
    capture.header.flags & LG_TEST_CAPTURE_BOTTOM_UP;
  const unsigned row = bottomUp ? capture.header.height - 1 - y : y;
  const uint8_t * pixel =
    capture.data.data() + row * capture.header.stride;

  switch (capture.header.captureFormat)
  {
    case LG_TEST_CAPTURE_RGBA8:
      pixel += x * 4;
      return {
        pixel[0] / 255.0f,
        pixel[1] / 255.0f,
        pixel[2] / 255.0f,
      };

    case LG_TEST_CAPTURE_RGB10_A2:
    {
      uint32_t packed;
      std::memcpy(&packed, pixel + x * sizeof(packed), sizeof(packed));
      return {
        static_cast<float>( packed        & 0x3ff) / 1023.0f,
        static_cast<float>((packed >> 10) & 0x3ff) / 1023.0f,
        static_cast<float>((packed >> 20) & 0x3ff) / 1023.0f,
      };
    }

    case LG_TEST_CAPTURE_RGBA32F:
    {
      std::array<float, 4> value;
      std::memcpy(value.data(), pixel + x * sizeof(value), sizeof(value));
      return {value[0], value[1], value[2]};
    }

    default:
      ADD_FAILURE() << "Unknown capture format: " <<
        capture.header.captureFormat;
      return {};
  }
}

void compareReference(const FormatCase & format, const Capture & capture)
{
  float maxError = 0.0f;
  unsigned maxX = 0;
  unsigned maxY = 0;
  RGB maxActual {};
  RGB maxExpected {};
  for (unsigned y = 0; y < capture.header.height; ++y)
    for (unsigned x = 0; x < capture.header.width; ++x)
    {
      const RGB actual = capturedColor(capture, x, y);
      const RGB expected = expectedColor(format, capture, x, y);
      const float error = std::max({
        std::abs(actual.r - expected.r),
        std::abs(actual.g - expected.g),
        std::abs(actual.b - expected.b),
      });
      if (error > maxError)
      {
        maxError = error;
        maxX = x;
        maxY = y;
        maxActual = actual;
        maxExpected = expected;
      }
    }

  const float tolerance = format.hdr ? 0.012f : 0.006f;
  EXPECT_LE(maxError, tolerance)
    << "max error at (" << maxX << ", " << maxY << ")"
    << "\nactual:   " << maxActual.r << ", " << maxActual.g << ", "
    << maxActual.b
    << "\nexpected: " << maxExpected.r << ", " << maxExpected.g << ", "
    << maxExpected.b
    << "\ncapture retained at: " << capture.path;
}

using RenderCase = std::tuple<FormatCase, DamageCase>;

class RenderPipelineTest : public testing::TestWithParam<RenderCase>
{
};

TEST_P(RenderPipelineTest, MatchesReference)
{
  const FormatCase & format = std::get<0>(GetParam());
  const DamageCase & damage = std::get<1>(GetParam());
  Capture capture = produceCapture(format, damage.name);
  ASSERT_FALSE(capture.data.empty()) << "artifacts: " << capture.directory;

  compareReference(format, capture);
  if (!testing::Test::HasFailure())
    std::filesystem::remove_all(capture.directory);
}

std::string renderCaseName(
    const testing::TestParamInfo<RenderCase> & info)
{
  return std::string(std::get<0>(info.param).name) + "_" +
    std::get<1>(info.param).name;
}

INSTANTIATE_TEST_SUITE_P(FormatDamageMatrix, RenderPipelineTest,
    testing::Combine(
      testing::Values(
        FormatCase {"bgra",    FRAME_TYPE_BGRA,    false, false},
        FormatCase {"rgba",    FRAME_TYPE_RGBA,    false, false},
        FormatCase {"bgr32",   FRAME_TYPE_BGR_32,  false, false},
        FormatCase {"rgb24",   FRAME_TYPE_RGB_24,  false, false},
        FormatCase {"rgba10",  FRAME_TYPE_RGBA10,  true,  true },
        FormatCase {"rgba16f", FRAME_TYPE_RGBA16F, true,  false}
      ),
      testing::Values(
        DamageCase {"full"},
        DamageCase {"moving"},
        DamageCase {"overlap"},
        DamageCase {"max"},
        DamageCase {"invalid"},
        DamageCase {"null"},
        DamageCase {"zero"}
      )
    ),
    renderCaseName);

using StrideRenderCase = std::tuple<StrideCase, DamageCase>;

class StrideRenderPipelineTest :
  public testing::TestWithParam<StrideRenderCase>
{
};

TEST_P(StrideRenderPipelineTest, MatchesReference)
{
  const StrideCase & stride = std::get<0>(GetParam());
  const DamageCase & damage = std::get<1>(GetParam());
  const FormatCase format = {
    "bgra", FRAME_TYPE_BGRA, false, false
  };
  Capture capture = produceCapture(format, damage.name, stride.stride);
  ASSERT_FALSE(capture.data.empty()) << "artifacts: " << capture.directory;

  const std::string clientLog = readText(capture.log);
  const std::string expectedLayout =
    "stride:" + std::to_string(stride.stride) +
    " pitch:" + std::to_string(stride.stride * 4);
  EXPECT_NE(clientLog.find(expectedLayout), std::string::npos) << clientLog;

  compareReference(format, capture);
  if (!testing::Test::HasFailure())
    std::filesystem::remove_all(capture.directory);
}

std::string strideRenderCaseName(
    const testing::TestParamInfo<StrideRenderCase> & info)
{
  return std::string(std::get<0>(info.param).name) + "_" +
    std::get<1>(info.param).name;
}

INSTANTIATE_TEST_SUITE_P(BGRAPaddedStride, StrideRenderPipelineTest,
    testing::Combine(
      testing::Values(
        StrideCase {"stride128",  128},
        StrideCase {"stride1280", 1280}
      ),
      testing::Values(
        DamageCase {"full"},
        DamageCase {"moving"}
      )
    ),
    strideRenderCaseName);

} // namespace
