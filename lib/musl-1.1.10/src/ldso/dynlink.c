#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <elf.h>
#include <sys/mman.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <link.h>
#include <setjmp.h>
#include <pthread.h>
#include <ctype.h>
#include <dlfcn.h>
#include "pthread_impl.h"
#include "libc.h"
#include "dynlink.h"

static void error(const char *, ...);

#ifdef SHARED

#define MAXP2(a,b) (-(-(a)&-(b)))
#define ALIGN(x,y) ((x)+(y)-1 & -(y))

struct debug {
	int ver;
	void *head;
	void (*bp)(void);
	int state;
	void *base;
};

struct td_index {
	size_t args[2];
	struct td_index *next;
};

struct dso {
	unsigned char *base;
	char *name;
	size_t *dynv;
	struct dso *next, *prev;

	Phdr *phdr;
	int phnum;
	size_t phentsize;
	int refcnt;
	Sym *syms;
	uint32_t *hashtab;
	uint32_t *ghashtab;
	int16_t *versym;
	char *strings;
	unsigned char *map;
	size_t map_len;
	dev_t dev;
	ino_t ino;
	signed char global;
	char relocated;
	char constructed;
	char kernel_mapped;
	struct dso **deps, *needed_by;
	char *rpath_orig, *rpath;
	void *tls_image;
	size_t tls_len, tls_size, tls_align, tls_id, tls_offset;
	size_t relro_start, relro_end;
	void **new_dtv;
	unsigned char *new_tls;
	volatile int new_dtv_idx, new_tls_idx;
	struct td_index *td_index;
	struct dso *fini_next;
	char *shortname;
	char buf[];
};

struct symdef {
	Sym *sym;
	struct dso *dso;
};

int __init_tp(void *);
void __init_libc(char **, char *);

const char *__libc_get_version(void);

static struct builtin_tls {
	char c;
	struct pthread pt;
	void *space[16];
} builtin_tls[1];
#define MIN_TLS_ALIGN offsetof(struct builtin_tls, pt)

#define ADDEND_LIMIT 4096
static size_t *saved_addends, *apply_addends_to;

static struct dso ldso;
static struct dso *head, *tail, *fini_head;
static char *env_path, *sys_path;
static unsigned long long gencnt;
static int runtime;
static int ldd_mode;
static int ldso_fail;
static int noload;
static jmp_buf *rtld_fail;
static pthread_rwlock_t lock;
static struct debug debug;
static size_t tls_cnt, tls_offset, tls_align = MIN_TLS_ALIGN;
static size_t static_tls_cnt;
static pthread_mutex_t init_fini_lock = { ._m_type = PTHREAD_MUTEX_RECURSIVE };

struct debug *_dl_debug_addr = &debug;

static int dl_strcmp(const char *l, const char *r)
{
	for (; *l==*r && *l; l++, r++);
	return *(unsigned char *)l - *(unsigned char *)r;
}
#define strcmp(l,r) dl_strcmp(l,r)

static void decode_vec(size_t *v, size_t *a, size_t cnt)
{
	size_t i;
	for (i=0; i<cnt; i++) a[i] = 0;
	for (; v[0]; v+=2) if (v[0]-1<cnt-1) {
		a[0] |= 1UL<<v[0];
		a[v[0]] = v[1];
	}
}

static int search_vec(size_t *v, size_t *r, size_t key)
{
	for (; v[0]!=key; v+=2)
		if (!v[0]) return 0;
	*r = v[1];
	return 1;
}

static uint32_t sysv_hash(const char *s0)
{
	const unsigned char *s = (void *)s0;
	uint_fast32_t h = 0;
	while (*s) {
		h = 16*h + *s++;
		h ^= h>>24 & 0xf0;
	}
	return h & 0xfffffff;
}

static uint32_t gnu_hash(const char *s0)
{
	const unsigned char *s = (void *)s0;
	uint_fast32_t h = 5381;
	for (; *s; s++)
		h = h*33 + *s;
	return h;
}

static Sym *sysv_lookup(const char *s, uint32_t h, struct dso *dso)
{
	size_t i;
	Sym *syms = dso->syms;
	uint32_t *hashtab = dso->hashtab;
	char *strings = dso->strings;
	for (i=hashtab[2+h%hashtab[0]]; i; i=hashtab[2+hashtab[0]+i]) {
		if ((!dso->versym || dso->versym[i] >= 0)
		    && (!strcmp(s, strings+syms[i].st_name)))
			return syms+i;
	}
	return 0;
}

static Sym *gnu_lookup(const char *s, uint32_t h1, struct dso *dso)
{
	Sym *syms = dso->syms;
	char *strings = dso->strings;
	uint32_t *hashtab = dso->ghashtab;
	uint32_t nbuckets = hashtab[0];
	uint32_t *buckets = hashtab + 4 + hashtab[2]*(sizeof(size_t)/4);
	uint32_t h2;
	uint32_t *hashval;
	uint32_t i = buckets[h1 % nbuckets];

	if (!i) return 0;

	hashval = buckets + nbuckets + (i - hashtab[1]);

	for (h1 |= 1; ; i++) {
		h2 = *hashval++;
		if ((!dso->versym || dso->versym[i] >= 0)
		    && (h1 == (h2|1)) && !strcmp(s, strings + syms[i].st_name))
			return syms+i;
		if (h2 & 1) break;
	}

	return 0;
}

#define OK_TYPES (1<<STT_NOTYPE | 1<<STT_OBJECT | 1<<STT_FUNC | 1<<STT_COMMON | 1<<STT_TLS)
#define OK_BINDS (1<<STB_GLOBAL | 1<<STB_WEAK | 1<<STB_GNU_UNIQUE)

#ifndef ARCH_SYM_REJECT_UND
#define ARCH_SYM_REJECT_UND(s) 0
#endif

static struct symdef find_sym(struct dso *dso, const char *s, int need_def)
{
	uint32_t h = 0, gh = 0;
	struct symdef def = {0};
	for (; dso; dso=dso->next) {
		Sym *sym;
		if (!dso->global) continue;
		if (dso->ghashtab) {
			if (!gh) gh = gnu_hash(s);
			sym = gnu_lookup(s, gh, dso);
		} else {
			if (!h) h = sysv_hash(s);
			sym = sysv_lookup(s, h, dso);
		}
		if (!sym) continue;
		if (!sym->st_shndx)
			if (need_def || (sym->st_info&0xf) == STT_TLS
			    || ARCH_SYM_REJECT_UND(sym))
				continue;
		if (!sym->st_value)
			if ((sym->st_info&0xf) != STT_TLS)
				continue;
		if (!(1<<(sym->st_info&0xf) & OK_TYPES)) continue;
		if (!(1<<(sym->st_info>>4) & OK_BINDS)) continue;

		if (def.sym && sym->st_info>>4 == STB_WEAK) continue;
		def.sym = sym;
		def.dso = dso;
		if (sym->st_info>>4 == STB_GLOBAL) break;
	}
	return def;
}

__attribute__((__visibility__("hidden")))
ptrdiff_t __tlsdesc_static(), __tlsdesc_dynamic();

