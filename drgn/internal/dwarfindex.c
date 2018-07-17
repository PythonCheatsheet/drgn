// Copyright 2018 - Omar Sandoval
// SPDX-License-Identifier: GPL-3.0+

#define PY_SSIZE_T_CLEAN

#include <assert.h>
#include <elf.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <Python.h>
#include "structmember.h"

#include "siphash.h"

enum {
	DW_TAG_class_type = 0x2,
	DW_TAG_enumeration_type = 0x4,
	DW_TAG_compile_unit = 0x11,
	DW_TAG_structure_type = 0x13,
	DW_TAG_typedef = 0x16,
	DW_TAG_union_type = 0x17,
	DW_TAG_base_type = 0x24,
	DW_TAG_enumerator = 0x28,
	DW_TAG_variable = 0x34,
};

enum {
	/* Maximum number of bits used by the tags we care about. */
	TAG_BITS = 6,
	TAG_MASK = (1 << TAG_BITS) - 1,
	/* The remaining bits can be used for other purposes. */
	TAG_FLAG_DECLARATION = 0x40,
	TAG_FLAG_CHILDREN = 0x80,
};

enum {
	DW_AT_sibling = 0x1,
	DW_AT_name = 0x3,
	DW_AT_stmt_list = 0x10,
	DW_AT_decl_file = 0x3a,
	DW_AT_declaration = 0x3c,
	DW_AT_specification = 0x47,
};

enum {
	DW_FORM_addr = 0x1,
	DW_FORM_block2 = 0x3,
	DW_FORM_block4 = 0x4,
	DW_FORM_data2 = 0x5,
	DW_FORM_data4 = 0x6,
	DW_FORM_data8 = 0x7,
	DW_FORM_string = 0x8,
	DW_FORM_block = 0x9,
	DW_FORM_block1 = 0xa,
	DW_FORM_data1 = 0xb,
	DW_FORM_flag = 0xc,
	DW_FORM_sdata = 0xd,
	DW_FORM_strp = 0xe,
	DW_FORM_udata = 0xf,
	DW_FORM_ref_addr = 0x10,
	DW_FORM_ref1 = 0x11,
	DW_FORM_ref2 = 0x12,
	DW_FORM_ref4 = 0x13,
	DW_FORM_ref8 = 0x14,
	DW_FORM_ref_udata = 0x15,
	DW_FORM_indirect = 0x16,
	DW_FORM_sec_offset = 0x17,
	DW_FORM_exprloc = 0x18,
	DW_FORM_flag_present = 0x19,
	DW_FORM_ref_sig8 = 0x20,
};

static PyObject *DwarfFile;
static PyObject *DwarfFormatError;
static PyObject *ElfFile;
static PyObject *ElfFormatError;
static PyObject *MemoryViewIO;

#define DIE_HASH_SHIFT 17
#define DIE_HASH_SIZE (1 << DIE_HASH_SHIFT)
#define DIE_HASH_MASK ((1 << DIE_HASH_SHIFT) - 1)

/* int resize_array(T **ptr, size_t n); */
#define resize_array(ptr, n) ({					\
	typeof(ptr) _ptr = ptr;					\
	size_t _n = n;						\
	int _ret;						\
								\
	if (_n > SIZE_MAX / sizeof(**_ptr)) {			\
		if (!PyErr_Occurred())				\
			PyErr_NoMemory();			\
		_ret = -1;					\
	} else {						\
		typeof(*_ptr) _tmp;				\
								\
		errno = 0;					\
		_tmp = realloc(*_ptr, _n * sizeof(**_ptr));	\
		if (errno) {					\
			if (!PyErr_Occurred())			\
				PyErr_NoMemory();		\
			_ret = -1;				\
		} else {					\
			*_ptr = _tmp;				\
			_ret = 0;				\
		}						\
	}							\
	_ret;							\
})

/*
 * XXX: hacky way to make sure everywhere we raise an exception does so with the
 * GIL held.
 */
#define PyErr_Format(...) ({				\
	PyGILState_STATE _state = PyGILState_Ensure();	\
	PyErr_Format(__VA_ARGS__);			\
	PyGILState_Release(_state);			\
	NULL;						\
})

#define PyErr_NoMemory(...) ({				\
	PyGILState_STATE _state = PyGILState_Ensure();	\
	PyErr_NoMemory(__VA_ARGS__);			\
	PyGILState_Release(_state);			\
	NULL;						\
})

#define PyErr_SetFromErrno(...) ({		\
	PyGILState_STATE _state = PyGILState_Ensure();		\
	PyErr_SetFromErrno(__VA_ARGS__);	\
	PyGILState_Release(_state);				\
	NULL;							\
})

#define PyErr_SetFromErrnoWithFilenameObject(...) ({		\
	PyGILState_STATE _state = PyGILState_Ensure();		\
	PyErr_SetFromErrnoWithFilenameObject(__VA_ARGS__);	\
	PyGILState_Release(_state);				\
	NULL;							\
})

#define PyErr_SetNone(...) ({				\
	PyGILState_STATE _state = PyGILState_Ensure();	\
	PyErr_SetNone(__VA_ARGS__);			\
	PyGILState_Release(_state);			\
	NULL;						\
})

#define PyErr_SetString(...) ({				\
	PyGILState_STATE _state = PyGILState_Ensure();	\
	PyErr_SetString(__VA_ARGS__);			\
	PyGILState_Release(_state);			\
	NULL;						\
})

enum {
	DEBUG_ABBREV,
	DEBUG_INFO,
	DEBUG_LINE,
	DEBUG_STR,
	NUM_DEBUG_SECTIONS,
};

static inline bool in_bounds(const char *ptr, const char *end, size_t size)
{
	return ptr <= end && (size_t)(end - ptr) >= size;
}

