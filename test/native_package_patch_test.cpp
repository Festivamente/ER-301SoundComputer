#include "NativePackageMachO.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace
{
#pragma pack(push, 1)
  struct MachHeader64
  {
    uint32_t magic;
    uint32_t cpuType;
    uint32_t cpuSubtype;
    uint32_t fileType;
    uint32_t commandCount;
    uint32_t commandBytes;
    uint32_t flags;
    uint32_t reserved;
  };

  struct LoadCommand
  {
    uint32_t command;
    uint32_t commandBytes;
  };

  struct DyldInfoCommand
  {
    uint32_t command;
    uint32_t commandBytes;
    uint32_t rebaseOffset;
    uint32_t rebaseSize;
    uint32_t bindOffset;
    uint32_t bindSize;
    uint32_t weakBindOffset;
    uint32_t weakBindSize;
    uint32_t lazyBindOffset;
    uint32_t lazyBindSize;
    uint32_t exportOffset;
    uint32_t exportSize;
  };
#pragma pack(pop)

  static bool readULEB(const std::vector<uint8_t> &bytes,
                       std::size_t end, std::size_t &cursor)
  {
    unsigned shift = 0;
    while (cursor < end && shift < 64)
    {
      const uint8_t byte = bytes[cursor++];
      if ((byte & 0x80U) == 0)
        return true;
      shift += 7;
    }
    return false;
  }

  static bool readCString(const std::vector<uint8_t> &bytes,
                          std::size_t end, std::size_t &cursor)
  {
    while (cursor < end)
      if (bytes[cursor++] == 0)
        return true;
    return false;
  }

  static bool validateDarwinLazyBindGrammar(const std::string &path,
                                             std::string &error)
  {
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input)
    {
      error = "could not open patched Mach-O";
      return false;
    }
    input.seekg(0, std::ios::end);
    const std::streamoff length = input.tellg();
    input.seekg(0, std::ios::beg);
    if (length <= 0)
    {
      error = "patched Mach-O is empty";
      return false;
    }
    std::vector<uint8_t> bytes(static_cast<std::size_t>(length));
    input.read(reinterpret_cast<char *>(&bytes[0]), length);
    if (!input || bytes.size() < sizeof(MachHeader64))
    {
      error = "could not read patched Mach-O";
      return false;
    }

    const MachHeader64 *header =
        reinterpret_cast<const MachHeader64 *>(&bytes[0]);
    std::size_t commandOffset = sizeof(MachHeader64);
    const DyldInfoCommand *dyldInfo = 0;
    for (uint32_t i = 0; i < header->commandCount; ++i)
    {
      if (commandOffset + sizeof(LoadCommand) > bytes.size())
      {
        error = "malformed Mach-O load commands";
        return false;
      }
      const LoadCommand *command = reinterpret_cast<const LoadCommand *>(
          &bytes[commandOffset]);
      if (command->commandBytes < sizeof(LoadCommand) ||
          commandOffset + command->commandBytes > bytes.size())
      {
        error = "malformed Mach-O load command";
        return false;
      }
      if (command->command == 0x22U || command->command == 0x80000022U)
        dyldInfo = reinterpret_cast<const DyldInfoCommand *>(command);
      commandOffset += command->commandBytes;
    }
    if (!dyldInfo ||
        static_cast<uint64_t>(dyldInfo->lazyBindOffset) +
                dyldInfo->lazyBindSize >
            bytes.size())
    {
      error = "missing or invalid lazy-bind stream";
      return false;
    }

    std::size_t cursor = dyldInfo->lazyBindOffset;
    const std::size_t end = cursor + dyldInfo->lazyBindSize;
    while (cursor < end)
    {
      const uint8_t byte = bytes[cursor++];
      const uint8_t opcode = byte & 0xf0U;
      switch (opcode)
      {
      case 0x00U: // DONE / padding
      case 0x10U: // SET_DYLIB_ORDINAL_IMM
      case 0x30U: // SET_DYLIB_SPECIAL_IMM
      case 0x90U: // DO_BIND
        break;
      case 0x20U: // SET_DYLIB_ORDINAL_ULEB
      case 0x60U: // SET_ADDEND_SLEB
      case 0x70U: // SET_SEGMENT_AND_OFFSET_ULEB
        if (!readULEB(bytes, end, cursor))
        {
          error = "malformed lazy-bind ULEB/SLEB";
          return false;
        }
        break;
      case 0x40U: // SET_SYMBOL_TRAILING_FLAGS_IMM
        if (!readCString(bytes, end, cursor))
        {
          error = "malformed lazy-bind symbol";
          return false;
        }
        break;
      case 0x50U:
        error = "lazy-bind stream contains forbidden SET_TYPE_IMM (0x50)";
        return false;
      default:
        error = "lazy-bind stream contains unsupported opcode";
        return false;
      }
    }
    return true;
  }
}

int main(int argc, char **argv)
{
  if (argc != 4)
  {
    std::cerr << "usage: native_package_patch_test INPUT OUTPUT ENGINE_IMAGE\n";
    return 2;
  }

  const emu::NativeSymbolResolver resolver = [](const std::string &symbol) {
    return symbol.compare(0, 7, "__ZN2od") == 0 ||
           symbol.compare(0, 8, "__ZTIN2od") == 0 ||
           symbol.compare(0, 4, "_lua") == 0 ||
           symbol == "_globalConfig" ||
           symbol.compare(0, 18, "_soundcomputer_compat_") == 0;
  };

  std::string error;
  if (!emu::patchNativePackageMachO(argv[1], argv[2], argv[3], resolver,
                                    error))
  {
    std::cerr << error << "\n";
    return 1;
  }
  if (!validateDarwinLazyBindGrammar(argv[2], error))
  {
    std::cerr << error << "\n";
    return 1;
  }
  std::cout << "native_package_patch_test: PASS\n";
  return 0;
}