static void do_relocs(struct dso *dso, size_t *rel, size_t rel_size, size_t stride)
{
	unsigned char *base = dso->base;
	Sym *syms = dso->syms;
	char *strings = dso->strings;
	Sym *sym;
	const char *name;
	void *ctx;
	int type;
	int sym_index;
	struct symdef def;
	size_t *reloc_addr;
	size_t sym_val;
	size_t tls_val;
	size_t addend;
	int skip_relative = 0, reuse_addends = 0, save_slot = 0;

	if (dso == &ldso) {
		/* Only ldso's REL table needs addend saving/reuse. */
		if (rel == apply_addends_to)
			reuse_addends = 1;
		skip_relative = 1;
	}

	for (; rel_size; rel+=stride, rel_size-=stride*sizeof(size_t)) {
		if (skip_relative && IS_RELATIVE(rel[1])) continue;
		type = R_TYPE(rel[1]);
		if (type == REL_NONE) continue;
		sym_index = R_SYM(rel[1]);
		reloc_addr = (void *)(base + rel[0]);
		if (sym_index) {
			sym = syms + sym_index;
			name = strings + sym->st_name;
			ctx = type==REL_COPY ? head->next : head;
			def = find_sym(ctx, name, type==REL_PLT);
			if (!def.sym && (sym->st_shndx != SHN_UNDEF
			    || sym->st_info>>4 != STB_WEAK)) {
				error("Error relocating %s: %s: symbol not found",
					dso->name, name);
				if (runtime) longjmp(*rtld_fail, 1);
				continue;
			}
		} else {
			sym = 0;
			def.sym = 0;
			def.dso = dso;
		}

		if (stride > 2) {
			addend = rel[2];
		} else if (type==REL_GOT || type==REL_PLT|| type==REL_COPY) {
			addend = 0;
		} else if (reuse_addends) {
			/* Save original addend in stage 2 where the dso
			 * chain consists of just ldso; otherwise read back
			 * saved addend since the inline one was clobbered. */
			if (head==&ldso)
				saved_addends[save_slot] = *reloc_addr;
			addend = saved_addends[save_slot++];
		} else {
			addend = *reloc_addr;
		}

		sym_val = def.sym ? (size_t)def.dso->base+def.sym->st_value : 0;
		tls_val = def.sym ? def.sym->st_value : 0;

		switch(type) {
		case REL_NONE:
			break;
		case REL_OFFSET:
			addend -= (size_t)reloc_addr;
		case REL_SYMBOLIC:
		case REL_GOT:
		case REL_PLT:
			*reloc_addr = sym_val + addend;
			break;
		case REL_RELATIVE:
			*reloc_addr = (size_t)base + addend;
			break;
		case REL_SYM_OR_REL:
			if (sym) *reloc_addr = sym_val + addend;
			else *reloc_addr = (size_t)base + addend;
			break;
		case REL_COPY:
			memcpy(reloc_addr, (void *)sym_val, sym->st_size);
			break;
		case REL_OFFSET32:
			*(uint32_t *)reloc_addr = sym_val + addend
				- (size_t)reloc_addr;
			break;
		case REL_DTPMOD:
			*reloc_addr = def.dso->tls_id;
			break;
		case REL_DTPOFF:
			*reloc_addr = tls_val + addend;
			break;
#ifdef TLS_ABOVE_TP
		case REL_TPOFF:
			*reloc_addr = tls_val + def.dso->tls_offset + TPOFF_K + addend;
			break;
#else
		case REL_TPOFF:
			*reloc_addr = tls_val - def.dso->tls_offset + addend;
			break;
		case REL_TPOFF_NEG:
			*reloc_addr = def.dso->tls_offset - tls_val + addend;
			break;
#endif
		case REL_TLSDESC:
			if (stride<3) addend = reloc_addr[1];
			if (runtime && def.dso->tls_id >= static_tls_cnt) {
				struct td_index *new = malloc(sizeof *new);
				if (!new) {
					error(
					"Error relocating %s: cannot allocate TLSDESC for %s",
					dso->name, sym ? name : "(local)" );
					longjmp(*rtld_fail, 1);
				}
				new->next = dso->td_index;
				dso->td_index = new;
				new->args[0] = def.dso->tls_id;
				new->args[1] = tls_val + addend;
				reloc_addr[0] = (size_t)__tlsdesc_dynamic;
				reloc_addr[1] = (size_t)new;
			} else {
				reloc_addr[0] = (size_t)__tlsdesc_static;
#ifdef TLS_ABOVE_TP
				reloc_addr[1] = tls_val + def.dso->tls_offset
					+ TPOFF_K + addend;
#else
				reloc_addr[1] = tls_val - def.dso->tls_offset
					+ addend;
#endif
			}
			break;
		default:
			error("Error relocating %s: unsupported relocation type %d",
				dso->name, type);
			if (runtime) longjmp(*rtld_fail, 1);
			continue;
		}
	}
}

/* A huge hack: to make up for the wastefulness of shared libraries
 * needing at least a page of dirty memory even if they have no global
 * data, we reclaim the gaps at the beginning and end of writable maps
 * and "donate" them to the heap by setting up minimal malloc
 * structures and then freeing them. */

static void reclaim(struct dso *dso, size_t start, size_t end)
{
	size_t *a, *z;
	if (start >= dso->relro_start && start < dso->relro_end) start = dso->relro_end;
	if (end   >= dso->relro_start && end   < dso->relro_end) end = dso->relro_start;
	start = start + 6*sizeof(size_t)-1 & -4*sizeof(size_t);
	end = (end & -4*sizeof(size_t)) - 2*sizeof(size_t);
	if (start>end || end-start < 4*sizeof(size_t)) return;
	a = (size_t *)(dso->base + start);
	z = (size_t *)(dso->base + end);
	a[-2] = 1;
	a[-1] = z[0] = end-start + 2*sizeof(size_t) | 1;
	z[1] = 1;
	free(a);
}

static void reclaim_gaps(struct dso *dso)
{
	Phdr *ph = dso->phdr;
	size_t phcnt = dso->phnum;

	for (; phcnt--; ph=(void *)((char *)ph+dso->phentsize)) {
		if (ph->p_type!=PT_LOAD) continue;
		if ((ph->p_flags&(PF_R|PF_W))!=(PF_R|PF_W)) continue;
		reclaim(dso, ph->p_vaddr & -PAGE_SIZE, ph->p_vaddr);
		reclaim(dso, ph->p_vaddr+ph->p_memsz,
			ph->p_vaddr+ph->p_memsz+PAGE_SIZE-1 & -PAGE_SIZE);
	}
}

