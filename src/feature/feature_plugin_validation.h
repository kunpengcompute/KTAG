/* Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved. */
#ifndef KTAG_FEATURE_PLUGIN_VALIDATION_H
#define KTAG_FEATURE_PLUGIN_VALIDATION_H

#include <elf.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>

namespace ktag_feature_plugin_internal {

// Check file bounds before dlopen can map a truncated object or run constructors.
// This is not a sandbox or a guarantee that plugin code is safe to execute.
template <class Header, class Program, class Section>
inline bool validate_elf(std::ifstream &input, uint64_t size,
                         std::string *reason) {
    const auto fail = [&](const char *message) {
        *reason = message;
        return false;
    };
    const auto contains = [size](uint64_t offset, uint64_t length) {
        return offset <= size && length <= size - offset;
    };
    const auto read = [&](uint64_t offset, void *value, size_t length) {
        if (!contains(offset, length)) return false;
        input.seekg(static_cast<std::streamoff>(offset));
        input.read(static_cast<char *>(value), length);
        return static_cast<bool>(input);
    };
    Header header{};
    if (!read(0U, &header, sizeof(header))) return fail("ELF header is truncated");
    if (header.e_type != ET_DYN || header.e_version != EV_CURRENT ||
        header.e_ehsize != sizeof(Header)) {
        return fail("invalid ELF shared-object header");
    }

    uint64_t sections = header.e_shnum;
    uint64_t programs = header.e_phnum;
    if (header.e_shoff != 0U) {
        Section first{};
        if (header.e_shentsize != sizeof(Section) ||
            !read(header.e_shoff, &first, sizeof(first))) {
            return fail("ELF section header table is truncated or invalid");
        }
        // ELF uses section zero to store extended header counts.
        if (sections == 0U) sections = first.sh_size;
        if (programs == PN_XNUM) programs = first.sh_info;
        if (sections == 0U ||
            sections > (size - header.e_shoff) / sizeof(Section)) {
            return fail("ELF section header table exceeds file size (truncated file)");
        }
    } else if (sections != 0U || programs == PN_XNUM) {
        return fail("invalid ELF section header table");
    }

    if (programs == 0U || header.e_phentsize != sizeof(Program) ||
        header.e_phoff < sizeof(Header) || header.e_phoff > size ||
        programs > (size - header.e_phoff) / sizeof(Program)) {
        return fail("ELF program header table is truncated or invalid");
    }
    for (uint64_t i = 0U; i < programs; ++i) {
        Program program{};
        if (!read(header.e_phoff + i * sizeof(Program), &program, sizeof(program)) ||
            (program.p_filesz != 0U && !contains(program.p_offset, program.p_filesz))) {
            return fail("ELF segment exceeds file size (truncated file)");
        }
        if (program.p_type == PT_LOAD && program.p_filesz > program.p_memsz) {
            return fail("invalid ELF load segment size");
        }
    }
    for (uint64_t i = 0U; i < sections; ++i) {
        Section section{};
        if (!read(header.e_shoff + i * sizeof(Section), &section, sizeof(section))) {
            return fail("ELF section header table is truncated");
        }
        // .bss occupies memory, not file bytes; section zero holds count metadata.
        if (section.sh_type != SHT_NOBITS && section.sh_type != SHT_NULL &&
            section.sh_size != 0U && !contains(section.sh_offset, section.sh_size)) {
            return fail("ELF section exceeds file size (truncated file)");
        }
    }
    return true;
}

inline bool validate_file(const char *path, std::string *error) {
    // A bare soname is resolved by ld.so, not relative to the current directory.
    // Keep its existing search semantics; configured plugin paths contain '/'.
    if (std::strchr(path, '/') == nullptr) return true;
    const auto fail = [&](const std::string &reason) {
        if (error != nullptr) {
            *error = std::string("invalid plugin file '") + path + "': " + reason;
        }
        return false;
    };
    errno = 0;
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        const int code = errno == 0 ? EIO : errno;
        return fail(std::string("cannot inspect file: ") + std::strerror(code));
    }
    const auto end = input.tellg();
    if (end < std::streampos(0)) return fail("cannot determine file size");
    const uint64_t size = static_cast<uint64_t>(static_cast<std::streamoff>(end));
    unsigned char ident[EI_NIDENT]{};
    input.seekg(0);
    input.read(reinterpret_cast<char *>(ident), sizeof(ident));
    if (!input) return fail("ELF identification is truncated");
    if (std::memcmp(ident, ELFMAG, SELFMAG) != 0) return fail("not an ELF file");
    const uint16_t endian = 1U;
    const auto encoding = *reinterpret_cast<const unsigned char *>(&endian) == 1U
                              ? ELFDATA2LSB : ELFDATA2MSB;
    if (ident[EI_VERSION] != EV_CURRENT || ident[EI_DATA] != encoding ||
        ident[EI_CLASS] != (sizeof(void *) == 8U ? ELFCLASS64 : ELFCLASS32)) {
        return fail("unsupported ELF class, byte order or version");
    }
    std::string reason;
    const bool valid = sizeof(void *) == 8U
        ? validate_elf<Elf64_Ehdr, Elf64_Phdr, Elf64_Shdr>(input, size, &reason)
        : validate_elf<Elf32_Ehdr, Elf32_Phdr, Elf32_Shdr>(input, size, &reason);
    return valid || fail(reason);
}

} // namespace ktag_feature_plugin_internal
#endif