#define DEFINE_READ(size)						\
static inline int read_u##size(const char **ptr, const char *end,	\
			       uint##size##_t *value)			\
{									\
	if (!in_bounds(*ptr, end, sizeof(uint##size##_t))) {		\
		PyErr_SetNone(PyExc_EOFError);				\
		return -1;						\
	}								\
	*value = *(const uint##size##_t *)*ptr;				\
	*ptr += sizeof(uint##size##_t);					\
	return 0;							\
}									\
									\
static inline int read_u##size##_into_bool(const char **ptr,		\
					   const char *end,		\
					   bool *value)			\
{									\
	if (!in_bounds(*ptr, end, sizeof(uint##size##_t))) {		\
		PyErr_SetNone(PyExc_EOFError);				\
		return -1;						\
	}								\
	*value = !!*(const uint##size##_t *)*ptr;			\
	*ptr += sizeof(uint##size##_t);					\
	return 0;							\
}									\
									\
static inline int read_u##size##_into_u64(const char **ptr,		\
					  const char *end,		\
					  uint64_t *value)		\
{									\
	if (!in_bounds(*ptr, end, sizeof(uint##size##_t))) {		\
		PyErr_SetNone(PyExc_EOFError);				\
		return -1;						\
	}								\
	*value = *(const uint##size##_t *)*ptr;				\
	*ptr += sizeof(uint##size##_t);					\
	return 0;							\
}									\
									\
static inline int read_u##size##_into_size_t(const char **ptr,		\
					     const char *end,		\
					     size_t *value)		\
{									\
	uint##size##_t tmp;						\
									\
	if (read_u##size(ptr, end, &tmp) == -1)				\
		return -1;						\
									\
	if (tmp > SIZE_MAX) {						\
		PyErr_SetNone(PyExc_EOFError);				\
		return -1;						\
	}								\
	*value = tmp;							\
	return 0;							\
}

DEFINE_READ(8);
DEFINE_READ(16);
DEFINE_READ(32);
DEFINE_READ(64);

static inline int skip_string(const char **ptr, const char *end)
{
	const char *nul;

	if (*ptr >= end) {
		PyErr_SetNone(PyExc_EOFError);
		return -1;
	}

	nul = memchr(*ptr, 0, end - *ptr);
	if (!nul) {
		PyErr_SetNone(PyExc_EOFError);
		return -1;
	}

	*ptr = nul + 1;
	return 0;
}

static inline int read_string(const char **ptr, const char *end,
			      const char **value, size_t *len)
{
	const char *nul;

	if (*ptr >= end) {
		PyErr_SetNone(PyExc_EOFError);
		return -1;
	}

	nul = memchr(*ptr, 0, end - *ptr);
	if (!nul) {
		PyErr_SetNone(PyExc_EOFError);
		return -1;
	}

	*value = *ptr;
	*len = nul - *ptr;
	*ptr = nul + 1;
	return 0;
}

static inline int skip_leb128(const char **ptr, const char *end)
{
	for (;;) {
		if (*ptr >= end) {
			PyErr_SetNone(PyExc_EOFError);
			return -1;
		}
		if (!(*(const uint8_t *)(*ptr)++ & 0x80))
			return 0;
	}
}

static inline int read_uleb128(const char **ptr, const char *end,
			       uint64_t *value)
{
	int shift = 0;
	uint8_t byte;

	*value = 0;
	for (;;) {
		if (*ptr >= end) {
			PyErr_SetNone(PyExc_EOFError);
			return -1;
		}
		byte = *(const uint8_t *)*ptr;
		(*ptr)++;
		if (shift == 63 && byte > 1) {
			PyErr_SetString(PyExc_OverflowError,
					"ULEB128 overflowed unsigned 64-bit integer");
			return -1;
		}
		*value |= (uint64_t)(byte & 0x7f) << shift;
		shift += 7;
		if (!(byte & 0x80))
			break;
	}
	return 0;
}

static inline int read_uleb128_into_size_t(const char **ptr, const char *end,
					   size_t *value)
{
	uint64_t tmp;

	if (read_uleb128(ptr, end, &tmp) == -1)
		return -1;

	if (tmp > SIZE_MAX) {
		PyErr_SetNone(PyExc_EOFError);
		return -1;
	}
	*value = tmp;
	return 0;
}

enum {
	CMD_MAX_SKIP = 229,
	ATTRIB_BLOCK1,
	ATTRIB_BLOCK2,
	ATTRIB_BLOCK4,
	ATTRIB_EXPRLOC,
	ATTRIB_LEB128,
	ATTRIB_STRING,
	ATTRIB_SIBLING_REF1,
	ATTRIB_SIBLING_REF2,
	ATTRIB_SIBLING_REF4,
	ATTRIB_SIBLING_REF8,
	ATTRIB_SIBLING_REF_UDATA,
	ATTRIB_NAME_STRP4,
	ATTRIB_NAME_STRP8,
	ATTRIB_NAME_STRING,
	ATTRIB_STMT_LIST_LINEPTR4,
	ATTRIB_STMT_LIST_LINEPTR8,
	ATTRIB_DECL_FILE_DATA1,
	ATTRIB_DECL_FILE_DATA2,
	ATTRIB_DECL_FILE_DATA4,
	ATTRIB_DECL_FILE_DATA8,
	ATTRIB_DECL_FILE_UDATA,
	ATTRIB_SPECIFICATION_REF1,
	ATTRIB_SPECIFICATION_REF2,
	ATTRIB_SPECIFICATION_REF4,
	ATTRIB_SPECIFICATION_REF8,
	ATTRIB_SPECIFICATION_REF_UDATA,
	ATTRIB_MAX_CMD = ATTRIB_SPECIFICATION_REF_UDATA,
};

struct abbrev_table {
	/*
	 * Technically, abbreviation codes don't have to be sequential. In
	 * practice, GCC seems to always generate sequential codes starting at
	 * one, so we can get away with a flat array.
	 */
	uint32_t *decls;
	size_t num_decls;
	uint8_t *cmds;
};

struct file_name_table {
	uint64_t *file_name_hashes;
	size_t num_files;
};

struct compilation_unit {
	const char *ptr;
	uint64_t unit_length;
	uint16_t version;
	uint64_t debug_abbrev_offset;
	uint8_t address_size;
	bool is_64_bit;
	uint32_t file;
};

struct section {
	uint16_t shdr_index;
	char *buffer;
	uint64_t size;
};

struct file {
	char *map;
	size_t size;
	struct section symtab;
	struct section debug_sections[NUM_DEBUG_SECTIONS];
	struct section rela_sections[NUM_DEBUG_SECTIONS];
	/* DwarfFile object or NULL if it has not been initialized yet. */
	PyObject *obj;
	/* dict mapping cu offsets to CUs */
	PyObject *cu_objs;
	PyObject *path;
};

struct die_hash_entry {
	const char *name;
	uint64_t file_name_hash;
	uint8_t tag;
	uint32_t cu;
	const char *ptr;
};

typedef struct {
	PyObject_HEAD
	struct file *files;
	size_t num_files;
	struct compilation_unit *cus;
	size_t num_cus;
	size_t cus_capacity;
	struct die_hash_entry die_hash[DIE_HASH_SIZE];
	int address_size;
} DwarfIndex;

static void DwarfIndex_dealloc(DwarfIndex *self)
{
	size_t i;

	for (i = 0; i < self->num_files; i++) {
		munmap(self->files[i].map, self->files[i].size);
		Py_XDECREF(self->files[i].obj);
		Py_XDECREF(self->files[i].cu_objs);
		Py_XDECREF(self->files[i].path);
	}
	free(self->files);
	free(self->cus);

	Py_TYPE(self)->tp_free((PyObject *)self);
}

static int DwarfIndex_traverse(DwarfIndex *self, visitproc visit, void *arg)
{
	size_t i;

	for (i = 0; i < self->num_files; i++) {
		Py_VISIT(self->files[i].obj);
		Py_VISIT(self->files[i].cu_objs);
		Py_VISIT(self->files[i].path);
	}
	return 0;
}

static int DwarfIndex_clear(DwarfIndex *self)
{
	size_t i;

	for (i = 0; i < self->num_files; i++) {
		Py_CLEAR(self->files[i].obj);
		Py_CLEAR(self->files[i].cu_objs);
		Py_CLEAR(self->files[i].path);
	}
	return 0;
}

static int open_file(struct file *file, const char *path)
{
	int saved_errno;
	struct stat st;
	void *map;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd == -1)
		return -1;

	if (fstat(fd, &st) == -1)
		return -1;

	map = mmap(NULL, st.st_size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	if (map == MAP_FAILED) {
		saved_errno = errno;
		close(fd);
		errno = saved_errno;
		return -1;
	}

	file->map = map;
	file->size = st.st_size;

	close(fd);
	return 0;
}

static int validate_ehdr(struct file *file, const Elf64_Ehdr *ehdr)
{
	if (file->size < EI_NIDENT ||
	    ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
	    ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
	    ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
	    ehdr->e_ident[EI_MAG3] != ELFMAG3) {
		PyErr_SetString(ElfFormatError, "not an ELF file");
		return -1;
	}

	if (ehdr->e_ident[EI_VERSION] != EV_CURRENT) {
		PyErr_Format(ElfFormatError, "ELF version %u is not EV_CURRENT",
			     (unsigned int)ehdr->e_ident[EI_VERSION]);
		return -1;
	}

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB) {
#else
	if (ehdr->e_ident[EI_DATA] != ELFDATA2MSB) {
#endif
		PyErr_SetString(PyExc_NotImplementedError,
				"ELF file endianness does not match machine");
		return -1;
	}

	if (ehdr->e_ident[EI_CLASS] == ELFCLASS32) {
		PyErr_SetString(PyExc_NotImplementedError,
				"32-bit ELF is not implemented");
		return -1;
	} else if (ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
		PyErr_Format(ElfFormatError, "unknown ELF class %u",
			     (unsigned int)ehdr->e_ident[EI_CLASS]);
		return -1;
	}

	if (file->size < sizeof(Elf64_Ehdr)) {
		PyErr_SetString(ElfFormatError, "ELF header is truncated");
		return -1;
	}

	if (ehdr->e_shnum == 0) {
		PyErr_SetString(ElfFormatError, "ELF file has no sections");
		return -1;
	}

	if (ehdr->e_shoff > SIZE_MAX - sizeof(Elf64_Shdr) * (size_t)ehdr->e_shnum ||
	    ehdr->e_shoff + sizeof(Elf64_Shdr) * (size_t)ehdr->e_shnum > file->size) {
		PyErr_SetString(ElfFormatError,
				"ELF section header table is beyond EOF");
		return -1;
	}

	return 0;
}

static int validate_shdr(struct file *file, const Elf64_Shdr *shdr)
{
	if (shdr->sh_offset > SIZE_MAX - shdr->sh_size ||
	    shdr->sh_offset + shdr->sh_size > file->size) {
		PyErr_SetString(ElfFormatError, "ELF section is beyond EOF");
		return -1;
	}
	return 0;
}

static int read_sections(struct file *file)
{
	const Elf64_Ehdr *ehdr;
	const Elf64_Shdr *shdrs;
	uint16_t shstrndx;
	const Elf64_Shdr *shstrtab_shdr;
	const char *shstrtab;
	uint16_t i;

	ehdr = (Elf64_Ehdr *)file->map;
	if (validate_ehdr(file, ehdr) == -1)
		return -1;


	shdrs = (Elf64_Shdr *)(file->map + ehdr->e_shoff);

	shstrndx = ehdr->e_shstrndx;
	if (shstrndx == SHN_XINDEX)
		shstrndx = shdrs[0].sh_link;
	if (shstrndx == SHN_UNDEF || shstrndx >= ehdr->e_shnum) {
		PyErr_SetString(ElfFormatError,
				"invalid ELF section header string table index");
		return -1;
	}
	shstrtab_shdr = &shdrs[shstrndx];
	if (validate_shdr(file, shstrtab_shdr) == -1)
		return -1;

	shstrtab = file->map + shstrtab_shdr->sh_offset;

	for (i = 0; i < ehdr->e_shnum; i++) {
		struct section *section;
		const char *name;
		size_t max_name_len;

		if (shdrs[i].sh_type == SHT_PROGBITS) {
			if (shdrs[i].sh_name == 0 ||
			    shdrs[i].sh_name >= shstrtab_shdr->sh_size)
				continue;
			max_name_len = shstrtab_shdr->sh_size - shdrs[i].sh_name;
			name = &shstrtab[shdrs[i].sh_name];
			/* Must be greater than to account for the NUL byte. */
			if (max_name_len > strlen(".debug_abbrev") &&
			    strcmp(name, ".debug_abbrev") == 0)
				section = &file->debug_sections[DEBUG_ABBREV];
			else if (max_name_len > strlen(".debug_info") &&
				 strcmp(name, ".debug_info") == 0)
				section = &file->debug_sections[DEBUG_INFO];
			else if (max_name_len > strlen(".debug_line") &&
				 strcmp(name, ".debug_line") == 0)
				section = &file->debug_sections[DEBUG_LINE];
			else if (max_name_len > strlen(".debug_str") &&
				 strcmp(name, ".debug_str") == 0)
				section = &file->debug_sections[DEBUG_STR];
			else
				continue;
		} else if (shdrs[i].sh_type == SHT_SYMTAB) {
			section = &file->symtab;
		} else {
			continue;
		}
		if (validate_shdr(file, &shdrs[i]) == -1)
			return -1;
		section->shdr_index = i;
		section->buffer = file->map + shdrs[i].sh_offset;
		section->size = shdrs[i].sh_size;
	}

	if (!file->symtab.buffer)
		return 0;
	for (i = 0; i < NUM_DEBUG_SECTIONS; i++) {
		if (!file->debug_sections[i].buffer)
			return 0;
	}

	for (i = 0; i < ehdr->e_shnum; i++) {
		struct section *section;
		int j;

		if (shdrs[i].sh_type != SHT_RELA)
			continue;
		for (j = 0; j < NUM_DEBUG_SECTIONS; j++) {
			if (shdrs[i].sh_info == file->debug_sections[j].shdr_index) {
				section = &file->rela_sections[j];
				break;
			}
		}
		if (j == NUM_DEBUG_SECTIONS)
			continue;
		if (shdrs[i].sh_link != file->symtab.shdr_index) {
			PyErr_SetString(ElfFormatError,
					"relocation symbol table section is not .symtab");
			return -1;
		}
		if (validate_shdr(file, &shdrs[i]) == -1)
			return -1;
		section->shdr_index = i;
		section->buffer = file->map + shdrs[i].sh_offset;
		section->size = shdrs[i].sh_size;
	}
	return 1;
}

static int apply_relocation(struct section *section,
			    struct section *rela_section,
			    struct section *symtab, size_t i)
{
	const Elf64_Rela *reloc;
	const Elf64_Sym *syms;
	size_t num_syms;
	uint32_t r_sym;
	uint32_t r_type;
	char *p;

	reloc = &((Elf64_Rela *)rela_section->buffer)[i];
	syms = (Elf64_Sym *)symtab->buffer;
	num_syms = symtab->size / sizeof(Elf64_Sym);

	p = section->buffer + reloc->r_offset;
	r_sym = reloc->r_info >> 32;
	r_type = reloc->r_info & UINT32_C(0xffffffff);
	switch (r_type) {
	case R_X86_64_NONE:
		break;
	case R_X86_64_32:
		if (r_sym >= num_syms) {
			PyErr_Format(ElfFormatError,
				     "invalid relocation symbol");
			return -1;
		}
		if (reloc->r_offset > SIZE_MAX - sizeof(uint32_t) ||
		    reloc->r_offset + sizeof(uint32_t) > section->size) {
			PyErr_Format(ElfFormatError,
				     "invalid relocation offset");
			return -1;
		}
		*(uint32_t *)p = syms[r_sym].st_value + reloc->r_addend;
		break;
	case R_X86_64_64:
		if (r_sym >= num_syms) {
			PyErr_Format(ElfFormatError,
				     "invalid relocation symbol");
			return -1;
		}
		if (reloc->r_offset > SIZE_MAX - sizeof(uint64_t) ||
		    reloc->r_offset + sizeof(uint64_t) > section->size) {
			PyErr_Format(ElfFormatError,
				     "invalid relocation offset");
			return -1;
		}
		*(uint64_t *)p = syms[r_sym].st_value + reloc->r_addend;
		break;
	default:
		PyErr_Format(PyExc_NotImplementedError,
			     "unimplemented relocation type %" PRIu32,
			     r_type);
		return -1;
	}
	return 0;
}

static int read_compilation_unit_header(const char *ptr, const char *end,
					struct compilation_unit *cu)
{
	uint32_t tmp;

	if (read_u32(&ptr, end, &tmp) == -1)
		return -1;
	cu->is_64_bit = tmp == UINT32_C(0xffffffff);
	if (cu->is_64_bit) {
		if (read_u64(&ptr, end, &cu->unit_length) == -1)
			return -1;
	} else {
		cu->unit_length = tmp;
	}

	if (read_u16(&ptr, end, &cu->version) == -1)
		return -1;
	if (cu->version != 2 && cu->version != 3 && cu->version != 4) {
		PyErr_Format(DwarfFormatError, "unknown DWARF version %" PRIu16,
			     cu->version);
		return -1;
	}

	if (cu->is_64_bit) {
		if (read_u64(&ptr, end, &cu->debug_abbrev_offset) == -1)
			return -1;
	} else {
		if (read_u32_into_u64(&ptr, end, &cu->debug_abbrev_offset) == -1)
			return -1;
	}

	return read_u8(&ptr, end, &cu->address_size);
}

static int read_cus(DwarfIndex *self, struct file *file)
{
	const struct section *debug_info = &file->debug_sections[DEBUG_INFO];
	const char *ptr = debug_info->buffer;
	const char *debug_info_end = &ptr[debug_info->size];

	while (ptr < debug_info_end) {
		struct compilation_unit *cu;

		if (self->num_cus >= self->cus_capacity) {
			size_t capacity = self->cus_capacity;

			if (capacity == 0)
				capacity = 1;
			else
				capacity *= 2;
			if (resize_array(&self->cus, capacity) == -1)
				return -1;
			self->cus_capacity = capacity;
		}

		cu = &self->cus[self->num_cus++];

		if (read_compilation_unit_header(ptr, debug_info_end, cu) == -1)
			return -1;
		cu->ptr = ptr;
		cu->file = file - self->files;
		self->address_size = cu->address_size;

		ptr += (cu->is_64_bit ? 12 : 4) + cu->unit_length;
	}
	return 0;
}

static int append_cmd(struct abbrev_table *table, uint8_t cmd, size_t *num_cmds,
		      size_t *cmds_capacity)
{
	if (*num_cmds >= *cmds_capacity) {
		if (*cmds_capacity == 0)
			*cmds_capacity = 32;
		else
			*cmds_capacity *= 2;
		if (resize_array(&table->cmds, *cmds_capacity) == -1)
			return -1;
	}
	table->cmds[(*num_cmds)++] = cmd;
	return 0;
}

static int read_abbrev_decl(const char **ptr, const char *end,
			    const struct compilation_unit *cu,
			    struct abbrev_table *table, size_t *decls_capacity,
			    size_t *num_cmds, size_t *cmds_capacity)
{
	uint64_t code;
	uint64_t tag;
	uint8_t children;
	uint8_t flags = 0;
	bool first = true;

	if (read_uleb128(ptr, end, &code) == -1)
		return -1;
	if (code == 0)
		return 0;
	if (code != table->num_decls + 1) {
		PyErr_SetString(PyExc_NotImplementedError,
				"abbreviation table is not sequential");
		return -1;
	}

	if (table->num_decls >= *decls_capacity) {
		if (*decls_capacity == 0)
			*decls_capacity = 1;
		else
			*decls_capacity *= 2;
		if (resize_array(&table->decls, *decls_capacity) == -1)
			return -1;
	}
	table->decls[table->num_decls++] = *num_cmds;

	if (read_uleb128(ptr, end, &tag) == -1)
		return -1;
	if (tag != DW_TAG_base_type &&
	    tag != DW_TAG_class_type &&
	    tag != DW_TAG_compile_unit &&
	    tag != DW_TAG_enumeration_type &&
	    tag != DW_TAG_enumerator &&
	    tag != DW_TAG_structure_type &&
	    tag != DW_TAG_typedef &&
	    tag != DW_TAG_union_type &&
	    tag != DW_TAG_variable)
		tag = 0;
	if (read_u8(ptr, end, &children) == -1)
		return -1;
	if (children)
		flags |= TAG_FLAG_CHILDREN;

	for (;;) {
		uint64_t name, form;
		uint8_t cmd;

		if (read_uleb128(ptr, end, &name) == -1)
			return -1;
		if (read_uleb128(ptr, end, &form) == -1)
			return -1;
		if (name == 0 && form == 0)
			break;

		if (name == DW_AT_sibling && tag != DW_TAG_enumeration_type) {
			/*
			 * Not for DW_TAG_enumeration_type because we need to
			 * descend into any DW_TAG_enumerator children.
			 */
			switch (form) {
			case DW_FORM_ref1:
				cmd = ATTRIB_SIBLING_REF1;
				goto append_cmd;
			case DW_FORM_ref2:
				cmd = ATTRIB_SIBLING_REF2;
				goto append_cmd;
			case DW_FORM_ref4:
				cmd = ATTRIB_SIBLING_REF4;
				goto append_cmd;
			case DW_FORM_ref8:
				cmd = ATTRIB_SIBLING_REF8;
				goto append_cmd;
			case DW_FORM_ref_udata:
				cmd = ATTRIB_SIBLING_REF_UDATA;
				goto append_cmd;
			default:
				break;
			}
		} else if (name == DW_AT_name && tag &&
			   tag != DW_TAG_compile_unit) {
			switch (form) {
			case DW_FORM_strp:
				if (cu->is_64_bit)
					cmd = ATTRIB_NAME_STRP8;
				else
					cmd = ATTRIB_NAME_STRP4;
				goto append_cmd;
			case DW_FORM_string:
				cmd = ATTRIB_NAME_STRING;
				goto append_cmd;
			default:
				break;
			}
		} else if (name == DW_AT_stmt_list &&
			   tag == DW_TAG_compile_unit) {
			switch (form) {
			case DW_FORM_data4:
				cmd = ATTRIB_STMT_LIST_LINEPTR4;
				goto append_cmd;
			case DW_FORM_data8:
				cmd = ATTRIB_STMT_LIST_LINEPTR8;
				goto append_cmd;
			case DW_FORM_sec_offset:
				if (cu->is_64_bit)
					cmd = ATTRIB_STMT_LIST_LINEPTR8;
				else
					cmd = ATTRIB_STMT_LIST_LINEPTR4;
				goto append_cmd;
			default:
				break;
			}
		} else if (name == DW_AT_decl_file && tag &&
			   tag != DW_TAG_compile_unit) {
			switch (form) {
			case DW_FORM_data1:
				cmd = ATTRIB_DECL_FILE_DATA1;
				goto append_cmd;
			case DW_FORM_data2:
				cmd = ATTRIB_DECL_FILE_DATA2;
				goto append_cmd;
			case DW_FORM_data4:
				cmd = ATTRIB_DECL_FILE_DATA4;
				goto append_cmd;
			case DW_FORM_data8:
				cmd = ATTRIB_DECL_FILE_DATA8;
				goto append_cmd;
			/*
			 * decl_file must be positive, so if the compiler uses
			 * DW_FORM_sdata for some reason, just treat it as
			 * udata.
			 */
			case DW_FORM_sdata:
			case DW_FORM_udata:
				cmd = ATTRIB_DECL_FILE_UDATA;
				goto append_cmd;
			default:
				break;
			}
		} else if (name == DW_AT_declaration) {
			/*
			 * In theory, this could be DW_FORM_flag with a value of
			 * zero, but in practice, GCC always uses
			 * DW_FORM_flag_present.
			 */
			flags |= TAG_FLAG_DECLARATION;
		} else if (name == DW_AT_specification && tag &&
			   tag != DW_TAG_compile_unit) {
			switch (form) {
			case DW_FORM_ref1:
				cmd = ATTRIB_SPECIFICATION_REF1;
				goto append_cmd;
			case DW_FORM_ref2:
				cmd = ATTRIB_SPECIFICATION_REF2;
				goto append_cmd;
			case DW_FORM_ref4:
				cmd = ATTRIB_SPECIFICATION_REF4;
				goto append_cmd;
			case DW_FORM_ref8:
				cmd = ATTRIB_SPECIFICATION_REF8;
				goto append_cmd;
			case DW_FORM_ref_udata:
				cmd = ATTRIB_SPECIFICATION_REF_UDATA;
				goto append_cmd;
			default:
				break;
			}
		}

		switch (form) {
		case DW_FORM_addr:
			cmd = cu->address_size;
			break;
		case DW_FORM_data1:
		case DW_FORM_ref1:
		case DW_FORM_flag:
			cmd = 1;
			break;
		case DW_FORM_data2:
		case DW_FORM_ref2:
			cmd = 2;
			break;
		case DW_FORM_data4:
		case DW_FORM_ref4:
			cmd = 4;
			break;
		case DW_FORM_data8:
		case DW_FORM_ref8:
		case DW_FORM_ref_sig8:
			cmd = 8;
			break;
		case DW_FORM_block1:
			cmd = ATTRIB_BLOCK1;
			goto append_cmd;
		case DW_FORM_block2:
			cmd = ATTRIB_BLOCK2;
			goto append_cmd;
		case DW_FORM_block4:
			cmd = ATTRIB_BLOCK4;
			goto append_cmd;
		case DW_FORM_exprloc:
			cmd = ATTRIB_EXPRLOC;
			goto append_cmd;
		case DW_FORM_sdata:
		case DW_FORM_udata:
		case DW_FORM_ref_udata:
			cmd = ATTRIB_LEB128;
			goto append_cmd;
		case DW_FORM_ref_addr:
		case DW_FORM_sec_offset:
		case DW_FORM_strp:
			cmd = cu->is_64_bit ? 8 : 4;
			break;
		case DW_FORM_string:
			cmd = ATTRIB_STRING;
			goto append_cmd;
		case DW_FORM_flag_present:
			continue;
		case DW_FORM_indirect:
			PyErr_SetString(PyExc_NotImplementedError,
					"DW_FORM_indirect is not implemented");
			return -1;
		default:
			PyErr_Format(DwarfFormatError,
				     "unknown attribute form %" PRIu64, form);
			return -1;
		}

		if (!first && table->cmds[*num_cmds - 1] < CMD_MAX_SKIP) {
			if ((uint16_t)table->cmds[*num_cmds - 1] + cmd <= CMD_MAX_SKIP) {
				table->cmds[*num_cmds - 1] += cmd;
				continue;
			} else {
				cmd = (uint16_t)table->cmds[*num_cmds - 1] + cmd - CMD_MAX_SKIP;
				table->cmds[*num_cmds - 1] = CMD_MAX_SKIP;
			}
		}

append_cmd:
		first = false;
		if (append_cmd(table, cmd, num_cmds, cmds_capacity) == -1)
			return -1;
	}
	if (append_cmd(table, 0, num_cmds, cmds_capacity) == -1)
		return -1;
	if (append_cmd(table, tag | flags, num_cmds, cmds_capacity) == -1)
		return -1;

	return 1;
}

static int read_abbrev_table(const char *ptr, const char *end,
			     const struct compilation_unit *cu,
			     struct abbrev_table *table)
{
	size_t decls_capacity = 0;
	size_t num_cmds = 0;
	size_t cmds_capacity = 0;

	for (;;) {
		int ret;

		ret = read_abbrev_decl(&ptr, end, cu, table, &decls_capacity,
				       &num_cmds, &cmds_capacity);
		if (ret != 1)
			return ret;
	}
}

static int skip_lnp_header(const char **ptr, const char *end)
{
	uint32_t tmp;
	bool is_64_bit;
	uint16_t version;
	uint8_t opcode_base;

	if (read_u32(ptr, end, &tmp) == -1)
		return -1;
	is_64_bit = tmp == UINT32_C(0xffffffff);
	if (is_64_bit)
		*ptr += sizeof(uint64_t);

	if (read_u16(ptr, end, &version) == -1)
		return -1;
	if (version != 2 && version != 3 && version != 4) {
		PyErr_Format(DwarfFormatError, "unknown DWARF version %" PRIu16,
			     version);
		return -1;
	}

	/*
	 * header_length
	 * minimum_instruction_length
	 * maximum_operations_per_instruction (DWARF 4 only)
	 * default_is_stmt
	 * line_base
	 * line_range
	 */
	*ptr += (is_64_bit ? 8 : 4) + 4 + (version >= 4);

	if (read_u8(ptr, end, &opcode_base) == -1)
		return -1;
	/* standard_opcode_lengths */
	*ptr += opcode_base - 1;

	return 0;
}

/*
 * Hash the canonical path of a directory. We always include a trailing slash.
 * We also reverse the path components (e.g., "a/b/c" becomes "c/b/a/" and
 * "/a/b" becomes "b/a//"). This makes it possible to handle ".." in one pass.
 */
static void hash_directory(struct siphash *hash, const char *path,
			   size_t path_len)
{
	unsigned int dot_dot = 0;

	if (!path_len)
		return;

	while (path_len) {
		size_t component_len = 0;

		/* Skip slashes. */
		if (path[path_len - 1] == '/') {
			path_len--;
			continue;
		}

		/* Skip "." components. */
		if (path_len == 1 && path[0] == '.')
			break;
		if (path_len >= 2 && path[path_len - 2] == '/' &&
		     path[path_len - 1] == '.') {
			path_len -= 2;
			continue;
		}

		/* Count ".." components. */
		if (path_len == 2 && path[0] == '.' && path[1] == '.') {
			dot_dot++;
			break;
		}
		if (path_len >= 3 && path[path_len - 3] == '/' &&
		    path[path_len - 2] == '.' && path[path_len - 1] == '.') {
			path_len -= 3;
			dot_dot++;
			continue;
		}

		/* Hash or skip other components. */
		while (path[path_len - 1] != '/') {
			path_len--;
			component_len++;
			if (!path_len)
				break;
		}
		if (dot_dot) {
			dot_dot--;
			continue;
		}
		siphash_update(hash, &path[path_len], component_len);
		siphash_update(hash, "/", 1);
	}

	if (path[0] == '/') {
		/* Absolute path. */
		siphash_update(hash, "/", 1);
	} else {
		/*
		 * Leftover ".." components must be above the current directory,
		 * but only if this wasn't an absolute path.
		 */
		while (dot_dot) {
			siphash_update(hash, "../", 3);
			dot_dot--;
		}
	}

}

static int read_file_name_table(DwarfIndex *self, struct compilation_unit *cu,
				size_t stmt_list, struct file_name_table *table)
{
	struct file *file = &self->files[cu->file];
	const struct section *debug_line = &file->debug_sections[DEBUG_LINE];
	const char *end = &debug_line->buffer[debug_line->size];
	const char *ptr = &debug_line->buffer[stmt_list];
	struct siphash *directories = NULL;
	size_t num_directories = 0;
	size_t directories_capacity = 0;
	size_t files_capacity = 0;
	int ret = -1;

	if (skip_lnp_header(&ptr, end) == -1)
		return -1;

	for (;;) {
		struct siphash *hash;
		const char *path;
		size_t path_len;

		if (read_string(&ptr, end, &path, &path_len) == -1)
			goto out;
		if (!path_len)
			break;

		if (num_directories >= directories_capacity) {
			if (directories_capacity == 0)
				directories_capacity = 16;
			else
				directories_capacity *= 2;
			if (resize_array(&directories,
					 directories_capacity) == -1)
				goto out;
		}

		hash = &directories[num_directories++];
		siphash_init(hash);
		hash_directory(hash, path, path_len);
	}

	for (;;) {
		const char *path;
		size_t path_len;
		uint64_t directory_index;
		struct siphash hash;

		if (read_string(&ptr, end, &path, &path_len) == -1)
			goto out;
		if (!path_len)
			break;

		if (read_uleb128(&ptr, end, &directory_index) == -1)
			goto out;
		/* mtime, size */
		if (skip_leb128(&ptr, end) == -1 ||
		    skip_leb128(&ptr, end) == -1)
			goto out;

		if (directory_index > num_directories) {
			PyErr_Format(DwarfFormatError,
				     "directory index %" PRIu64 " is invalid",
				     directory_index);
			goto out;
		}

		if (directory_index)
			hash = directories[directory_index - 1];
		else
			siphash_init(&hash);
		siphash_update(&hash, path, path_len);

		if (table->num_files >= files_capacity) {
			if (files_capacity == 0)
				files_capacity = 16;
			else
				files_capacity *= 2;
			if (resize_array(&table->file_name_hashes,
					 files_capacity) == -1)
				goto out;
		}
		table->file_name_hashes[table->num_files++] = siphash_final(&hash);
	}

	ret = 0;
out:
	free(directories);
	return ret;
}

/* DJBX33A hash function */
static inline uint32_t name_hash(const char *name)
{
	uint32_t hash = 5381;
	const uint8_t *p = (const uint8_t *)name;

	while (*p)
		hash = ((hash << 5) + hash) + *p++;
	return hash;
}

static int add_die_hash_entry(DwarfIndex *self, const char *name, uint64_t tag,
			      uint64_t file_name_hash,
			      struct compilation_unit *cu, const char *ptr)
{
	struct die_hash_entry *entry;
	uint32_t i, orig_i;

	i = orig_i = name_hash(name) & DIE_HASH_MASK;
	for (;;) {
		const char *entry_name;
		uint64_t entry_tag;

		entry = &self->die_hash[i];
		entry_name = __atomic_load_n(&entry->name, __ATOMIC_RELAXED);
		if (!entry_name &&
		    __atomic_compare_exchange_n(&entry->name, &entry_name, name,
						false, __ATOMIC_RELAXED,
						__ATOMIC_RELAXED)) {
			entry->cu = cu - self->cus;
			entry->ptr = ptr;
			entry->file_name_hash = file_name_hash;
			__atomic_store_n(&entry->tag, tag, __ATOMIC_RELEASE);
			return 0;
		}
		do {
			entry_tag = __atomic_load_n(&entry->tag,
						    __ATOMIC_ACQUIRE);
		} while (!entry_tag);
		if (tag == entry_tag &&
		    file_name_hash == entry->file_name_hash &&
		    strcmp(name, entry_name) == 0)
			return 0;
		i = (i + 1) & DIE_HASH_MASK;
		if (i == orig_i) {
			PyErr_NoMemory();
			return -1;
		}
	}
}

struct die {
	const char *sibling;
	const char *name;
	size_t stmt_list;
	size_t decl_file;
	const char *specification;
	uint8_t flags;
};

static int read_die(struct compilation_unit *cu,
		    const struct abbrev_table *abbrev_table,
		    const char **ptr, const char *end,
		    const char *debug_str_buffer, const char *debug_str_end,
		    struct die *die)
{
	uint64_t code;
	uint8_t *cmdp;
	uint8_t cmd;

	if (read_uleb128(ptr, end, &code) == -1)
		return -1;
	if (code == 0)
		return 0;

	if (code < 1 || code > abbrev_table->num_decls) {
		PyErr_Format(DwarfFormatError,
			     "unknown abbreviation code %" PRIu64,
			     code);
		return -1;
	}
	cmdp = &abbrev_table->cmds[abbrev_table->decls[code - 1]];

	while ((cmd = *cmdp++)) {
		uint64_t skip;
		uint64_t tmp;

		switch (cmd) {
		case ATTRIB_BLOCK1:
			if (read_u8_into_size_t(ptr, end, &skip) == -1)
				return -1;
			goto skip;
		case ATTRIB_BLOCK2:
			if (read_u16_into_size_t(ptr, end, &skip) == -1)
				return -1;
			goto skip;
		case ATTRIB_BLOCK4:
			if (read_u32_into_size_t(ptr, end, &skip) == -1)
				return -1;
			goto skip;
		case ATTRIB_EXPRLOC:
			if (read_uleb128_into_size_t(ptr, end, &skip) == -1)
				return -1;
			goto skip;
		case ATTRIB_LEB128:
			if (skip_leb128(ptr, end) == -1)
				return -1;
			break;
		case ATTRIB_NAME_STRING:
			die->name = *ptr;
			/* fallthrough */
		case ATTRIB_STRING:
			if (skip_string(ptr, end) == -1)
				return -1;
			break;
		case ATTRIB_SIBLING_REF1:
			if (read_u8_into_size_t(ptr, end, &tmp) == -1)
				return -1;
			goto sibling;
		case ATTRIB_SIBLING_REF2:
			if (read_u16_into_size_t(ptr, end, &tmp) == -1)
				return -1;
			goto sibling;
		case ATTRIB_SIBLING_REF4:
			if (read_u32_into_size_t(ptr, end, &tmp) == -1)
				return -1;
			goto sibling;
		case ATTRIB_SIBLING_REF8:
			if (read_u64_into_size_t(ptr, end, &tmp) == -1)
				return -1;
			goto sibling;
		case ATTRIB_SIBLING_REF_UDATA:
			if (read_uleb128_into_size_t(ptr, end, &tmp) == -1)
				return -1;
sibling:
			if (!in_bounds(cu->ptr, end, tmp)) {
				PyErr_SetNone(PyExc_EOFError);
				return -1;
			}
			die->sibling = &cu->ptr[tmp];
			__builtin_prefetch(die->sibling);
			break;
		case ATTRIB_NAME_STRP4:
			if (read_u32_into_size_t(ptr, end, &tmp) == -1)
				return -1;
			goto strp;
		case ATTRIB_NAME_STRP8:
			if (read_u64_into_size_t(ptr, end, &tmp) == -1)
				return -1;
strp:
			if (!in_bounds(debug_str_buffer, debug_str_end, tmp)) {
				PyErr_SetNone(PyExc_EOFError);
				return -1;
			}
			die->name = &debug_str_buffer[tmp];
			__builtin_prefetch(die->name);
			break;
		case ATTRIB_STMT_LIST_LINEPTR4:
			if (read_u32_into_size_t(ptr, end, &die->stmt_list) == -1)
				return -1;
			break;
		case ATTRIB_STMT_LIST_LINEPTR8:
			if (read_u64_into_size_t(ptr, end, &die->stmt_list) == -1)
				return -1;
			break;
		case ATTRIB_DECL_FILE_DATA1:
			if (read_u8_into_size_t(ptr, end, &die->decl_file) == -1)
				return -1;
			break;
		case ATTRIB_DECL_FILE_DATA2:
			if (read_u16_into_size_t(ptr, end, &die->decl_file) == -1)
				return -1;
			break;
		case ATTRIB_DECL_FILE_DATA4:
			if (read_u32_into_size_t(ptr, end, &die->decl_file) == -1)
				return -1;
			break;
		case ATTRIB_DECL_FILE_DATA8:
			if (read_u64_into_size_t(ptr, end, &die->decl_file) == -1)
				return -1;
			break;
		case ATTRIB_DECL_FILE_UDATA:
			if (read_uleb128_into_size_t(ptr, end, &die->decl_file) == -1)
				return -1;
			break;
		case ATTRIB_SPECIFICATION_REF1:
			if (read_u8_into_size_t(ptr, end, &tmp) == -1)
				return -1;
			goto specification;
		case ATTRIB_SPECIFICATION_REF2:
			if (read_u16_into_size_t(ptr, end, &tmp) == -1)
				return -1;
			goto specification;
		case ATTRIB_SPECIFICATION_REF4:
			if (read_u32_into_size_t(ptr, end, &tmp) == -1)
				return -1;
			goto specification;
		case ATTRIB_SPECIFICATION_REF8:
			if (read_u64_into_size_t(ptr, end, &tmp) == -1)
				return -1;
			goto specification;
		case ATTRIB_SPECIFICATION_REF_UDATA:
			if (read_uleb128_into_size_t(ptr, end, &tmp) == -1)
				return -1;
specification:
			if (!in_bounds(cu->ptr, end, tmp)) {
				PyErr_SetNone(PyExc_EOFError);
				return -1;
			}
			die->specification = &cu->ptr[tmp];
			__builtin_prefetch(die->specification);
			break;
		default:
			skip = cmd;
skip:
			if (!in_bounds(*ptr, end, skip)) {
				PyErr_SetNone(PyExc_EOFError);
				return -1;
			}
			*ptr += skip;
			break;
		}
	}

	die->flags = *cmdp;

	return 1;
}

static int index_cu(DwarfIndex *self, struct compilation_unit *cu)
{
	struct abbrev_table abbrev_table = {};
	struct file_name_table file_name_table = {};
	struct file *file = &self->files[cu->file];
	const struct section *debug_abbrev = &file->debug_sections[DEBUG_ABBREV];
	const char *debug_abbrev_end = &debug_abbrev->buffer[debug_abbrev->size];
	const char *ptr = &cu->ptr[cu->is_64_bit ? 23 : 11];
	const char *end = &cu->ptr[(cu->is_64_bit ? 12 : 4) + cu->unit_length];
	const struct section *debug_str = &file->debug_sections[DEBUG_STR];
	const char *debug_str_buffer = debug_str->buffer;
	const char *debug_str_end = &debug_str_buffer[debug_str->size];
	unsigned int depth = 0;
	const char *enum_die_ptr = NULL;
	int ret = -1;

	if (read_abbrev_table(&debug_abbrev->buffer[cu->debug_abbrev_offset],
			      debug_abbrev_end, cu, &abbrev_table) == -1)
		goto out;

	for (;;) {
		struct die die = {
			.stmt_list = SIZE_MAX,
		};
		const char *die_ptr = ptr;
		uint64_t tag;
		int ret2;

		ret2 = read_die(cu, &abbrev_table, &ptr, end, debug_str_buffer,
				debug_str_end, &die);
		if (ret2 == -1)
			goto out;
		if (ret2 == 0) {
			depth--;
			if (depth == 1)
				enum_die_ptr = NULL;
			else if (depth == 0)
				break;
			continue;
		}

		tag = die.flags & TAG_MASK;
		if (tag == DW_TAG_compile_unit) {
			if (depth == 0 && die.stmt_list != SIZE_MAX &&
			    read_file_name_table(self, cu, die.stmt_list,
						 &file_name_table) == -1)
				goto out;
		} else if (tag && !(die.flags & TAG_FLAG_DECLARATION)) {
			uint64_t file_name_hash;

			/*
			 * NB: the enumerator name points to the
			 * enumeration_type DIE instead of the enumerator DIE.
			 */
			if (depth == 1 && tag == DW_TAG_enumeration_type)
				enum_die_ptr = die_ptr;
			else if (depth == 2 && tag == DW_TAG_enumerator &&
				 enum_die_ptr)
				die_ptr = enum_die_ptr;
			else if (depth != 1)
				goto next;

			if (die.specification && (!die.name || !die.decl_file)) {
				struct die decl = {};
				const char *decl_ptr = die.specification;

				if (read_die(cu, &abbrev_table, &decl_ptr, end,
					     debug_str_buffer, debug_str_end,
					     &decl) == -1)
					goto out;
				if (!die.name && decl.name)
					die.name = decl.name;
				if (!die.decl_file && decl.decl_file)
					die.decl_file = decl.decl_file;
			}

			if (die.name) {
				if (die.decl_file > file_name_table.num_files) {
					PyErr_Format(DwarfFormatError,
						     "invalid DW_AT_decl_file %" PRIu64,
						     die.decl_file);
					goto out;
				}
				if (die.decl_file)
					file_name_hash = file_name_table.file_name_hashes[die.decl_file - 1];
				else
					file_name_hash = 0;
				if (add_die_hash_entry(self, die.name, tag,
						       file_name_hash, cu,
						       die_ptr) == -1)
					goto out;
			}
		}

next:
		if (die.flags & TAG_FLAG_CHILDREN) {
			if (die.sibling)
				ptr = die.sibling;
			else
				depth++;
		} else if (depth == 0) {
			break;
		}
	}

	ret = 0;
out:
	free(file_name_table.file_name_hashes);
	free(abbrev_table.decls);
	free(abbrev_table.cmds);
	return ret;
}

static int apply_relocations(struct file *files, size_t num_files)
{
	PyObject *type = NULL, *value = NULL, *traceback = NULL;
	size_t total_num_relocs;
	size_t i;

	Py_BEGIN_ALLOW_THREADS

	total_num_relocs = 0;
	for (i = 0; i < num_files; i++) {
		size_t j;

		for (j = 0; j < NUM_DEBUG_SECTIONS; j++) {
			total_num_relocs += (files[i].rela_sections[j].size /
					     sizeof(Elf64_Rela));
		}
	}

#pragma omp parallel
	{
		PyGILState_STATE state = PyGILState_Ensure();
		size_t file_idx, section_idx = 0, reloc_idx = 0;
		size_t k;
		bool first = true;
		size_t num_relocs = 0;

		Py_BEGIN_ALLOW_THREADS

#pragma omp for
		for (k = 0; k < total_num_relocs; k++) {
			if (type)
				continue;
			if (first) {
				size_t cur = 0;

				for (file_idx = 0; file_idx < num_files; file_idx++) {
					for (section_idx = 0; section_idx < NUM_DEBUG_SECTIONS; section_idx++) {
						num_relocs = (files[file_idx].rela_sections[section_idx].size /
							      sizeof(Elf64_Rela));
						if (cur + num_relocs > k) {
							reloc_idx = k - cur;
							goto done;
						} else {
							cur += num_relocs;
						}
					}
				}
done:
				first = false;
			}

			if (apply_relocation(&files[file_idx].debug_sections[section_idx],
					     &files[file_idx].rela_sections[section_idx],
					     &files[file_idx].symtab, reloc_idx) == -1) {
				Py_BLOCK_THREADS
				if (type)
					PyErr_Clear();
				else
					PyErr_Fetch(&type, &value, &traceback);
				Py_UNBLOCK_THREADS
				continue;
			}

			if (file_idx < num_files) {
				reloc_idx++;
				while (reloc_idx >= num_relocs) {
					reloc_idx = 0;
					if (++section_idx >= NUM_DEBUG_SECTIONS) {
						section_idx = 0;
						if (++file_idx >= num_files)
							break;
					}
					num_relocs = (files[file_idx].rela_sections[section_idx].size /
						      sizeof(Elf64_Rela));
				}
			}
		}

		Py_END_ALLOW_THREADS

		PyGILState_Release(state);
	}

	Py_END_ALLOW_THREADS

	if (type) {
		PyErr_Restore(type, value, traceback);
		return -1;
	}
	return 0;
}

static int index_cus(DwarfIndex *self, struct compilation_unit *cus,
		     size_t num_cus)
{
	PyObject *type = NULL, *value = NULL, *traceback = NULL;

	Py_BEGIN_ALLOW_THREADS

#pragma omp parallel
	{
		PyGILState_STATE state = PyGILState_Ensure();
		size_t i;

		Py_BEGIN_ALLOW_THREADS

#pragma omp for schedule(dynamic)
		for (i = 0; i < num_cus; i++) {
			if (type)
				continue;
			if (index_cu(self, &cus[i]) == -1) {
				Py_BLOCK_THREADS
				if (type)
					PyErr_Clear();
				else
					PyErr_Fetch(&type, &value, &traceback);
				Py_UNBLOCK_THREADS
			}
		}

		Py_END_ALLOW_THREADS

		PyGILState_Release(state);
	}

	Py_END_ALLOW_THREADS

	if (type) {
		PyErr_Restore(type, value, traceback);
		return -1;
	}
	return 0;
}

static PyObject *DwarfIndex_add(DwarfIndex *self, PyObject *args)
{
	size_t num_args = PyTuple_GET_SIZE(args);
	size_t old_num_files = self->num_files;
	size_t old_num_cus = self->num_cus;
	size_t i;

	if (resize_array(&self->files, self->num_files + num_args) == -1)
		return NULL;

	memset(&self->files[old_num_files], 0,
	       num_args * sizeof(self->files[0]));

	for (i = 0; i < num_args; i++) {
		PyObject *arg = PyTuple_GET_ITEM(args, i);
		struct file *file;
		PyObject *path;
		int ret;

		file = &self->files[self->num_files++];
		file->cu_objs = PyDict_New();
		if (!file->cu_objs)
			goto err;

		path = PyUnicode_EncodeFSDefault(arg);
		if (!path)
			goto err;
		if (open_file(file, PyBytes_AS_STRING(path))) {
			PyErr_SetFromErrnoWithFilenameObject(PyExc_OSError,
							     arg);
			Py_DECREF(path);
			goto err;
		}
		Py_DECREF(path);

		Py_INCREF(arg);
		file->path = arg;

		ret = read_sections(file);
		if (ret == -1) {
			goto err;
		} else if (!ret) {
			Py_DECREF(file->path);
			Py_DECREF(file->cu_objs);
			memset(file, 0, sizeof(*file));
			self->num_files--;
		}
	}

	if (self->num_files != old_num_files + num_args &&
	    resize_array(&self->files, self->num_files) == -1)
		goto err;
	if (self->num_files == old_num_files)
		Py_RETURN_NONE;

	if (apply_relocations(&self->files[old_num_files],
			      self->num_files - old_num_files) == -1)
		goto err;

	for (i = old_num_files; i < self->num_files; i++) {
		const struct section *debug_str;

		debug_str = &self->files[i].debug_sections[DEBUG_STR];
		if (debug_str->size == 0 ||
		    debug_str->buffer[debug_str->size - 1] != '\0') {
			PyErr_SetString(DwarfFormatError,
					".debug_str is not null terminated");
			goto err;
		}

		if (read_cus(self, &self->files[i]) == -1)
			goto err;
	}

	/*
	 * Once we start indexing, the DIE hash table will reference the new
	 * CUs, so we can't free the new files or CUs if there's an error.
	 */
	if (index_cus(self, &self->cus[old_num_cus],
		      self->num_cus - old_num_cus) == -1)
		return NULL;

	Py_RETURN_NONE;

err:
	self->num_cus = old_num_cus;
	for (i = old_num_files; i < self->num_files; i++) {
		Py_XDECREF(self->files[i].path);
		Py_XDECREF(self->files[i].cu_objs);
	}
	resize_array(&self->files, old_num_files);
	self->num_files = old_num_files;
	return NULL;
}

static int DwarfIndex_init(DwarfIndex *self, PyObject *args, PyObject *kwds)
{
	PyObject *ret;

	if (kwds && PyDict_Size(kwds)) {
		PyErr_SetString(PyExc_TypeError,
				"DwarfIndex() takes no keyword arguments");
		return -1;
	}

	ret = DwarfIndex_add(self, args);
	Py_XDECREF(ret);
	return ret ? 0 : -1;
}

static PyObject *create_file_object(DwarfIndex *self, struct file *file)
{
	PyObject *mview, *io, *elf_file, *dwarf_file;

	/* XXX: need to reference count self */
	mview = PyMemoryView_FromMemory(file->map, file->size, PyBUF_READ);
	if (!mview)
		return NULL;

	io = PyObject_CallFunctionObjArgs(MemoryViewIO, mview, NULL);
	Py_DECREF(mview);
	if (!io)
		return NULL;

	elf_file = PyObject_CallFunctionObjArgs(ElfFile, io, NULL);
	Py_DECREF(io);
	if (!elf_file)
		return NULL;

	dwarf_file = PyObject_CallFunctionObjArgs(DwarfFile, file->path,
						  elf_file, NULL);
	Py_DECREF(elf_file);
	return dwarf_file;
}

static PyObject *die_object_from_entry(DwarfIndex *self, struct die_hash_entry *entry)
{
	struct compilation_unit *cu = &self->cus[entry->cu];
	struct file *file = &self->files[cu->file];
	PyObject *method_name;
	PyObject *cu_offset;
	PyObject *cu_obj;
	PyObject *die_offset;
	PyObject *die_obj;

	cu_offset = PyLong_FromUnsignedLongLong(cu->ptr -
						file->debug_sections[DEBUG_INFO].buffer);
	if (!cu_offset)
		return NULL;
	cu_obj = PyDict_GetItem(file->cu_objs, cu_offset);
	if (!cu_obj) {
		if (!file->obj) {
			file->obj = create_file_object(self, file);
			if (!file->obj)
				return NULL;
		}

		method_name = PyUnicode_FromString("compilation_unit");
		cu_obj = PyObject_CallMethodObjArgs(file->obj, method_name,
						    cu_offset, NULL);
		Py_DECREF(method_name);
		if (!cu_obj) {
			Py_DECREF(cu_offset);
			return NULL;
		}

		if (PyDict_SetItem(file->cu_objs, cu_offset, cu_obj) == -1) {
			Py_DECREF(cu_offset);
			Py_DECREF(cu_obj);
			return NULL;
		}
		Py_DECREF(cu_obj);
	}
	Py_DECREF(cu_offset);

	die_offset = PyLong_FromUnsignedLongLong(entry->ptr - cu->ptr);
	if (!die_offset)
		return NULL;

	method_name = PyUnicode_FromString("die");
	if (!method_name) {
		Py_DECREF(die_offset);
		return NULL;
	}
	die_obj = PyObject_CallMethodObjArgs(cu_obj, method_name, die_offset,
					     NULL);
	Py_DECREF(method_name);
	Py_DECREF(die_offset);
	return die_obj;
}

static PyObject *DwarfIndex_find(DwarfIndex *self, PyObject *args, PyObject
				 *kwds)
{
	static char *keywords[] = {"name", "tag", NULL};
	struct die_hash_entry *entry;
	const char *name;
	unsigned long long tag = 0;
	uint32_t i, orig_i;
	PyObject *dies = NULL;

	if (!PyArg_ParseTupleAndKeywords(args, kwds, "s|K:find", keywords,
					 &name, &tag))
		return NULL;

	i = orig_i = name_hash(name) & DIE_HASH_MASK;
	for (;;) {
		entry = &self->die_hash[i];
		if (!entry->name)
			break;

		if ((!tag || entry->tag == tag) &&
		    strcmp(entry->name, name) == 0) {
			PyObject *die;

			if (!dies) {
				dies = PyList_New(0);
				if (!dies)
					goto err;
			}
			die = die_object_from_entry(self, entry);
			if (!die)
				goto err;
			if (PyList_Append(dies, die) == -1) {
				Py_DECREF(die);
				goto err;
			}
			Py_DECREF(die);
		}

		i = (i + 1) & DIE_HASH_MASK;
		if (i == orig_i)
			break;
	}
	if (!dies)
		PyErr_SetString(PyExc_ValueError, "DIE not found");
	return dies;

err:
	Py_XDECREF(dies);
	return NULL;
}

static PyObject *DwarfIndex_files(DwarfIndex *self, void *arg)
{
	PyObject *list;
	size_t i;

	list = PyList_New(self->num_files);
	if (!list)
		return NULL;

	for (i = 0; i < self->num_files; i++) {
		Py_INCREF(self->files[i].path);
		PyList_SET_ITEM(list, i, self->files[i].path);
	}

	return list;
}

#define DwarfIndex_DOC	\
	"DwarfIndex(*paths) -> new DWARF debugging information index"

static PyMethodDef DwarfIndex_methods[] = {
	{"add", (PyCFunction)DwarfIndex_add,
	 METH_VARARGS,
	 "add(*paths)\n\n"
	 "Index the debugging information of the files with the given paths.\n\n"
	 "Arguments:\n"
	 "paths -- paths to index"},
	{"find", (PyCFunction)DwarfIndex_find,
	 METH_VARARGS | METH_KEYWORDS,
	 "find(name, tag=0)\n\n"
	 "Find DWARF DIEs with the given name and tag.\n\n"
	 "Arguments:\n"
	 "name -- string name of the DIE\n"
	 "tag -- int tag of the DIE, or zero for any tag"},
	{},
};

static PyMemberDef DwarfIndex_members[] = {
	{"address_size", T_INT, offsetof(DwarfIndex, address_size),
	 READONLY, "size in bytes of a pointer"},
	{},
};

static PyGetSetDef DwarfIndex_getset[] = {
	{"files", (getter)DwarfIndex_files, NULL,
	 "list of file paths which were indexed, excluding those without debugging symbols"},
	{},
};

static PyTypeObject DwarfIndex_type = {
	PyVarObject_HEAD_INIT(NULL, 0)
	"drgn.internal.dwarfindex.DwarfIndex",	/* tp_name */
	sizeof(DwarfIndex),			/* tp_basicsize */
	0,					/* tp_itemsize */
	(destructor)DwarfIndex_dealloc,		/* tp_dealloc */
	NULL,					/* tp_print */
	NULL,					/* tp_getattr */
	NULL,					/* tp_setattr */
	NULL,					/* tp_as_async */
	NULL,					/* tp_repr */
	NULL,					/* tp_as_number */
	NULL,					/* tp_as_sequence */
	NULL,					/* tp_as_mapping */
	NULL,					/* tp_hash  */
	NULL,					/* tp_call */
	NULL,					/* tp_str */
	NULL,					/* tp_getattro */
	NULL,					/* tp_setattro */
	NULL,					/* tp_as_buffer */
	Py_TPFLAGS_DEFAULT,			/* tp_flags */
	DwarfIndex_DOC,				/* tp_doc */
	(traverseproc)DwarfIndex_traverse,	/* tp_traverse */
	(inquiry)DwarfIndex_clear,		/* tp_clear */
	NULL,					/* tp_richcompare */
	0,					/* tp_weaklistoffset */
	NULL,					/* tp_iter */
	NULL,					/* tp_iternext */
	DwarfIndex_methods,			/* tp_methods */
	DwarfIndex_members,			/* tp_members */
	DwarfIndex_getset,			/* tp_getset */
	NULL,					/* tp_base */
	NULL,					/* tp_dict */
	NULL,					/* tp_descr_get */
	NULL,					/* tp_descr_set */
	0,					/* tp_dictoffset */
	(initproc)DwarfIndex_init,		/* tp_init */
};

static struct PyModuleDef dwarfindexmodule = {
	PyModuleDef_HEAD_INIT,
	"dwarfindex",
	"Fast DWARF debugging information index",
	-1,
};

static int import_dwarf(void)
{
	PyObject *name;
	PyObject *m;

	name = PyUnicode_FromString("drgn.internal.dwarf");
	if (!name)
		return -1;

	m = PyImport_Import(name);
	Py_DECREF(name);
	if (!m)
		return -1;

	DwarfFile = PyObject_GetAttrString(m, "DwarfFile");
	if (!DwarfFile) {
		Py_DECREF(m);
		return -1;
	}
	DwarfFormatError = PyObject_GetAttrString(m, "DwarfFormatError");
	if (!DwarfFormatError) {
		Py_DECREF(m);
		return -1;
	}

	Py_DECREF(m);
	return 0;
}

static int import_elf(void)
{
	PyObject *name;
	PyObject *m;

	name = PyUnicode_FromString("drgn.internal.elf");
	if (!name)
		return -1;

	m = PyImport_Import(name);
	Py_DECREF(name);
	if (!m)
		return -1;

	ElfFile = PyObject_GetAttrString(m, "ElfFile");
	if (!ElfFile) {
		Py_DECREF(m);
		return -1;
	}
	ElfFormatError = PyObject_GetAttrString(m, "ElfFormatError");
	if (!ElfFormatError) {
		Py_DECREF(m);
		return -1;
	}

	Py_DECREF(m);
	return 0;
}

static int import_memoryviewio(void)
{
	PyObject *name;
	PyObject *m;

	name = PyUnicode_FromString("drgn.internal.memoryviewio");
	if (!name)
		return -1;

	m = PyImport_Import(name);
	Py_DECREF(name);
	if (!m)
		return -1;

	MemoryViewIO = PyObject_GetAttrString(m, "MemoryViewIO");
	if (!MemoryViewIO) {
		Py_DECREF(m);
		return -1;
	}

	Py_DECREF(m);
	return 0;
}

PyMODINIT_FUNC
PyInit_dwarfindex(void)
{
	PyObject *m;

	static_assert(ATTRIB_MAX_CMD == UINT8_MAX,
		      "maximum DWARF attribute command is invalid");

	PyEval_InitThreads();

	if (import_dwarf() == -1 || import_elf() == -1 ||
	    import_memoryviewio() == -1)
		return NULL;

	m = PyModule_Create(&dwarfindexmodule);
	if (!m)
		return NULL;

	DwarfIndex_type.tp_new = PyType_GenericNew;
	if (PyType_Ready(&DwarfIndex_type) < 0)
		return NULL;
	Py_INCREF(&DwarfIndex_type);
	PyModule_AddObject(m, "DwarfIndex", (PyObject *)&DwarfIndex_type);

	return m;
}