static void *map_library(int fd, struct dso *dso)
{
	Ehdr buf[(896+sizeof(Ehdr))/sizeof(Ehdr)];
	void *allocated_buf=0;
	size_t phsize;
	size_t addr_min=SIZE_MAX, addr_max=0, map_len;
	size_t this_min, this_max;
	off_t off_start;
	Ehdr *eh;
	Phdr *ph, *ph0;
	unsigned prot;
	unsigned char *map=MAP_FAILED, *base;
	size_t dyn=0;
	size_t tls_image=0;
	size_t i;

	ssize_t l = read(fd, buf, sizeof buf);
	eh = buf;
	if (l<0) return 0;
	if (l<sizeof *eh || (eh->e_type != ET_DYN && eh->e_type != ET_EXEC))
		goto noexec;
	phsize = eh->e_phentsize * eh->e_phnum;
	if (phsize > sizeof buf - sizeof *eh) {
		allocated_buf = malloc(phsize);
		if (!allocated_buf) return 0;
		l = pread(fd, allocated_buf, phsize, eh->e_phoff);
		if (l < 0) goto error;
		if (l != phsize) goto noexec;
		ph = ph0 = allocated_buf;
	} else if (eh->e_phoff + phsize > l) {
		l = pread(fd, buf+1, phsize, eh->e_phoff);
		if (l < 0) goto error;
		if (l != phsize) goto noexec;
		ph = ph0 = (void *)(buf + 1);
	} else {
		ph = ph0 = (void *)((char *)buf + eh->e_phoff);
	}
	for (i=eh->e_phnum; i; i--, ph=(void *)((char *)ph+eh->e_phentsize)) {
		if (ph->p_type == PT_DYNAMIC) {
			dyn = ph->p_vaddr;
		} else if (ph->p_type == PT_TLS) {
			tls_image = ph->p_vaddr;
			dso->tls_align = ph->p_align;
			dso->tls_len = ph->p_filesz;
			dso->tls_size = ph->p_memsz;
		} else if (ph->p_type == PT_GNU_RELRO) {
			dso->relro_start = ph->p_vaddr & -PAGE_SIZE;
			dso->relro_end = (ph->p_vaddr + ph->p_memsz) & -PAGE_SIZE;
		}
		if (ph->p_type != PT_LOAD) continue;
		if (ph->p_vaddr < addr_min) {
			addr_min = ph->p_vaddr;
			off_start = ph->p_offset;
			prot = (((ph->p_flags&PF_R) ? PROT_READ : 0) |
				((ph->p_flags&PF_W) ? PROT_WRITE: 0) |
				((ph->p_flags&PF_X) ? PROT_EXEC : 0));
		}
		if (ph->p_vaddr+ph->p_memsz > addr_max) {
			addr_max = ph->p_vaddr+ph->p_memsz;
		}
	}
	if (!dyn) goto noexec;
	addr_max += PAGE_SIZE-1;
	addr_max &= -PAGE_SIZE;
	addr_min &= -PAGE_SIZE;
	off_start &= -PAGE_SIZE;
	map_len = addr_max - addr_min + off_start;
	/* The first time, we map too much, possibly even more than
	 * the length of the file. This is okay because we will not
	 * use the invalid part; we just need to reserve the right
	 * amount of virtual address space to map over later. */
	map = mmap((void *)addr_min, map_len, prot, MAP_PRIVATE, fd, off_start);
	if (map==MAP_FAILED) goto error;
	/* If the loaded file is not relocatable and the requested address is
	 * not available, then the load operation must fail. */
	if (eh->e_type != ET_DYN && addr_min && map!=(void *)addr_min) {
		errno = EBUSY;
		goto error;
	}
	base = map - addr_min;
	dso->phdr = 0;
	dso->phnum = 0;
	for (ph=ph0, i=eh->e_phnum; i; i--, ph=(void *)((char *)ph+eh->e_phentsize)) {
		if (ph->p_type != PT_LOAD) continue;
		/* Check if the programs headers are in this load segment, and
		 * if so, record the address for use by dl_iterate_phdr. */
		if (!dso->phdr && eh->e_phoff >= ph->p_offset
		    && eh->e_phoff+phsize <= ph->p_offset+ph->p_filesz) {
			dso->phdr = (void *)(base + ph->p_vaddr
				+ (eh->e_phoff-ph->p_offset));
			dso->phnum = eh->e_phnum;
			dso->phentsize = eh->e_phentsize;
		}
		/* Reuse the existing mapping for the lowest-address LOAD */
		if ((ph->p_vaddr & -PAGE_SIZE) == addr_min) continue;
		this_min = ph->p_vaddr & -PAGE_SIZE;
		this_max = ph->p_vaddr+ph->p_memsz+PAGE_SIZE-1 & -PAGE_SIZE;
		off_start = ph->p_offset & -PAGE_SIZE;
		prot = (((ph->p_flags&PF_R) ? PROT_READ : 0) |
			((ph->p_flags&PF_W) ? PROT_WRITE: 0) |
			((ph->p_flags&PF_X) ? PROT_EXEC : 0));
		if (mmap(base+this_min, this_max-this_min, prot, MAP_PRIVATE|MAP_FIXED, fd, off_start) == MAP_FAILED)
			goto error;
		if (ph->p_memsz > ph->p_filesz) {
			size_t brk = (size_t)base+ph->p_vaddr+ph->p_filesz;
			size_t pgbrk = brk+PAGE_SIZE-1 & -PAGE_SIZE;
			memset((void *)brk, 0, pgbrk-brk & PAGE_SIZE-1);
			if (pgbrk-(size_t)base < this_max && mmap((void *)pgbrk, (size_t)base+this_max-pgbrk, prot, MAP_PRIVATE|MAP_FIXED|MAP_ANONYMOUS, -1, 0) == MAP_FAILED)
				goto error;
		}
	}
	for (i=0; ((size_t *)(base+dyn))[i]; i+=2)
		if (((size_t *)(base+dyn))[i]==DT_TEXTREL) {
			if (mprotect(map, map_len, PROT_READ|PROT_WRITE|PROT_EXEC) < 0)
				goto error;
			break;
		}
	dso->map = map;
	dso->map_len = map_len;
	dso->base = base;
	dso->dynv = (void *)(base+dyn);
	if (dso->tls_size) dso->tls_image = (void *)(base+tls_image);
	if (!runtime) reclaim_gaps(dso);
	free(allocated_buf);
	return map;
noexec:
	errno = ENOEXEC;
error:
	if (map!=MAP_FAILED) munmap(map, map_len);
	free(allocated_buf);
	return 0;
}

static int path_open(const char *name, const char *s, char *buf, size_t buf_size)
{
	size_t l;
	int fd;
	for (;;) {
		s += strspn(s, ":\n");
		l = strcspn(s, ":\n");
		if (l-1 >= INT_MAX) return -1;
		if (snprintf(buf, buf_size, "%.*s/%s", (int)l, s, name) < buf_size) {
			if ((fd = open(buf, O_RDONLY|O_CLOEXEC))>=0) return fd;
			switch (errno) {
			case ENOENT:
			case ENOTDIR:
			case EACCES:
			case ENAMETOOLONG:
				break;
			default:
				/* Any negative value but -1 will inhibit
				 * futher path search. */
				return -2;
			}
		}
		s += l;
	}
}

static int fixup_rpath(struct dso *p, char *buf, size_t buf_size)
{
	size_t n, l;
	const char *s, *t, *origin;
	char *d;
	if (p->rpath || !p->rpath_orig) return 0;
	if (!strchr(p->rpath_orig, '$')) {
		p->rpath = p->rpath_orig;
		return 0;
	}
	n = 0;
	s = p->rpath_orig;
	while ((t=strchr(s, '$'))) {
		if (strncmp(t, "$ORIGIN", 7) && strncmp(t, "${ORIGIN}", 9))
			return 0;
		s = t+1;
		n++;
	}
	if (n > SSIZE_MAX/PATH_MAX) return 0;

	if (p->kernel_mapped) {
		/* $ORIGIN searches cannot be performed for the main program
		 * when it is suid/sgid/AT_SECURE. This is because the
		 * pathname is under the control of the caller of execve.
		 * For libraries, however, $ORIGIN can be processed safely
		 * since the library's pathname came from a trusted source
		 * (either system paths or a call to dlopen). */
		if (libc.secure)
			return 0;
		l = readlink("/proc/self/exe", buf, buf_size);
		if (l == -1) switch (errno) {
		case ENOENT:
		case ENOTDIR:
		case EACCES:
			break;
		default:
			return -1;
		}
		if (l >= buf_size)
			return 0;
		buf[l] = 0;
		origin = buf;
	} else {
		origin = p->name;
	}
	t = strrchr(origin, '/');
	l = t ? t-origin : 0;
	p->rpath = malloc(strlen(p->rpath_orig) + n*l + 1);
	if (!p->rpath) return -1;

	d = p->rpath;
	s = p->rpath_orig;
	while ((t=strchr(s, '$'))) {
		memcpy(d, s, t-s);
		d += t-s;
		memcpy(d, origin, l);
		d += l;
		/* It was determined previously that the '$' is followed
		 * either by "ORIGIN" or "{ORIGIN}". */
		s = t + 7 + 2*(t[1]=='{');
	}
	strcpy(d, s);
	return 0;
}

static void decode_dyn(struct dso *p)
{
	size_t dyn[DYN_CNT];
	decode_vec(p->dynv, dyn, DYN_CNT);
	p->syms = (void *)(p->base + dyn[DT_SYMTAB]);
	p->strings = (void *)(p->base + dyn[DT_STRTAB]);
	if (dyn[0]&(1<<DT_HASH))
		p->hashtab = (void *)(p->base + dyn[DT_HASH]);
	if (dyn[0]&(1<<DT_RPATH))
		p->rpath_orig = (void *)(p->strings + dyn[DT_RPATH]);
	if (dyn[0]&(1<<DT_RUNPATH))
		p->rpath_orig = (void *)(p->strings + dyn[DT_RUNPATH]);
	if (search_vec(p->dynv, dyn, DT_GNU_HASH))
		p->ghashtab = (void *)(p->base + *dyn);
	if (search_vec(p->dynv, dyn, DT_VERSYM))
		p->versym = (void *)(p->base + *dyn);
}

