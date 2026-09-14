#include "NativePackageMachO.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace
{
  const uint32_t MH_MAGIC_64 = 0xfeedfacfU;
  const uint32_t CPU_TYPE_ARM64 = 0x0100000cU;
  const uint32_t LC_SEGMENT_64 = 0x19U;
  const uint32_t LC_LOAD_DYLIB = 0x0cU;
  const uint32_t LC_LOAD_WEAK_DYLIB = 0x80000018U;
  const uint32_t LC_REEXPORT_DYLIB = 0x8000001fU;
  const uint32_t LC_LAZY_LOAD_DYLIB = 0x20U;
  const uint32_t LC_LOAD_UPWARD_DYLIB = 0x80000023U;
  const uint32_t LC_DYLD_INFO = 0x22U;
  const uint32_t LC_DYLD_INFO_ONLY = 0x80000022U;
  const uint32_t LC_CODE_SIGNATURE = 0x1dU;

  const uint8_t BIND_OPCODE_MASK = 0xf0U;
  const uint8_t BIND_IMMEDIATE_MASK = 0x0fU;
  const uint8_t BIND_OPCODE_DONE = 0x00U;
  const uint8_t BIND_OPCODE_SET_DYLIB_ORDINAL_IMM = 0x10U;
  const uint8_t BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB = 0x20U;
  const uint8_t BIND_OPCODE_SET_DYLIB_SPECIAL_IMM = 0x30U;
  const uint8_t BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM = 0x40U;
  const uint8_t BIND_OPCODE_SET_TYPE_IMM = 0x50U;
  const uint8_t BIND_OPCODE_SET_ADDEND_SLEB = 0x60U;
  const uint8_t BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB = 0x70U;
  const uint8_t BIND_OPCODE_ADD_ADDR_ULEB = 0x80U;
  const uint8_t BIND_OPCODE_DO_BIND = 0x90U;
  const uint8_t BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB = 0xa0U;
  const uint8_t BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED = 0xb0U;
  const uint8_t BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB = 0xc0U;
  const int BIND_SPECIAL_DYLIB_FLAT_LOOKUP = -2;
  const uint8_t BIND_TYPE_POINTER = 1U;

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

  struct SegmentCommand64
  {
    uint32_t command;
    uint32_t commandBytes;
    char segmentName[16];
    uint64_t virtualAddress;
    uint64_t virtualSize;
    uint64_t fileOffset;
    uint64_t fileSize;
    uint32_t maxProtection;
    uint32_t initialProtection;
    uint32_t sectionCount;
    uint32_t flags;
  };

  struct Section64
  {
    char sectionName[16];
    char segmentName[16];
    uint64_t address;
    uint64_t size;
    uint32_t offset;
    uint32_t alignment;
    uint32_t relocationOffset;
    uint32_t relocationCount;
    uint32_t flags;
    uint32_t reserved1;
    uint32_t reserved2;
    uint32_t reserved3;
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

  struct DylibCommand
  {
    uint32_t command;
    uint32_t commandBytes;
    uint32_t nameOffset;
    uint32_t timestamp;
    uint32_t currentVersion;
    uint32_t compatibilityVersion;
  };
#pragma pack(pop)

  struct BindAction
  {
    uint8_t segment;
    uint64_t offset;
    int ordinal;
    uint8_t type;
    int64_t addend;
    uint8_t symbolFlags;
    std::string symbol;
  };

  static bool checkedRange(std::size_t offset, std::size_t length,
                           std::size_t total)
  {
    return offset <= total && length <= total - offset;
  }

  template <typename T>
  static T *objectAt(std::vector<uint8_t> &bytes, std::size_t offset)
  {
    if (!checkedRange(offset, sizeof(T), bytes.size()))
      return 0;
    return reinterpret_cast<T *>(&bytes[offset]);
  }

  template <typename T>
  static const T *objectAt(const std::vector<uint8_t> &bytes,
                           std::size_t offset)
  {
    if (!checkedRange(offset, sizeof(T), bytes.size()))
      return 0;
    return reinterpret_cast<const T *>(&bytes[offset]);
  }

  static bool readFile(const std::string &path, std::vector<uint8_t> &bytes,
                       std::string &error)
  {
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input)
    {
      error = "Could not open native package image: " + path;
      return false;
    }
    input.seekg(0, std::ios::end);
    const std::streamoff length = input.tellg();
    if (length <= 0 || static_cast<uint64_t>(length) >
                           static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
    {
      error = "Native package image has an invalid size: " + path;
      return false;
    }
    input.seekg(0, std::ios::beg);
    bytes.resize(static_cast<std::size_t>(length));
    input.read(reinterpret_cast<char *>(&bytes[0]), length);
    if (!input)
    {
      error = "Could not read native package image: " + path;
      return false;
    }
    return true;
  }

  static bool writeFileAtomically(const std::string &path,
                                  const std::vector<uint8_t> &bytes,
                                  std::string &error)
  {
    const std::string temporary = path + ".tmp";
    std::remove(temporary.c_str());
    {
      std::ofstream output(temporary.c_str(),
                           std::ios::binary | std::ios::trunc);
      if (!output)
      {
        error = "Could not create patched native package image: " + temporary;
        return false;
      }
      output.write(reinterpret_cast<const char *>(&bytes[0]), bytes.size());
      output.flush();
      if (!output)
      {
        error = "Could not write patched native package image: " + temporary;
        std::remove(temporary.c_str());
        return false;
      }
    }
    std::remove(path.c_str());
    if (std::rename(temporary.c_str(), path.c_str()) != 0)
    {
      error = "Could not finalize patched native package image: " +
              std::string(std::strerror(errno));
      std::remove(temporary.c_str());
      return false;
    }
    return true;
  }

  static bool readULEB(const uint8_t *data, std::size_t size,
                       std::size_t &cursor, uint64_t &value)
  {
    value = 0;
    unsigned shift = 0;
    while (cursor < size && shift < 64)
    {
      const uint8_t byte = data[cursor++];
      value |= static_cast<uint64_t>(byte & 0x7fU) << shift;
      if ((byte & 0x80U) == 0)
        return true;
      shift += 7;
    }
    return false;
  }

  static bool readSLEB(const uint8_t *data, std::size_t size,
                       std::size_t &cursor, int64_t &value)
  {
    value = 0;
    unsigned shift = 0;
    uint8_t byte = 0;
    while (cursor < size && shift < 64)
    {
      byte = data[cursor++];
      value |= static_cast<int64_t>(byte & 0x7fU) << shift;
      shift += 7;
      if ((byte & 0x80U) == 0)
      {
        if (shift < 64 && (byte & 0x40U))
          value |= -((int64_t)1 << shift);
        return true;
      }
    }
    return false;
  }

  static void appendULEB(std::vector<uint8_t> &output, uint64_t value)
  {
    do
    {
      uint8_t byte = static_cast<uint8_t>(value & 0x7fU);
      value >>= 7;
      if (value)
        byte |= 0x80U;
      output.push_back(byte);
    } while (value);
  }

  static void appendSLEB(std::vector<uint8_t> &output, int64_t value)
  {
    bool more = true;
    while (more)
    {
      uint8_t byte = static_cast<uint8_t>(value & 0x7f);
      const bool sign = (byte & 0x40U) != 0;
      value >>= 7;
      if ((value == 0 && !sign) || (value == -1 && sign))
        more = false;
      else
        byte |= 0x80U;
      output.push_back(byte);
    }
  }

  static bool decodeBindActions(const uint8_t *data, std::size_t size,
                                std::vector<BindAction> &actions,
                                std::string &error)
  {
    std::size_t cursor = 0;
    int ordinal = 0;
    uint8_t type = BIND_TYPE_POINTER;
    int64_t addend = 0;
    uint8_t symbolFlags = 0;
    uint8_t segment = 0;
    uint64_t offset = 0;
    std::string symbol;
    const uint64_t pointerSize = 8;

    while (cursor < size)
    {
      const uint8_t byte = data[cursor++];
      const uint8_t opcode = byte & BIND_OPCODE_MASK;
      const uint8_t immediate = byte & BIND_IMMEDIATE_MASK;
      uint64_t uleb = 0;
      int64_t sleb = 0;

      switch (opcode)
      {
      case BIND_OPCODE_DONE:
        break;
      case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM:
        ordinal = immediate;
        break;
      case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB:
        if (!readULEB(data, size, cursor, uleb))
          goto malformed;
        ordinal = static_cast<int>(uleb);
        break;
      case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM:
        ordinal = immediate == 0 ? 0
                                 : static_cast<int8_t>(immediate | 0xf0U);
        break;
      case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM:
      {
        const std::size_t start = cursor;
        while (cursor < size && data[cursor] != 0)
          ++cursor;
        if (cursor >= size)
          goto malformed;
        symbol.assign(reinterpret_cast<const char *>(data + start),
                      cursor - start);
        ++cursor;
        symbolFlags = immediate;
        break;
      }
      case BIND_OPCODE_SET_TYPE_IMM:
        type = immediate;
        break;
      case BIND_OPCODE_SET_ADDEND_SLEB:
        if (!readSLEB(data, size, cursor, sleb))
          goto malformed;
        addend = sleb;
        break;
      case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
        if (!readULEB(data, size, cursor, uleb))
          goto malformed;
        segment = immediate;
        offset = uleb;
        break;
      case BIND_OPCODE_ADD_ADDR_ULEB:
        if (!readULEB(data, size, cursor, uleb))
          goto malformed;
        offset += uleb;
        break;
      case BIND_OPCODE_DO_BIND:
        actions.push_back(BindAction{segment, offset, ordinal, type, addend,
                                     symbolFlags, symbol});
        offset += pointerSize;
        break;
      case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB:
        actions.push_back(BindAction{segment, offset, ordinal, type, addend,
                                     symbolFlags, symbol});
        if (!readULEB(data, size, cursor, uleb))
          goto malformed;
        offset += pointerSize + uleb;
        break;
      case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED:
        actions.push_back(BindAction{segment, offset, ordinal, type, addend,
                                     symbolFlags, symbol});
        offset += pointerSize + immediate * pointerSize;
        break;
      case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB:
      {
        uint64_t count = 0;
        uint64_t skip = 0;
        if (!readULEB(data, size, cursor, count) ||
            !readULEB(data, size, cursor, skip))
          goto malformed;
        for (uint64_t i = 0; i < count; ++i)
        {
          actions.push_back(BindAction{segment, offset, ordinal, type, addend,
                                       symbolFlags, symbol});
          offset += pointerSize + skip;
        }
        break;
      }
      default:
        goto malformed;
      }
    }
    return true;

  malformed:
    error = "Malformed Mach-O bind opcode stream.";
    return false;
  }

  static std::string remapLegacyGnuAbiSymbol(const std::string &symbol)
  {
    if (symbol ==
        "__ZN2od5InletC1ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE" ||
        symbol ==
        "__ZN2od5InletC2ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE")
      return "_soundcomputer_compat_inlet_ctor";
    if (symbol ==
        "__ZN2od6OutletC1ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE" ||
        symbol ==
        "__ZN2od6OutletC2ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE")
      return "_soundcomputer_compat_outlet_ctor";
    if (symbol ==
        "__ZN2od5InletC1ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEj" ||
        symbol ==
        "__ZN2od5InletC2ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEj")
      return "_soundcomputer_compat_inlet_indexed_ctor";
    if (symbol ==
        "__ZN2od6OutletC1ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEj" ||
        symbol ==
        "__ZN2od6OutletC2ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEj")
      return "_soundcomputer_compat_outlet_indexed_ctor";
    if (symbol ==
        "__ZN2od9ParameterC1ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEf" ||
        symbol ==
        "__ZN2od9ParameterC2ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEf")
      return "_soundcomputer_compat_parameter_ctor";
    if (symbol ==
        "__ZN2od6OptionC1ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEi" ||
        symbol ==
        "__ZN2od6OptionC2ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEi")
      return "_soundcomputer_compat_option_value_ctor";
    if (symbol ==
        "__ZN2od6OptionC1ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE" ||
        symbol ==
        "__ZN2od6OptionC2ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE")
      return "_soundcomputer_compat_option_ctor";
    return std::string();
  }

  static bool isProcessRuntimeSymbol(const std::string &symbol)
  {
    return symbol == "_main" ||
           symbol.compare(0, 5, "__Zdl") == 0 ||
           symbol.compare(0, 4, "__Zn") == 0 ||
           symbol.compare(0, 7, "___gxx_") == 0 ||
           symbol.compare(0, 11, "__ZTVN10__c") == 0 ||
           symbol.compare(0, 11, "__ZTIN10__c") == 0 ||
           symbol.compare(0, 9, "__Unwind_") == 0;
  }

  static bool transformAction(BindAction &action, int engineOrdinal,
                              const emu::NativeSymbolResolver &engineDefines,
                              std::string &error)
  {
    if (action.ordinal != BIND_SPECIAL_DYLIB_FLAT_LOOKUP)
      return true;

    const std::string remapped = remapLegacyGnuAbiSymbol(action.symbol);
    if (!remapped.empty())
    {
      action.symbol = remapped;
      action.ordinal = engineOrdinal;
      return true;
    }

    if (action.symbol.find("St7__cxx1112basic_string") != std::string::npos)
    {
      error = "Native package uses an unsupported GNU std::string ABI symbol: " +
              action.symbol +
              ". Rebuild this package with the ER-301 Sound Computer Darwin clang/libc++ "
              "profile.";
      return false;
    }

    if (!isProcessRuntimeSymbol(action.symbol) && engineDefines &&
        engineDefines(action.symbol))
      action.ordinal = engineOrdinal;
    return true;
  }

  static void appendOrdinal(std::vector<uint8_t> &output, int ordinal)
  {
    if (ordinal >= 0 && ordinal <= 15)
    {
      output.push_back(BIND_OPCODE_SET_DYLIB_ORDINAL_IMM |
                       static_cast<uint8_t>(ordinal));
    }
    else if (ordinal > 15)
    {
      output.push_back(BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB);
      appendULEB(output, static_cast<uint64_t>(ordinal));
    }
    else
    {
      output.push_back(BIND_OPCODE_SET_DYLIB_SPECIAL_IMM |
                       (static_cast<uint8_t>(ordinal) & 0x0fU));
    }
  }

  static std::vector<uint8_t> encodeBindActions(
      const std::vector<BindAction> &actions, bool lazy)
  {
    std::vector<uint8_t> output;
    for (std::size_t i = 0; i < actions.size(); ++i)
    {
      const BindAction &action = actions[i];
      output.push_back(BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB |
                       (action.segment & 0x0fU));
      appendULEB(output, action.offset);
      appendOrdinal(output, action.ordinal);
      output.push_back(BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM |
                       (action.symbolFlags & 0x0fU));
      output.insert(output.end(), action.symbol.begin(), action.symbol.end());
      output.push_back(0);
      // Lazy-bind streams have a smaller opcode grammar than regular bind
      // streams. In particular, dyld rejects SET_TYPE_IMM (0x50) in a lazy
      // entry even though LLVM's Mach-O dumper accepts it. The bind type is
      // implicitly BIND_TYPE_POINTER for lazy symbols. Emit SET_TYPE_IMM only
      // for the normal bind stream and emit an addend in the lazy stream only
      // when it is actually non-zero.
      if (!lazy)
      {
        output.push_back(BIND_OPCODE_SET_TYPE_IMM | (action.type & 0x0fU));
        output.push_back(BIND_OPCODE_SET_ADDEND_SLEB);
        appendSLEB(output, action.addend);
      }
      else if (action.addend != 0)
      {
        output.push_back(BIND_OPCODE_SET_ADDEND_SLEB);
        appendSLEB(output, action.addend);
      }
      output.push_back(BIND_OPCODE_DO_BIND);
      if (lazy)
        output.push_back(BIND_OPCODE_DONE);
    }
    if (!lazy)
      output.push_back(BIND_OPCODE_DONE);
    while ((output.size() & 7U) != 0)
      output.push_back(0);
    return output;
  }

  static bool isDylibLoadCommand(uint32_t command)
  {
    return command == LC_LOAD_DYLIB || command == LC_LOAD_WEAK_DYLIB ||
           command == LC_REEXPORT_DYLIB || command == LC_LAZY_LOAD_DYLIB ||
           command == LC_LOAD_UPWARD_DYLIB;
  }

  static uint64_t alignUp(uint64_t value, uint64_t alignment)
  {
    return (value + alignment - 1) & ~(alignment - 1);
  }

  static std::vector<uint8_t> makeDylibCommand(const std::string &path)
  {
    const std::size_t rawSize = sizeof(DylibCommand) + path.size() + 1;
    const std::size_t commandSize = static_cast<std::size_t>(alignUp(rawSize, 8));
    std::vector<uint8_t> bytes(commandSize, 0);
    DylibCommand *command = reinterpret_cast<DylibCommand *>(&bytes[0]);
    command->command = LC_LOAD_DYLIB;
    command->commandBytes = static_cast<uint32_t>(commandSize);
    command->nameOffset = sizeof(DylibCommand);
    command->timestamp = 2;
    command->currentVersion = 0x00010000U;
    command->compatibilityVersion = 0x00010000U;
    std::memcpy(&bytes[sizeof(DylibCommand)], path.c_str(), path.size() + 1);
    return bytes;
  }
}

namespace emu
{
  bool patchNativePackageMachO(const std::string &sourcePath,
                               const std::string &destinationPath,
                               const std::string &engineImagePath,
                               const NativeSymbolResolver &engineDefines,
                               std::string &error)
  {
    std::vector<uint8_t> original;
    if (!readFile(sourcePath, original, error))
      return false;

    const MachHeader64 *header = objectAt<MachHeader64>(original, 0);
    if (!header || header->magic != MH_MAGIC_64)
    {
      error = "Native package is not a 64-bit little-endian Mach-O image: " +
              sourcePath;
      return false;
    }
    if (header->cpuType != CPU_TYPE_ARM64)
    {
      error = "Native package is not an Apple Silicon arm64 image: " +
              sourcePath;
      return false;
    }
    if (!checkedRange(sizeof(MachHeader64), header->commandBytes,
                      original.size()))
    {
      error = "Native package has malformed Mach-O load commands.";
      return false;
    }

    std::size_t commandOffset = sizeof(MachHeader64);
    std::size_t minimumSectionOffset = original.size();
    unsigned dylibCount = 0;
    const DyldInfoCommand *originalDyldInfo = 0;
    uint64_t codeSignatureOffset = original.size();
    uint64_t codeSignatureEnd = original.size();

    for (uint32_t i = 0; i < header->commandCount; ++i)
    {
      const LoadCommand *command = objectAt<LoadCommand>(original, commandOffset);
      if (!command || command->commandBytes < sizeof(LoadCommand) ||
          !checkedRange(commandOffset, command->commandBytes, original.size()))
      {
        error = "Native package has a malformed Mach-O load command.";
        return false;
      }
      if (isDylibLoadCommand(command->command))
        ++dylibCount;
      if (command->command == LC_DYLD_INFO ||
          command->command == LC_DYLD_INFO_ONLY)
        originalDyldInfo = reinterpret_cast<const DyldInfoCommand *>(command);
      if (command->command == LC_CODE_SIGNATURE &&
          command->commandBytes >= 16)
      {
        const uint32_t *fields = reinterpret_cast<const uint32_t *>(command);
        if (fields[2] > 0 && fields[2] < codeSignatureOffset)
        {
          codeSignatureOffset = fields[2];
          codeSignatureEnd = static_cast<uint64_t>(fields[2]) + fields[3];
        }
      }
      if (command->command == LC_SEGMENT_64 &&
          command->commandBytes >= sizeof(SegmentCommand64))
      {
        const SegmentCommand64 *segment =
            reinterpret_cast<const SegmentCommand64 *>(command);
        const std::size_t sectionsOffset = commandOffset + sizeof(SegmentCommand64);
        for (uint32_t sectionIndex = 0;
             sectionIndex < segment->sectionCount; ++sectionIndex)
        {
          const Section64 *section = objectAt<Section64>(
              original, sectionsOffset + sectionIndex * sizeof(Section64));
          if (!section)
          {
            error = "Native package has a malformed Mach-O section table.";
            return false;
          }
          if (section->offset > 0 && section->offset < minimumSectionOffset)
            minimumSectionOffset = section->offset;
        }
      }
      commandOffset += command->commandBytes;
    }

    if (!originalDyldInfo)
    {
      error = "Native package has no dyld binding information.";
      return false;
    }
    if (minimumSectionOffset == original.size())
    {
      error = "Native package has no file-backed Mach-O sections.";
      return false;
    }
    if (!checkedRange(originalDyldInfo->bindOffset,
                      originalDyldInfo->bindSize, original.size()) ||
        !checkedRange(originalDyldInfo->lazyBindOffset,
                      originalDyldInfo->lazyBindSize, original.size()))
    {
      error = "Native package has invalid dyld bind ranges.";
      return false;
    }

    std::vector<BindAction> bindActions;
    std::vector<BindAction> lazyActions;
    if (!decodeBindActions(&original[originalDyldInfo->bindOffset],
                           originalDyldInfo->bindSize, bindActions, error) ||
        !decodeBindActions(&original[originalDyldInfo->lazyBindOffset],
                           originalDyldInfo->lazyBindSize, lazyActions, error))
      return false;

    const int engineOrdinal = static_cast<int>(dylibCount + 1);
    for (std::size_t i = 0; i < bindActions.size(); ++i)
      if (!transformAction(bindActions[i], engineOrdinal, engineDefines, error))
        return false;
    for (std::size_t i = 0; i < lazyActions.size(); ++i)
      if (!transformAction(lazyActions[i], engineOrdinal, engineDefines, error))
        return false;

    const std::vector<uint8_t> newBind = encodeBindActions(bindActions, false);
    const std::vector<uint8_t> newLazyBind = encodeBindActions(lazyActions, true);

    std::vector<std::vector<uint8_t> > commands;
    commandOffset = sizeof(MachHeader64);
    for (uint32_t i = 0; i < header->commandCount; ++i)
    {
      const LoadCommand *command = objectAt<LoadCommand>(original, commandOffset);
      if (command->command != LC_CODE_SIGNATURE)
      {
        commands.push_back(std::vector<uint8_t>(
            original.begin() + commandOffset,
            original.begin() + commandOffset + command->commandBytes));
      }
      commandOffset += command->commandBytes;
    }
    commands.push_back(makeDylibCommand(engineImagePath));

    std::size_t newCommandBytes = 0;
    for (std::size_t i = 0; i < commands.size(); ++i)
      newCommandBytes += commands[i].size();
    if (sizeof(MachHeader64) + newCommandBytes > minimumSectionOffset)
    {
      error = "Native package Mach-O header has no room for the ER-301 Sound Computer engine "
              "dependency.";
      return false;
    }

    if (codeSignatureOffset > original.size())
      codeSignatureOffset = original.size();
    // Code signatures are normally the final linkedit blob. Truncate only in
    // that common case; otherwise retain any later data and merely remove the
    // signature load command before appending new bind streams.
    const std::size_t preservedSize =
        codeSignatureEnd == original.size()
            ? static_cast<std::size_t>(codeSignatureOffset)
            : original.size();
    std::vector<uint8_t> patched(original.begin(),
                                 original.begin() + preservedSize);
    patched.resize(static_cast<std::size_t>(alignUp(patched.size(), 8)), 0);
    const uint32_t newBindOffset = static_cast<uint32_t>(patched.size());
    patched.insert(patched.end(), newBind.begin(), newBind.end());
    const uint32_t newLazyBindOffset = static_cast<uint32_t>(patched.size());
    patched.insert(patched.end(), newLazyBind.begin(), newLazyBind.end());

    MachHeader64 *newHeader = objectAt<MachHeader64>(patched, 0);
    newHeader->commandCount = static_cast<uint32_t>(commands.size());
    newHeader->commandBytes = static_cast<uint32_t>(newCommandBytes);
    std::fill(patched.begin() + sizeof(MachHeader64),
              patched.begin() + minimumSectionOffset, 0);
    std::size_t outputCommandOffset = sizeof(MachHeader64);
    for (std::size_t i = 0; i < commands.size(); ++i)
    {
      std::copy(commands[i].begin(), commands[i].end(),
                patched.begin() + outputCommandOffset);
      LoadCommand *command = objectAt<LoadCommand>(patched, outputCommandOffset);
      if (command->command == LC_DYLD_INFO ||
          command->command == LC_DYLD_INFO_ONLY)
      {
        DyldInfoCommand *dyldInfo =
            reinterpret_cast<DyldInfoCommand *>(command);
        dyldInfo->bindOffset = newBindOffset;
        dyldInfo->bindSize = static_cast<uint32_t>(newBind.size());
        dyldInfo->lazyBindOffset = newLazyBindOffset;
        dyldInfo->lazyBindSize = static_cast<uint32_t>(newLazyBind.size());
      }
      if (command->command == LC_SEGMENT_64 &&
          command->commandBytes >= sizeof(SegmentCommand64))
      {
        SegmentCommand64 *segment =
            reinterpret_cast<SegmentCommand64 *>(command);
        if (std::strncmp(segment->segmentName, "__LINKEDIT", 16) == 0)
        {
          if (segment->fileOffset > patched.size())
          {
            error = "Native package has an invalid __LINKEDIT segment.";
            return false;
          }
          segment->fileSize = patched.size() - segment->fileOffset;
          segment->virtualSize = alignUp(segment->fileSize, 0x4000U);
        }
      }
      outputCommandOffset += commands[i].size();
    }

    return writeFileAtomically(destinationPath, patched, error);
  }
}