static struct dso *load_library(const char *name, struct dso *needed_by)
{
	char buf[2*NAME_MAX+2];
	const char *pathname;
	unsigned char *map;
	struct dso *p, temp_dso = {0};
	int fd;
	struct stat st;
	size_t alloc_size;
	int n_th = 0;
	int is_self = 0;

	if (!*name) {
		errno = EINVAL;
		return 0;
	}

	/* Catch and block attempts to reload the implementation itself */
	if (name[0]=='l' && name[1]=='i' && name[2]=='b') {
		static const char *rp, reserved[] =
			"c\0pthread\0rt\0m\0dl\0util\0xnet\0";
		char *z = strchr(name, '.');
		if (z) {
			size_t l = z-name;
			for (rp=reserved; *rp && strncmp(name+3, rp, l-3); rp+=strlen(rp)+1);
			if (*rp) {
				if (ldd_mode) {
					/* Track which names have been resolved
					 * and only report each one once. */
					static unsigned reported;
					unsigned mask = 1U<<(rp-reserved);
					if (!(reported & mask)) {
						reported |= mask;
						dprintf(1, "\t%s => %s (%p)\n",
							name, ldso.name,
							ldso.base);
					}
				}
				is_self = 1;
			}
		}
	}
	if (!strcmp(name, ldso.name)) is_self = 1;
	if (is_self) {
		if (!ldso.prev) {
			tail->next = &ldso;
			ldso.prev = tail;
			tail = ldso.next ? ldso.next : &ldso;
		}
		return &ldso;
	}
	if (strchr(name, '/')) {
		pathname = name;
		fd = open(name, O_RDONLY|O_CLOEXEC);
	} else {
		/* Search for the name to see if it's already loaded */
		for (p=head->next; p; p=p->next) {
			if (p->shortname && !strcmp(p->shortname, name)) {
				p->refcnt++;
				return p;
			}
		}
		if (strlen(name) > NAME_MAX) return 0;
		fd = -1;
		if (env_path) fd = path_open(name, env_path, buf, sizeof buf);
		for (p=needed_by; fd == -1 && p; p=p->needed_by) {
			if (fixup_rpath(p, buf, sizeof buf) < 0)
				fd = -2; /* Inhibit further search. */
			if (p->rpath)
				fd = path_open(name, p->rpath, buf, sizeof buf);
		}
		if (fd == -1) {
			if (!sys_path) {
				char *prefix = 0;
				size_t prefix_len;
				if (ldso.name[0]=='/') {
					char *s, *t, *z;
					for (s=t=z=ldso.name; *s; s++)
						if (*s=='/') z=t, t=s;
					prefix_len = z-ldso.name;
					if (prefix_len < PATH_MAX)
						prefix = ldso.name;
				}
				if (!prefix) {
					prefix = "";
					prefix_len = 0;
				}
				char etc_ldso_path[prefix_len + 1
					+ sizeof "/etc/ld-musl-" LDSO_ARCH ".path"];
				snprintf(etc_ldso_path, sizeof etc_ldso_path,
					"%.*s/etc/ld-musl-" LDSO_ARCH ".path",
					(int)prefix_len, prefix);
				FILE *f = fopen(etc_ldso_path, "rbe");
				if (f) {
					if (getdelim(&sys_path, (size_t[1]){0}, 0, f) <= 0) {
						free(sys_path);
						sys_path = "";
					}
					fclose(f);
				} else if (errno != ENOENT) {
					sys_path = "";
				}
			}
			if (!sys_path) sys_path = "/lib:/usr/local/lib:/usr/lib";
			fd = path_open(name, sys_path, buf, sizeof buf);
		}
		pathname = buf;
	}
	if (fd < 0) return 0;
	if (fstat(fd, &st) < 0) {
		close(fd);
		return 0;
	}
	for (p=head->next; p; p=p->next) {
		if (p->dev == st.st_dev && p->ino == st.st_ino) {
			/* If this library was previously loaded with a
			 * pathname but a search found the same inode,
			 * setup its shortname so it can be found by name. */
			if (!p->shortname && pathname != name)
				p->shortname = strrchr(p->name, '/')+1;
			close(fd);
			p->refcnt++;
			return p;
		}
	}
	map = noload ? 0 : map_library(fd, &temp_dso);
	close(fd);
	if (!map) return 0;

	/* Allocate storage for the new DSO. When there is TLS, this
	 * storage must include a reservation for all pre-existing
	 * threads to obtain copies of both the new TLS, and an
	 * extended DTV capable of storing an additional slot for
	 * the newly-loaded DSO. */
	alloc_size = sizeof *p + strlen(pathname) + 1;
	if (runtime && temp_dso.tls_image) {
		size_t per_th = temp_dso.tls_size + temp_dso.tls_align
			+ sizeof(void *) * (tls_cnt+3);
		n_th = libc.threads_minus_1 + 1;
		if (n_th > SSIZE_MAX / per_th) alloc_size = SIZE_MAX;
		else alloc_size += n_th * per_th;
	}
	p = calloc(1, alloc_size);
	if (!p) {
		munmap(map, temp_dso.map_len);
		return 0;
	}
	memcpy(p, &temp_dso, sizeof temp_dso);
	decode_dyn(p);
	p->dev = st.st_dev;
	p->ino = st.st_ino;
	p->refcnt = 1;
	p->needed_by = needed_by;
	p->name = p->buf;
	strcpy(p->name, pathname);
	/* Add a shortname only if name arg was not an explicit pathname. */
	if (pathname != name) p->shortname = strrchr(p->name, '/')+1;
	if (p->tls_image) {
		p->tls_id = ++tls_cnt;
		tls_align = MAXP2(tls_align, p->tls_align);
#ifdef TLS_ABOVE_TP
		p->tls_offset = tls_offset + ( (tls_align-1) &
			-(tls_offset + (uintptr_t)p->tls_image) );
		tls_offset += p->tls_size;
#else
		tls_offset += p->tls_size + p->tls_align - 1;
		tls_offset -= (tls_offset + (uintptr_t)p->tls_image)
			& (p->tls_align-1);
		p->tls_offset = tls_offset;
#endif
		p->new_dtv = (void *)(-sizeof(size_t) &
			(uintptr_t)(p->name+strlen(p->name)+sizeof(size_t)));
		p->new_tls = (void *)(p->new_dtv + n_th*(tls_cnt+1));
	}

	tail->next = p;
	p->prev = tail;
	tail = p;

	if (ldd_mode) dprintf(1, "\t%s => %s (%p)\n", name, pathname, p->base);

	return p;
}

static void load_deps(struct dso *p)
{
	size_t i, ndeps=0;
	struct dso ***deps = &p->deps, **tmp, *dep;
	for (; p; p=p->next) {
		for (i=0; p->dynv[i]; i+=2) {
			if (p->dynv[i] != DT_NEEDED) continue;
			dep = load_library(p->strings + p->dynv[i+1], p);
			if (!dep) {
				error("Error loading shared library %s: %m (needed by %s)",
					p->strings + p->dynv[i+1], p->name);
				if (runtime) longjmp(*rtld_fail, 1);
				continue;
			}
			if (runtime) {
				tmp = realloc(*deps, sizeof(*tmp)*(ndeps+2));
				if (!tmp) longjmp(*rtld_fail, 1);
				tmp[ndeps++] = dep;
				tmp[ndeps] = 0;
				*deps = tmp;
			}
		}
	}
}

static void load_preload(char *s)
{
	int tmp;
	char *z;
	for (z=s; *z; s=z) {
		for (   ; *s && (isspace(*s) || *s==':'); s++);
		for (z=s; *z && !isspace(*z) && *z!=':'; z++);
		tmp = *z;
		*z = 0;
		load_library(s, 0);
		*z = tmp;
	}
}

static void make_global(struct dso *p)
{
	for (; p; p=p->next) p->global = 1;
}

static void do_mips_relocs(struct dso *p, size_t *got)
{
	size_t i, j, rel[2];
	unsigned char *base = p->base;
	i=0; search_vec(p->dynv, &i, DT_MIPS_LOCAL_GOTNO);
	if (p==&ldso) {
		got += i;
	} else {
		while (i--) *got++ += (size_t)base;
	}
	j=0; search_vec(p->dynv, &j, DT_MIPS_GOTSYM);
	i=0; search_vec(p->dynv, &i, DT_MIPS_SYMTABNO);
	Sym *sym = p->syms + j;
	rel[0] = (unsigned char *)got - base;
	for (i-=j; i; i--, sym++, rel[0]+=sizeof(size_t)) {
		rel[1] = sym-p->syms << 8 | R_MIPS_JUMP_SLOT;
		do_relocs(p, rel, sizeof rel, 2);
	}
}

static void reloc_all(struct dso *p)
{
	size_t dyn[DYN_CNT];
	for (; p; p=p->next) {
		if (p->relocated) continue;
		decode_vec(p->dynv, dyn, DYN_CNT);
		if (NEED_MIPS_GOT_RELOCS)
			do_mips_relocs(p, (void *)(p->base+dyn[DT_PLTGOT]));
		do_relocs(p, (void *)(p->base+dyn[DT_JMPREL]), dyn[DT_PLTRELSZ],
			2+(dyn[DT_PLTREL]==DT_RELA));
		do_relocs(p, (void *)(p->base+dyn[DT_REL]), dyn[DT_RELSZ], 2);
		do_relocs(p, (void *)(p->base+dyn[DT_RELA]), dyn[DT_RELASZ], 3);

		if (head != &ldso && p->relro_start != p->relro_end &&
		    mprotect(p->base+p->relro_start, p->relro_end-p->relro_start, PROT_READ) < 0) {
			error("Error relocating %s: RELRO protection failed: %m",
				p->name);
			if (runtime) longjmp(*rtld_fail, 1);
		}

		p->relocated = 1;
	}
}

static void kernel_mapped_dso(struct dso *p)
{
	size_t min_addr = -1, max_addr = 0, cnt;
	Phdr *ph = p->phdr;
	for (cnt = p->phnum; cnt--; ph = (void *)((char *)ph + p->phentsize)) {
		if (ph->p_type == PT_DYNAMIC) {
			p->dynv = (void *)(p->base + ph->p_vaddr);
		} else if (ph->p_type == PT_GNU_RELRO) {
			p->relro_start = ph->p_vaddr & -PAGE_SIZE;
			p->relro_end = (ph->p_vaddr + ph->p_memsz) & -PAGE_SIZE;
		}
		if (ph->p_type != PT_LOAD) continue;
		if (ph->p_vaddr < min_addr)
			min_addr = ph->p_vaddr;
		if (ph->p_vaddr+ph->p_memsz > max_addr)
			max_addr = ph->p_vaddr+ph->p_memsz;
	}
	min_addr &= -PAGE_SIZE;
	max_addr = (max_addr + PAGE_SIZE-1) & -PAGE_SIZE;
	p->map = p->base + min_addr;
	p->map_len = max_addr - min_addr;
	p->kernel_mapped = 1;
}

static void do_fini()
{
	struct dso *p;
	size_t dyn[DYN_CNT];
	for (p=fini_head; p; p=p->fini_next) {
		if (!p->constructed) continue;
		decode_vec(p->dynv, dyn, DYN_CNT);
		if (dyn[0] & (1<<DT_FINI_ARRAY)) {
			size_t n = dyn[DT_FINI_ARRAYSZ]/sizeof(size_t);
			size_t *fn = (size_t *)(p->base + dyn[DT_FINI_ARRAY])+n;
			while (n--) ((void (*)(void))*--fn)();
		}
#ifndef NO_LEGACY_INITFINI
		if ((dyn[0] & (1<<DT_FINI)) && dyn[DT_FINI])
			((void (*)(void))(p->base + dyn[DT_FINI]))();
#endif
	}
}

static void do_init_fini(struct dso *p)
{
	size_t dyn[DYN_CNT];
	int need_locking = libc.threads_minus_1;
	/* Allow recursive calls that arise when a library calls
	 * dlopen from one of its constructors, but block any
	 * other threads until all ctors have finished. */
	if (need_locking) pthread_mutex_lock(&init_fini_lock);
	for (; p; p=p->prev) {
		if (p->constructed) continue;
		p->constructed = 1;
		decode_vec(p->dynv, dyn, DYN_CNT);
		if (dyn[0] & ((1<<DT_FINI) | (1<<DT_FINI_ARRAY))) {
			p->fini_next = fini_head;
			fini_head = p;
		}
#ifndef NO_LEGACY_INITFINI
		if ((dyn[0] & (1<<DT_INIT)) && dyn[DT_INIT])
			((void (*)(void))(p->base + dyn[DT_INIT]))();
#endif
		if (dyn[0] & (1<<DT_INIT_ARRAY)) {
			size_t n = dyn[DT_INIT_ARRAYSZ]/sizeof(size_t);
			size_t *fn = (void *)(p->base + dyn[DT_INIT_ARRAY]);
			while (n--) ((void (*)(void))*fn++)();
		}
		if (!need_locking && libc.threads_minus_1) {
			need_locking = 1;
			pthread_mutex_lock(&init_fini_lock);
		}
	}
	if (need_locking) pthread_mutex_unlock(&init_fini_lock);
}

static void dl_debug_state(void)
{
}

weak_alias(dl_debug_state, _dl_debug_state);

void __reset_tls()
{
	pthread_t self = __pthread_self();
	struct dso *p;
	for (p=head; p; p=p->next) {
		if (!p->tls_id || !self->dtv[p->tls_id]) continue;
		memcpy(self->dtv[p->tls_id], p->tls_image, p->tls_len);
		memset((char *)self->dtv[p->tls_id]+p->tls_len, 0,
			p->tls_size - p->tls_len);
		if (p->tls_id == (size_t)self->dtv[0]) break;
	}
}

void *__copy_tls(unsigned char *mem)
{
	pthread_t td;
	struct dso *p;
	void **dtv;

#ifdef TLS_ABOVE_TP
	dtv = (void **)(mem + libc.tls_size) - (tls_cnt + 1);

	mem += -((uintptr_t)mem + sizeof(struct pthread)) & (tls_align-1);
	td = (pthread_t)mem;
	mem += sizeof(struct pthread);

	for (p=head; p; p=p->next) {
		if (!p->tls_id) continue;
		dtv[p->tls_id] = mem + p->tls_offset;
		memcpy(dtv[p->tls_id], p->tls_image, p->tls_len);
	}
#else
	dtv = (void **)mem;

	mem += libc.tls_size - sizeof(struct pthread);
	mem -= (uintptr_t)mem & (tls_align-1);
	td = (pthread_t)mem;

	for (p=head; p; p=p->next) {
		if (!p->tls_id) continue;
		dtv[p->tls_id] = mem - p->tls_offset;
		memcpy(dtv[p->tls_id], p->tls_image, p->tls_len);
	}
#endif
	dtv[0] = (void *)tls_cnt;
	td->dtv = td->dtv_copy = dtv;
	return td;
}

__attribute__((__visibility__("hidden")))
void *__tls_get_new(size_t *v)
{
	pthread_t self = __pthread_self();

	/* Block signals to make accessing new TLS async-signal-safe */
	sigset_t set;
	__block_all_sigs(&set);
	if (v[0]<=(size_t)self->dtv[0]) {
		__restore_sigs(&set);
		return (char *)self->dtv[v[0]]+v[1];
	}

	/* This is safe without any locks held because, if the caller
	 * is able to request the Nth entry of the DTV, the DSO list
	 * must be valid at least that far out and it was synchronized
	 * at program startup or by an already-completed call to dlopen. */
	struct dso *p;
	for (p=head; p->tls_id != v[0]; p=p->next);

	/* Get new DTV space from new DSO if needed */
	if (v[0] > (size_t)self->dtv[0]) {
		void **newdtv = p->new_dtv +
			(v[0]+1)*sizeof(void *)*a_fetch_add(&p->new_dtv_idx,1);
		memcpy(newdtv, self->dtv,
			((size_t)self->dtv[0]+1) * sizeof(void *));
		newdtv[0] = (void *)v[0];
		self->dtv = self->dtv_copy = newdtv;
	}

	/* Get new TLS memory from all new DSOs up to the requested one */
	unsigned char *mem;
	for (p=head; ; p=p->next) {
		if (!p->tls_id || self->dtv[p->tls_id]) continue;
		mem = p->new_tls + (p->tls_size + p->tls_align)
			* a_fetch_add(&p->new_tls_idx,1);
		mem += ((uintptr_t)p->tls_image - (uintptr_t)mem)
			& (p->tls_align-1);
		self->dtv[p->tls_id] = mem;
		memcpy(mem, p->tls_image, p->tls_len);
		if (p->tls_id == v[0]) break;
	}
	__restore_sigs(&set);
	return mem + v[1];
}

static void update_tls_size()
{
	libc.tls_size = ALIGN(
		(1+tls_cnt) * sizeof(void *) +
		tls_offset +
		sizeof(struct pthread) +
		tls_align * 2,
	tls_align);
}

/* Stage 1 of the dynamic linker is defined in dlstart.c. It calls the
 * following stage 2 and stage 3 functions via primitive symbolic lookup
 * since it does not have access to their addresses to begin with. */

/* Stage 2 of the dynamic linker is called after relative relocations 
 * have been processed. It can make function calls to static functions
 * and access string literals and static data, but cannot use extern
 * symbols. Its job is to perform symbolic relocations on the dynamic
 * linker itself, but some of the relocations performed may need to be
 * replaced later due to copy relocations in the main program. */

void __dls2(unsigned char *base, size_t *sp)
{
	Ehdr *ehdr = (void *)base;
	ldso.base = base;
	ldso.name = ldso.shortname = "libc.so";
	ldso.global = 1;
	ldso.phnum = ehdr->e_phnum;
	ldso.phdr = (void *)(base + ehdr->e_phoff);
	ldso.phentsize = ehdr->e_phentsize;
	kernel_mapped_dso(&ldso);
	decode_dyn(&ldso);

	/* Prepare storage for to save clobbered REL addends so they
	 * can be reused in stage 3. There should be very few. If
	 * something goes wrong and there are a huge number, abort
	 * instead of risking stack overflow. */
	size_t dyn[DYN_CNT];
	decode_vec(ldso.dynv, dyn, DYN_CNT);
	size_t *rel = (void *)(base+dyn[DT_REL]);
	size_t rel_size = dyn[DT_RELSZ];
	size_t symbolic_rel_cnt = 0;
	apply_addends_to = rel;
	for (; rel_size; rel+=2, rel_size-=2*sizeof(size_t))
		if (!IS_RELATIVE(rel[1])) symbolic_rel_cnt++;
	if (symbolic_rel_cnt >= ADDEND_LIMIT) a_crash();
	size_t addends[symbolic_rel_cnt+1];
	saved_addends = addends;

	head = &ldso;
	reloc_all(&ldso);

	ldso.relocated = 0;

	/* Call dynamic linker stage-3, __dls3, looking it up
	 * symbolically as a barrier against moving the address
	 * load across the above relocation processing. */
	struct symdef dls3_def = find_sym(&ldso, "__dls3", 0);
	((stage3_func)(ldso.base+dls3_def.sym->st_value))(sp);
}

/* Stage 3 of the dynamic linker is called with the dynamic linker/libc
 * fully functional. Its job is to load (if not already loaded) and
 * process dependencies and relocations for the main application and
 * transfer control to its entry point. */

_Noreturn void __dls3(size_t *sp)
{
	static struct dso app, vdso;
	size_t aux[AUX_CNT], *auxv;
	size_t i;
	char *env_preload=0;
	size_t vdso_base;
	int argc = *sp;
	char **argv = (void *)(sp+1);
	char **argv_orig = argv;
	char **envp = argv+argc+1;

	/* Setup early thread pointer in builtin_tls for ldso/libc itself to
	 * use during dynamic linking. If possible it will also serve as the
	 * thread pointer at runtime. */
	libc.tls_size = sizeof builtin_tls;
	if (__init_tp(__copy_tls((void *)builtin_tls)) < 0) {
		a_crash();
	}

	/* Find aux vector just past environ[] */
	for (i=argc+1; argv[i]; i++)
		if (!memcmp(argv[i], "LD_LIBRARY_PATH=", 16))
			env_path = argv[i]+16;
		else if (!memcmp(argv[i], "LD_PRELOAD=", 11))
			env_preload = argv[i]+11;
	auxv = (void *)(argv+i+1);

	decode_vec(auxv, aux, AUX_CNT);

	/* Only trust user/env if kernel says we're not suid/sgid */
	if ((aux[0]&0x7800)!=0x7800 || aux[AT_UID]!=aux[AT_EUID]
	  || aux[AT_GID]!=aux[AT_EGID] || aux[AT_SECURE]) {
		env_path = 0;
		env_preload = 0;
		libc.secure = 1;
	}
	libc.page_size = aux[AT_PAGESZ];
	libc.auxv = auxv;

	/* If the main program was already loaded by the kernel,
	 * AT_PHDR will point to some location other than the dynamic
	 * linker's program headers. */
	if (aux[AT_PHDR] != (size_t)ldso.phdr) {
		size_t interp_off = 0;
		size_t tls_image = 0;
		/* Find load address of the main program, via AT_PHDR vs PT_PHDR. */
		Phdr *phdr = app.phdr = (void *)aux[AT_PHDR];
		app.phnum = aux[AT_PHNUM];
		app.phentsize = aux[AT_PHENT];
		for (i=aux[AT_PHNUM]; i; i--, phdr=(void *)((char *)phdr + aux[AT_PHENT])) {
			if (phdr->p_type == PT_PHDR)
				app.base = (void *)(aux[AT_PHDR] - phdr->p_vaddr);
			else if (phdr->p_type == PT_INTERP)
				interp_off = (size_t)phdr->p_vaddr;
			else if (phdr->p_type == PT_TLS) {
				tls_image = phdr->p_vaddr;
				app.tls_len = phdr->p_filesz;
				app.tls_size = phdr->p_memsz;
				app.tls_align = phdr->p_align;
			}
		}
		if (app.tls_size) app.tls_image = (char *)app.base + tls_image;
		if (interp_off) ldso.name = (char *)app.base + interp_off;
		if ((aux[0] & (1UL<<AT_EXECFN))
		    && strncmp((char *)aux[AT_EXECFN], "/proc/", 6))
			app.name = (char *)aux[AT_EXECFN];
		else
			app.name = argv[0];
		kernel_mapped_dso(&app);
	} else {
		int fd;
		char *ldname = argv[0];
		size_t l = strlen(ldname);
		if (l >= 3 && !strcmp(ldname+l-3, "ldd")) ldd_mode = 1;
		argv++;
		while (argv[0] && argv[0][0]=='-' && argv[0][1]=='-') {
			char *opt = argv[0]+2;
			*argv++ = (void *)-1;
			if (!*opt) {
				break;
			} else if (!memcmp(opt, "list", 5)) {
				ldd_mode = 1;
			} else if (!memcmp(opt, "library-path", 12)) {
				if (opt[12]=='=') env_path = opt+13;
				else if (opt[12]) *argv = 0;
				else if (*argv) env_path = *argv++;
			} else if (!memcmp(opt, "preload", 7)) {
				if (opt[7]=='=') env_preload = opt+8;
				else if (opt[7]) *argv = 0;
				else if (*argv) env_preload = *argv++;
			} else {
				argv[0] = 0;
			}
		}
		argv[-1] = (void *)(argc - (argv-argv_orig));
		if (!argv[0]) {
			dprintf(2, "musl libc\n"
				"Version %s\n"
				"Dynamic Program Loader\n"
				"Usage: %s [options] [--] pathname%s\n",
				__libc_get_version(), ldname,
				ldd_mode ? "" : " [args]");
			_exit(1);
		}
		fd = open(argv[0], O_RDONLY);
		if (fd < 0) {
			dprintf(2, "%s: cannot load %s: %s\n", ldname, argv[0], strerror(errno));
			_exit(1);
		}
		runtime = 1;
		Ehdr *ehdr = (void *)map_library(fd, &app);
		if (!ehdr) {
			dprintf(2, "%s: %s: Not a valid dynamic program\n", ldname, argv[0]);
			_exit(1);
		}
		runtime = 0;
		close(fd);
		ldso.name = ldname;
		app.name = argv[0];
		aux[AT_ENTRY] = (size_t)app.base + ehdr->e_entry;
		/* Find the name that would have been used for the dynamic
		 * linker had ldd not taken its place. */
		if (ldd_mode) {
			for (i=0; i<app.phnum; i++) {
				if (app.phdr[i].p_type == PT_INTERP)
					ldso.name = (void *)(app.base
						+ app.phdr[i].p_vaddr);
			}
			dprintf(1, "\t%s (%p)\n", ldso.name, ldso.base);
		}
	}
	if (app.tls_size) {
		app.tls_id = tls_cnt = 1;
#ifdef TLS_ABOVE_TP
		app.tls_offset = 0;
		tls_offset = app.tls_size
			+ ( -((uintptr_t)app.tls_image + app.tls_size)
			& (app.tls_align-1) );
#else
		tls_offset = app.tls_offset = app.tls_size
			+ ( -((uintptr_t)app.tls_image + app.tls_size)
			& (app.tls_align-1) );
#endif
		tls_align = MAXP2(tls_align, app.tls_align);
	}
	app.global = 1;
	decode_dyn(&app);

	/* Attach to vdso, if provided by the kernel */
	if (search_vec(auxv, &vdso_base, AT_SYSINFO_EHDR)) {
		Ehdr *ehdr = (void *)vdso_base;
		Phdr *phdr = vdso.phdr = (void *)(vdso_base + ehdr->e_phoff);
		vdso.phnum = ehdr->e_phnum;
		vdso.phentsize = ehdr->e_phentsize;
		for (i=ehdr->e_phnum; i; i--, phdr=(void *)((char *)phdr + ehdr->e_phentsize)) {
			if (phdr->p_type == PT_DYNAMIC)
				vdso.dynv = (void *)(vdso_base + phdr->p_offset);
			if (phdr->p_type == PT_LOAD)
				vdso.base = (void *)(vdso_base - phdr->p_vaddr + phdr->p_offset);
		}
		vdso.name = "";
		vdso.shortname = "linux-gate.so.1";
		vdso.global = 1;
		vdso.relocated = 1;
		decode_dyn(&vdso);
		vdso.prev = &ldso;
		ldso.next = &vdso;
	}

	/* Initial dso chain consists only of the app. */
	head = tail = &app;

	/* Donate unused parts of app and library mapping to malloc */
	reclaim_gaps(&app);
	reclaim_gaps(&ldso);

	/* Load preload/needed libraries, add their symbols to the global
	 * namespace, and perform all remaining relocations. */
	if (env_preload) load_preload(env_preload);
	load_deps(&app);
	make_global(&app);

#ifndef DYNAMIC_IS_RO
	for (i=0; app.dynv[i]; i+=2)
		if (app.dynv[i]==DT_DEBUG)
			app.dynv[i+1] = (size_t)&debug;
#endif

	/* The main program must be relocated LAST since it may contin
	 * copy relocations which depend on libraries' relocations. */
	reloc_all(app.next);
	reloc_all(&app);

	update_tls_size();
	if (libc.tls_size > sizeof builtin_tls || tls_align > MIN_TLS_ALIGN) {
		void *initial_tls = calloc(libc.tls_size, 1);
		if (!initial_tls) {
			dprintf(2, "%s: Error getting %zu bytes thread-local storage: %m\n",
				argv[0], libc.tls_size);
			_exit(127);
		}
		if (__init_tp(__copy_tls(initial_tls)) < 0) {
			a_crash();
		}
	} else {
		size_t tmp_tls_size = libc.tls_size;
		pthread_t self = __pthread_self();
		/* Temporarily set the tls size to the full size of
		 * builtin_tls so that __copy_tls will use the same layout
		 * as it did for before. Then check, just to be safe. */
		libc.tls_size = sizeof builtin_tls;
		if (__copy_tls((void*)builtin_tls) != self) a_crash();
		libc.tls_size = tmp_tls_size;
	}
	static_tls_cnt = tls_cnt;

	if (ldso_fail) _exit(127);
	if (ldd_mode) _exit(0);

	/* Switch to runtime mode: any further failures in the dynamic
	 * linker are a reportable failure rather than a fatal startup
	 * error. */
	runtime = 1;

	debug.ver = 1;
	debug.bp = dl_debug_state;
	debug.head = head;
	debug.base = ldso.base;
	debug.state = 0;
	_dl_debug_state();

	__init_libc(envp, argv[0]);
	atexit(do_fini);
	errno = 0;
	do_init_fini(tail);

	CRTJMP((void *)aux[AT_ENTRY], argv-1);
	for(;;);
}

void *dlopen(const char *file, int mode)
{
	struct dso *volatile p, *orig_tail, *next;
	size_t orig_tls_cnt, orig_tls_offset, orig_tls_align;
	size_t i;
	int cs;
	jmp_buf jb;

	if (!file) return head;

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cs);
	pthread_rwlock_wrlock(&lock);
	__inhibit_ptc();

	p = 0;
	orig_tls_cnt = tls_cnt;
	orig_tls_offset = tls_offset;
	orig_tls_align = tls_align;
	orig_tail = tail;
	noload = mode & RTLD_NOLOAD;

	rtld_fail = &jb;
	if (setjmp(*rtld_fail)) {
		/* Clean up anything new that was (partially) loaded */
		if (p && p->deps) for (i=0; p->deps[i]; i++)
			if (p->deps[i]->global < 0)
				p->deps[i]->global = 0;
		for (p=orig_tail->next; p; p=next) {
			next = p->next;
			munmap(p->map, p->map_len);
			while (p->td_index) {
				void *tmp = p->td_index->next;
				free(p->td_index);
				p->td_index = tmp;
			}
			if (p->rpath != p->rpath_orig)
				free(p->rpath);
			free(p->deps);
			free(p);
		}
		tls_cnt = orig_tls_cnt;
		tls_offset = orig_tls_offset;
		tls_align = orig_tls_align;
		tail = orig_tail;
		tail->next = 0;
		p = 0;
		goto end;
	} else p = load_library(file, head);

	if (!p) {
		error(noload ?
			"Library %s is not already loaded" :
			"Error loading shared library %s: %m",
			file);
		goto end;
	}

	/* First load handling */
	if (!p->deps) {
		load_deps(p);
		if (p->deps) for (i=0; p->deps[i]; i++)
			if (!p->deps[i]->global)
				p->deps[i]->global = -1;
		if (!p->global) p->global = -1;
		reloc_all(p);
		if (p->deps) for (i=0; p->deps[i]; i++)
			if (p->deps[i]->global < 0)
				p->deps[i]->global = 0;
		if (p->global < 0) p->global = 0;
	}

	if (mode & RTLD_GLOBAL) {
		if (p->deps) for (i=0; p->deps[i]; i++)
			p->deps[i]->global = 1;
		p->global = 1;
	}

	update_tls_size();
	_dl_debug_state();
	orig_tail = tail;
end:
	__release_ptc();
	if (p) gencnt++;
	pthread_rwlock_unlock(&lock);
	if (p) do_init_fini(orig_tail);
	pthread_setcancelstate(cs, 0);
	return p;
}

static int invalid_dso_handle(void *h)
{
	struct dso *p;
	for (p=head; p; p=p->next) if (h==p) return 0;
	error("Invalid library handle %p", (void *)h);
	return 1;
}

void *__tls_get_addr(size_t *);

static void *do_dlsym(struct dso *p, const char *s, void *ra)
{
	size_t i;
	uint32_t h = 0, gh = 0;
	Sym *sym;
	if (p == head || p == RTLD_DEFAULT || p == RTLD_NEXT) {
		if (p == RTLD_DEFAULT) {
			p = head;
		} else if (p == RTLD_NEXT) {
			for (p=head; p && (unsigned char *)ra-p->map>p->map_len; p=p->next);
			if (!p) p=head;
			p = p->next;
		}
		struct symdef def = find_sym(p, s, 0);
		if (!def.sym) goto failed;
		if ((def.sym->st_info&0xf) == STT_TLS)
			return __tls_get_addr((size_t []){def.dso->tls_id, def.sym->st_value});
		return def.dso->base + def.sym->st_value;
	}
	if (invalid_dso_handle(p))
		return 0;
	if (p->ghashtab) {
		gh = gnu_hash(s);
		sym = gnu_lookup(s, gh, p);
	} else {
		h = sysv_hash(s);
		sym = sysv_lookup(s, h, p);
	}
	if (sym && (sym->st_info&0xf) == STT_TLS)
		return __tls_get_addr((size_t []){p->tls_id, sym->st_value});
	if (sym && sym->st_value && (1<<(sym->st_info&0xf) & OK_TYPES))
		return p->base + sym->st_value;
	if (p->deps) for (i=0; p->deps[i]; i++) {
		if (p->deps[i]->ghashtab) {
			if (!gh) gh = gnu_hash(s);
			sym = gnu_lookup(s, gh, p->deps[i]);
		} else {
			if (!h) h = sysv_hash(s);
			sym = sysv_lookup(s, h, p->deps[i]);
		}
		if (sym && (sym->st_info&0xf) == STT_TLS)
			return __tls_get_addr((size_t []){p->deps[i]->tls_id, sym->st_value});
		if (sym && sym->st_value && (1<<(sym->st_info&0xf) & OK_TYPES))
			return p->deps[i]->base + sym->st_value;
	}
failed:
	error("Symbol not found: %s", s);
	return 0;
}

int __dladdr(const void *addr, Dl_info *info)
{
	struct dso *p;
	Sym *sym;
	uint32_t nsym;
	char *strings;
	size_t i;
	void *best = 0;
	char *bestname;

	pthread_rwlock_rdlock(&lock);
	for (p=head; p && (unsigned char *)addr-p->map>p->map_len; p=p->next);
	pthread_rwlock_unlock(&lock);

	if (!p) return 0;

	sym = p->syms;
	strings = p->strings;
	if (p->hashtab) {
		nsym = p->hashtab[1];
	} else {
		uint32_t *buckets;
		uint32_t *hashval;
		buckets = p->ghashtab + 4 + (p->ghashtab[2]*sizeof(size_t)/4);
		sym += p->ghashtab[1];
		for (i = nsym = 0; i < p->ghashtab[0]; i++) {
			if (buckets[i] > nsym)
				nsym = buckets[i];
		}
		if (nsym) {
			nsym -= p->ghashtab[1];
			hashval = buckets + p->ghashtab[0] + nsym;
			do nsym++;
			while (!(*hashval++ & 1));
		}
	}

	for (; nsym; nsym--, sym++) {
		if (sym->st_value
		 && (1<<(sym->st_info&0xf) & OK_TYPES)
		 && (1<<(sym->st_info>>4) & OK_BINDS)) {
			void *symaddr = p->base + sym->st_value;
			if (symaddr > addr || symaddr < best)
				continue;
			best = symaddr;
			bestname = strings + sym->st_name;
			if (addr == symaddr)
				break;
		}
	}

	if (!best) return 0;

	info->dli_fname = p->name;
	info->dli_fbase = p->base;
	info->dli_sname = bestname;
	info->dli_saddr = best;

	return 1;
}

__attribute__((__visibility__("hidden")))
void *__dlsym(void *restrict p, const char *restrict s, void *restrict ra)
{
	void *res;
	pthread_rwlock_rdlock(&lock);
	res = do_dlsym(p, s, ra);
	pthread_rwlock_unlock(&lock);
	return res;
}

int dl_iterate_phdr(int(*callback)(struct dl_phdr_info *info, size_t size, void *data), void *data)
{
	struct dso *current;
	struct dl_phdr_info info;
	int ret = 0;
	for(current = head; current;) {
		info.dlpi_addr      = (uintptr_t)current->base;
		info.dlpi_name      = current->name;
		info.dlpi_phdr      = current->phdr;
		info.dlpi_phnum     = current->phnum;
		info.dlpi_adds      = gencnt;
		info.dlpi_subs      = 0;
		info.dlpi_tls_modid = current->tls_id;
		info.dlpi_tls_data  = current->tls_image;

		ret = (callback)(&info, sizeof (info), data);

		if (ret != 0) break;

		pthread_rwlock_rdlock(&lock);
		current = current->next;
		pthread_rwlock_unlock(&lock);
	}
	return ret;
}
#else
static int invalid_dso_handle(void *h)
{
	error("Invalid library handle %p", (void *)h);
	return 1;
}
void *dlopen(const char *file, int mode)
{
	error("Dynamic loading not supported");
	return 0;
}
void *__dlsym(void *restrict p, const char *restrict s, void *restrict ra)
{
	error("Symbol not found: %s", s);
	return 0;
}
int __dladdr (const void *addr, Dl_info *info)
{
	return 0;
}
#endif

int __dlinfo(void *dso, int req, void *res)
{
	if (invalid_dso_handle(dso)) return -1;
	if (req != RTLD_DI_LINKMAP) {
		error("Unsupported request %d", req);
		return -1;
	}
	*(struct link_map **)res = dso;
	return 0;
}

char *dlerror()
{
	pthread_t self = __pthread_self();
	if (!self->dlerror_flag) return 0;
	self->dlerror_flag = 0;
	char *s = self->dlerror_buf;
	if (s == (void *)-1)
		return "Dynamic linker failed to allocate memory for error message";
	else
		return s;
}

int dlclose(void *p)
{
	return invalid_dso_handle(p);
}

void __dl_thread_cleanup(void)
{
	pthread_t self = __pthread_self();
	if (self->dlerror_buf != (void *)-1)
		free(self->dlerror_buf);
}

static void error(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
#ifdef SHARED
	if (!runtime) {
		vdprintf(2, fmt, ap);
		dprintf(2, "\n");
		ldso_fail = 1;
		va_end(ap);
		return;
	}
#endif
	pthread_t self = __pthread_self();
	if (self->dlerror_buf != (void *)-1)
		free(self->dlerror_buf);
	size_t len = vsnprintf(0, 0, fmt, ap);
	va_end(ap);
	char *buf = malloc(len+1);
	if (buf) {
		va_start(ap, fmt);
		vsnprintf(buf, len+1, fmt, ap);
		va_end(ap);
	} else {
		buf = (void *)-1;	
	}
	self->dlerror_buf = buf;
	self->dlerror_flag = 1;
}
