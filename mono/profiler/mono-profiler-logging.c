/*
 * mono-profiler-logging.c: Logging profiler for Mono.
 *
 * Author:
 *   Massimiliano Mantione (massi@ximian.com)
 *
 * Copyright 2008-2009 Novell, Inc (http://www.novell.com)
 */
#include <config.h>
#include <mono/metadata/profiler.h> 
#include <mono/metadata/object.h> 
#include <mono/metadata/class.h> 
#include <mono/metadata/object.h>
#include <mono/metadata/metadata.h>
#include <mono/metadata/tabledefs.h>
#include <mono/metadata/class-internals.h>

#include <glib.h>

#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <Windows.h> 
#include <io.h> 
#include <ctype.h> 


typedef int ssize_t;

#ifdef WIN32
int gettimeofday(struct timeval *tp, void *tzp)
{
	time_t clock;
	struct tm tm;
	SYSTEMTIME wtm;
	GetLocalTime(&wtm);
	tm.tm_year     = wtm.wYear - 1900;
	tm.tm_mon     = wtm.wMonth - 1;
	tm.tm_mday     = wtm.wDay;
	tm.tm_hour     = wtm.wHour;
	tm.tm_min     = wtm.wMinute;
	tm.tm_sec     = wtm.wSecond;
	tm. tm_isdst    = -1;
	clock = mktime(&tm);
	tp->tv_sec = clock;
	tp->tv_usec = wtm.wMilliseconds * 1000;
	return (0);
}
#endif

#define HAS_OPROFILE 0

#if (HAS_OPROFILE)
#include <libopagent.h>
#endif

// Needed for heap analysis
//用来侦测一个对象是否还存活
extern gboolean mono_object_is_alive (MonoObject* obj);

typedef enum {
	MONO_PROFILER_FILE_BLOCK_KIND_INTRO = 1,
	MONO_PROFILER_FILE_BLOCK_KIND_END = 2,
	MONO_PROFILER_FILE_BLOCK_KIND_MAPPING = 3,
	MONO_PROFILER_FILE_BLOCK_KIND_LOADED = 4,
	MONO_PROFILER_FILE_BLOCK_KIND_UNLOADED = 5,
	MONO_PROFILER_FILE_BLOCK_KIND_EVENTS = 6,
	MONO_PROFILER_FILE_BLOCK_KIND_STATISTICAL = 7,
	MONO_PROFILER_FILE_BLOCK_KIND_HEAP_DATA = 8,
	MONO_PROFILER_FILE_BLOCK_KIND_HEAP_SUMMARY = 9,
	MONO_PROFILER_FILE_BLOCK_KIND_DIRECTIVES = 10
} MonoProfilerFileBlockKind;

typedef enum {
	MONO_PROFILER_DIRECTIVE_END = 0,
	MONO_PROFILER_DIRECTIVE_ALLOCATIONS_CARRY_CALLER = 1,
	MONO_PROFILER_DIRECTIVE_ALLOCATIONS_HAVE_STACK = 2,
	MONO_PROFILER_DIRECTIVE_ALLOCATIONS_CARRY_ID = 3,
	MONO_PROFILER_DIRECTIVE_LOADED_ELEMENTS_CARRY_ID = 4,
	MONO_PROFILER_DIRECTIVE_CLASSES_CARRY_ASSEMBLY_ID = 5,
	MONO_PROFILER_DIRECTIVE_METHODS_CARRY_WRAPPER_FLAG = 6,
	MONO_PROFILER_DIRECTIVE_LAST
} MonoProfilerDirectives;


#define MONO_PROFILER_LOADED_EVENT_MODULE     1
#define MONO_PROFILER_LOADED_EVENT_ASSEMBLY   2
#define MONO_PROFILER_LOADED_EVENT_APPDOMAIN  4
#define MONO_PROFILER_LOADED_EVENT_SUCCESS    8
#define MONO_PROFILER_LOADED_EVENT_FAILURE   16

typedef enum {
	MONO_PROFILER_EVENT_DATA_TYPE_OTHER = 0,
	MONO_PROFILER_EVENT_DATA_TYPE_METHOD = 1,
	MONO_PROFILER_EVENT_DATA_TYPE_CLASS = 2
} MonoProfilerEventDataType;

typedef struct _ProfilerEventData {
	union {
		gpointer address;
		gsize number;
	} data;
	unsigned int data_type:2;
	unsigned int code:4;
	//kind是何意？
	unsigned int kind:1;
	unsigned int value:25;
} ProfilerEventData;

//25位值的最大值
#define EVENT_VALUE_BITS (25)
#define MAX_EVENT_VALUE ((1<<EVENT_VALUE_BITS)-1)

typedef enum {
	MONO_PROFILER_EVENT_METHOD_JIT = 0,
	MONO_PROFILER_EVENT_METHOD_FREED = 1,
	MONO_PROFILER_EVENT_METHOD_CALL = 2,
	MONO_PROFILER_EVENT_METHOD_ALLOCATION_CALLER = 3,
	MONO_PROFILER_EVENT_METHOD_ALLOCATION_JIT_TIME_CALLER = 4
} MonoProfilerMethodEvents;
typedef enum {
	MONO_PROFILER_EVENT_CLASS_LOAD = 0,
	MONO_PROFILER_EVENT_CLASS_UNLOAD = 1,
	MONO_PROFILER_EVENT_CLASS_EXCEPTION = 2,
	MONO_PROFILER_EVENT_CLASS_MONITOR = 3,
	MONO_PROFILER_EVENT_CLASS_ALLOCATION = 4
} MonoProfilerClassEvents;
typedef enum {
	MONO_PROFILER_EVENT_RESULT_SUCCESS = 0,
	MONO_PROFILER_EVENT_RESULT_FAILURE = 4
} MonoProfilerEventResult;
#define MONO_PROFILER_EVENT_RESULT_MASK MONO_PROFILER_EVENT_RESULT_FAILURE
typedef enum {
	MONO_PROFILER_EVENT_THREAD = 1,
	MONO_PROFILER_EVENT_GC_COLLECTION = 2,
	MONO_PROFILER_EVENT_GC_MARK = 3,
	MONO_PROFILER_EVENT_GC_SWEEP = 4,
	MONO_PROFILER_EVENT_GC_RESIZE = 5,
	MONO_PROFILER_EVENT_GC_STOP_WORLD = 6,
	MONO_PROFILER_EVENT_GC_START_WORLD = 7,
	MONO_PROFILER_EVENT_JIT_TIME_ALLOCATION = 8,
	MONO_PROFILER_EVENT_STACK_SECTION = 9,
	MONO_PROFILER_EVENT_ALLOCATION_OBJECT_ID = 10,
	MONO_PROFILER_EVENT_OBJECT_MONITOR = 11
} MonoProfilerEvents;
typedef enum {
	MONO_PROFILER_EVENT_KIND_START = 0,
	MONO_PROFILER_EVENT_KIND_END = 1
} MonoProfilerEventKind;

#define MONO_PROFILER_GET_CURRENT_TIME(t) {\
	struct timeval current_time;\
	gettimeofday (&current_time, NULL);\
	(t) = (((guint64)current_time.tv_sec) * 1000000) + current_time.tv_usec;\
} while (0)

static gboolean use_fast_timer = FALSE;

#if (defined(__i386__) || defined(__x86_64__)) && ! defined(PLATFORM_WIN32)

#if defined(__i386__)
static const guchar cpuid_impl [] = {
	0x55,                   	/* push   %ebp */
	0x89, 0xe5,                	/* mov    %esp,%ebp */
	0x53,                   	/* push   %ebx */
	0x8b, 0x45, 0x08,             	/* mov    0x8(%ebp),%eax */
	0x0f, 0xa2,                	/* cpuid   */
	0x50,                   	/* push   %eax */
	0x8b, 0x45, 0x10,             	/* mov    0x10(%ebp),%eax */
	0x89, 0x18,                	/* mov    %ebx,(%eax) */
	0x8b, 0x45, 0x14,             	/* mov    0x14(%ebp),%eax */
	0x89, 0x08,                	/* mov    %ecx,(%eax) */
	0x8b, 0x45, 0x18,             	/* mov    0x18(%ebp),%eax */
	0x89, 0x10,                	/* mov    %edx,(%eax) */
	0x58,                   	/* pop    %eax */
	0x8b, 0x55, 0x0c,             	/* mov    0xc(%ebp),%edx */
	0x89, 0x02,                	/* mov    %eax,(%edx) */
	0x5b,                   	/* pop    %ebx */
	0xc9,                   	/* leave   */
	0xc3,                   	/* ret     */
};

typedef void (*CpuidFunc) (int id, int* p_eax, int* p_ebx, int* p_ecx, int* p_edx);

static int 
cpuid (int id, int* p_eax, int* p_ebx, int* p_ecx, int* p_edx) {
	int have_cpuid = 0;
#ifndef _MSC_VER
	__asm__  __volatile__ (
		"pushfl\n"
		"popl %%eax\n"
		"movl %%eax, %%edx\n"
		"xorl $0x200000, %%eax\n"
		"pushl %%eax\n"
		"popfl\n"
		"pushfl\n"
		"popl %%eax\n"
		"xorl %%edx, %%eax\n"
		"andl $0x200000, %%eax\n"
		"movl %%eax, %0"
		: "=r" (have_cpuid)
		:
		: "%eax", "%edx"
	);
#else
	__asm {
		pushfd
		pop eax
		mov edx, eax
		xor eax, 0x200000
		push eax
		popfd
		pushfd
		pop eax
		xor eax, edx
		and eax, 0x200000
		mov have_cpuid, eax
	}
#endif
	if (have_cpuid) {
		CpuidFunc func = (CpuidFunc) cpuid_impl;
		func (id, p_eax, p_ebx, p_ecx, p_edx);
		/*
		 * We use this approach because of issues with gcc and pic code, see:
		 * http://gcc.gnu.org/cgi-bin/gnatsweb.pl?cmd=view%20audit-trail&database=gcc&pr=7329
		__asm__ __volatile__ ("cpuid"
			: "=a" (*p_eax), "=b" (*p_ebx), "=c" (*p_ecx), "=d" (*p_edx)
			: "a" (id));
		*/
		return 1;
	}
	return 0;
}

static void detect_fast_timer (void) {
	int p_eax, p_ebx, p_ecx, p_edx;
	
	if (cpuid (0x1, &p_eax, &p_ebx, &p_ecx, &p_edx)) {
		if (p_edx & 0x10) {
			use_fast_timer = TRUE;
		} else {
			use_fast_timer = FALSE;
		}
	} else {
		use_fast_timer = FALSE;
	}
}
#endif

#if defined(__x86_64__)
static void detect_fast_timer (void) {
	guint32 op = 0x1;
	guint32 eax,ebx,ecx,edx;
	__asm__ __volatile__ ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(op));
	if (edx & 0x10) {
		use_fast_timer = TRUE;
	} else {
		use_fast_timer = FALSE;
	}
}
#endif

static __inline__ guint64 rdtsc(void) {
	guint32 hi, lo;
	__asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
	return ((guint64) lo) | (((guint64) hi) << 32);
}
#define MONO_PROFILER_GET_CURRENT_COUNTER(c) {\
	if (use_fast_timer) {\
		(c) = rdtsc ();\
	} else {\
		MONO_PROFILER_GET_CURRENT_TIME ((c));\
	}\
} while (0)
#else
static void detect_fast_timer (void) {
	use_fast_timer = FALSE;
}
#define MONO_PROFILER_GET_CURRENT_COUNTER(c) MONO_PROFILER_GET_CURRENT_TIME ((c))
#endif


#define CLASS_LAYOUT_PACKED_BITMAP_SIZE 64
#define CLASS_LAYOUT_NOT_INITIALIZED (0xFFFF)
typedef enum {
	HEAP_CODE_NONE = 0,
	HEAP_CODE_OBJECT = 1,
	HEAP_CODE_FREE_OBJECT_CLASS = 2,
	HEAP_CODE_MASK = 3
} HeapProfilerJobValueCode;
typedef struct _MonoProfilerClassData {
	union {
		guint64 compact;
		guint8 *extended;
	} bitmap;
	struct {
		guint16 slots;
		guint16 references;
	} layout;
	int refs_count; //预计算出的引用数量
	int assembly_id; //用来比对类是否相同
	int alive;			//类是否还可用，在assembly释放时会被标记为false
} MonoProfilerClassData;

typedef struct _MonoProfilerMethodData {
	gpointer code_start;
	guint32 code_size;
} MonoProfilerMethodData;

typedef struct _ClassIdMappingElement {
	char *name;
	guint32 id;
	MonoClass *klass;
	struct _ClassIdMappingElement *next_unwritten;
	MonoProfilerClassData data;
} ClassIdMappingElement;

typedef struct _MethodIdMappingElement {
	char *name;
	guint32 id;
	MonoMethod *method;
	struct _MethodIdMappingElement *next_unwritten;
	MonoProfilerMethodData data;
} MethodIdMappingElement;

typedef struct _ClassIdMapping {
	GHashTable *table;
	ClassIdMappingElement *unwritten;
	guint32 next_id;
} ClassIdMapping;

typedef struct _MethodIdMapping {
	GHashTable *table;
	MethodIdMappingElement *unwritten;
	guint32 next_id;
} MethodIdMapping;

typedef struct _LoadedElement {
	char *name;
	guint64 load_start_counter;
	guint64 load_end_counter;
	guint64 unload_start_counter;
	guint64 unload_end_counter;
	guint32 id;
	guint8 loaded;
	guint8 load_written;
	guint8 unloaded;
	guint8 unload_written;
} LoadedElement;
struct _ProfilerCodeBufferArray;
typedef struct _ProfilerCodeBuffer {
	gpointer start;
	gpointer end;
	struct {
		union {
			MonoMethod *method;
			MonoClass *klass;
			void *data;
			struct _ProfilerCodeBufferArray *sub_buffers;
		} data;
		guint16 value;
		guint16 type;
	} info;
} ProfilerCodeBuffer;

#define PROFILER_CODE_BUFFER_ARRAY_SIZE 64
typedef struct _ProfilerCodeBufferArray {
	int level;
	int number_of_buffers;
	ProfilerCodeBuffer buffers [PROFILER_CODE_BUFFER_ARRAY_SIZE];
} ProfilerCodeBufferArray;

typedef struct _ProfilerCodeChunk {
	gpointer start;
	gpointer end;
	gboolean destroyed;
	ProfilerCodeBufferArray *buffers;
} ProfilerCodeChunk;

typedef struct _ProfilerCodeChunks {
	int capacity;
	int number_of_chunks;
	ProfilerCodeChunk *chunks;
} ProfilerCodeChunks;


#define PROFILER_HEAP_SHOT_OBJECT_BUFFER_SIZE 1024
#define PROFILER_HEAP_SHOT_HEAP_BUFFER_SIZE 4096
#define PROFILER_HEAP_SHOT_WRITE_BUFFER_SIZE 4096

//用于保存在分配回调传入的MonoObject*
typedef struct _ProfilerHeapShotObjectBuffer {
	struct _ProfilerHeapShotObjectBuffer *next;
	MonoObject **next_free_slot;
	MonoObject **end;
	MonoObject **first_unprocessed_slot;
	MonoObject *buffer [PROFILER_HEAP_SHOT_OBJECT_BUFFER_SIZE];
} ProfilerHeapShotObjectBuffer;

 
typedef struct _ProfilerHeapObjectInfo
{
	MonoObject* obj;	//对象地址
	MonoDomain *	 domain; //对象所属domain
	int			size;   //对象大小
	int			alive;	//对象是否存活
	MonoClass*	klass;	//对象类地址
	guint32    class_id;  //类id
	int ref_start_indx;	//对象引用在引用缓冲区的起始
	int refs_count;		//对象的引用数
}ProfilerHeapObjectInfo;

typedef struct _ProfilerClassInfo
{
	char* name;	//类名
	guint32 class_id;	//类ID
	guint32 elem_class_id; //元素类ID(对于数组类有意义)
	int rank;	//类的维度(非0说明是数组类型)
	guint32 elem_is_valuetype; //类是否为值类型
	guint32 array_elem_size; //若类为数组且元素为值类型时此值有意义

	//后续字段是ClassIdMapping数据的备份
	union {
		guint64 compact;
		guint8 *extended;
	} bitmap;
	struct {
		guint16 slots;
		guint16 references;
	} layout;
}ProfilerClassInfo;

typedef struct _ProfilerHeapShotHeapBuffer {
	struct _ProfilerHeapShotHeapBuffer *next;
	struct _ProfilerHeapShotHeapBuffer *previous;
	ProfilerHeapObjectInfo* start_slot;
	ProfilerHeapObjectInfo* end_slot;
	ProfilerHeapObjectInfo  buffer [PROFILER_HEAP_SHOT_HEAP_BUFFER_SIZE];
} ProfilerHeapShotHeapBuffer;

typedef struct _ProfilerHeapShotHeapBuffers {
	ProfilerHeapShotHeapBuffer *buffers;
	ProfilerHeapShotHeapBuffer *last;
	ProfilerHeapShotHeapBuffer *current;
	//指向当前块current的第一个空闲槽位
	ProfilerHeapObjectInfo* first_free_slot;
} ProfilerHeapShotHeapBuffers;


typedef struct _ProfilerHeapShotWriteBuffer {
	struct _ProfilerHeapShotWriteBuffer *next;
	gpointer buffer [PROFILER_HEAP_SHOT_WRITE_BUFFER_SIZE];
} ProfilerHeapShotWriteBuffer;

//用于存储每个类的总览信息
typedef struct _ProfilerHeapShotClassSummary {
	struct {
		guint32 instances;
		guint32 bytes;
	} reachable;
	struct {
		guint32 instances;
		guint32 bytes;
	} unreachable;
} ProfilerHeapShotClassSummary;

//HeapShot总览信息，里面有每个类的实例总览
typedef struct _ProfilerHeapShotCollectionSummary {
	ProfilerHeapShotClassSummary *per_class_data;
	guint32 capacity;
} ProfilerHeapShotCollectionSummary;

typedef struct _ProfilerHeapShotWriteJob {
	struct _ProfilerHeapShotWriteJob *next;
	struct _ProfilerHeapShotWriteJob *next_unwritten;

	//指向buffers中第一个Buffer的第一个槽位
	gpointer *start;
	//指向buffers中当前使用Buffer的槽位
	gpointer *cursor;
	//指向buffers中最后一个Buffer的最后一个槽位
	gpointer *end;
	ProfilerHeapShotWriteBuffer *buffers;
	ProfilerHeapShotWriteBuffer **last_next;
	guint32 full_buffers;

	//是否进行HeapShot标记
	gboolean heap_shot_was_requested;

	//开始与结束时间戳
	guint64 start_counter;
	guint64 start_time;
	guint64 end_counter;
	guint64 end_time;
	guint32 collection;

	ProfilerHeapShotCollectionSummary summary;
	gboolean dump_heap_data;
} ProfilerHeapShotWriteJob;

typedef struct _ProfilerThreadStack {
	guint32 capacity;
	guint32 top;
	guint32 last_saved_top;
	guint32 last_written_frame;
	MonoMethod **stack;
	guint8 *method_is_jitted;
	guint32 *written_frames;
} ProfilerThreadStack;

typedef struct _ProfilerPerThreadData {
	ProfilerEventData *events;
	ProfilerEventData *next_free_event;
	ProfilerEventData *next_unreserved_event;
	ProfilerEventData *end_event;
	ProfilerEventData *first_unwritten_event;
	ProfilerEventData *first_unmapped_event;
	guint64 start_event_counter;
	guint64 last_event_counter;
	gsize thread_id;
	ProfilerHeapShotObjectBuffer *heap_shot_object_buffers;
	ProfilerThreadStack stack;
	struct _ProfilerPerThreadData* next;
} ProfilerPerThreadData;

#define MAX_STATISTICAL_CALL_CHAIN_DEPTH 128

typedef struct _ProfilerStatisticalHit {
	gpointer *address;
	MonoDomain *domain;
} ProfilerStatisticalHit;

typedef struct _ProfilerStatisticalData {
	ProfilerStatisticalHit *hits;
	unsigned int next_free_index;
	unsigned int end_index;
	unsigned int first_unwritten_index;
} ProfilerStatisticalData;

typedef struct _ProfilerUnmanagedSymbol {
	guint32 offset;
	guint32 size;
	guint32 id;
	guint32 index;
} ProfilerUnmanagedSymbol;

struct _ProfilerExecutableFile;
struct _ProfilerExecutableFileSectionRegion;

typedef struct _ProfilerExecutableMemoryRegionData {
	gpointer start;
	gpointer end;
	guint32 file_offset;
	char *file_name;
	guint32 id;
	gboolean is_new;
	
	struct _ProfilerExecutableFile *file;
	struct _ProfilerExecutableFileSectionRegion *file_region_reference;
	guint32 symbols_count;
	guint32 symbols_capacity;
	ProfilerUnmanagedSymbol *symbols;
} ProfilerExecutableMemoryRegionData;

typedef struct _ProfilerExecutableMemoryRegions {
	ProfilerExecutableMemoryRegionData **regions;
	guint32 regions_capacity;
	guint32 regions_count;
	guint32 next_id;
	guint32 next_unmanaged_function_id;
} ProfilerExecutableMemoryRegions;

/* Start of ELF definitions */
#define EI_NIDENT 16
typedef guint16 ElfHalf;
typedef guint32 ElfWord;
typedef gsize ElfAddr;
typedef gsize ElfOff;

typedef struct {
	unsigned char e_ident[EI_NIDENT];
	ElfHalf e_type;
	ElfHalf e_machine;
	ElfWord e_version;
	ElfAddr e_entry;
	ElfOff  e_phoff;
	ElfOff  e_shoff; // Section header table
	ElfWord e_flags;
	ElfHalf e_ehsize; // Header size
	ElfHalf e_phentsize;
	ElfHalf e_phnum;
	ElfHalf e_shentsize; // Section header entry size
	ElfHalf e_shnum; // Section header entries number
	ElfHalf e_shstrndx; // String table index
} ElfHeader;

#if (SIZEOF_VOID_P == 4)
typedef struct {
	ElfWord sh_name;
	ElfWord sh_type;
	ElfWord sh_flags;
	ElfAddr sh_addr; // Address in memory
	ElfOff  sh_offset; // Offset in file
	ElfWord sh_size;
	ElfWord sh_link;
	ElfWord sh_info;
	ElfWord sh_addralign;
	ElfWord sh_entsize;
} ElfSection;
typedef struct {
	ElfWord       st_name;
	ElfAddr       st_value;
	ElfWord       st_size;
	unsigned char st_info; // Use ELF32_ST_TYPE to get symbol type
	unsigned char st_other;
	ElfHalf       st_shndx; // Or one of SHN_ABS, SHN_COMMON or SHN_UNDEF.
} ElfSymbol;
#elif (SIZEOF_VOID_P == 8)
typedef struct {
	ElfWord sh_name;
	ElfWord sh_type;
	ElfOff sh_flags;
	ElfAddr sh_addr; // Address in memory
	ElfOff  sh_offset; // Offset in file
	ElfOff sh_size;
	ElfWord sh_link;
	ElfWord sh_info;
	ElfOff sh_addralign;
	ElfOff sh_entsize;
} ElfSection;
typedef struct {
	ElfWord       st_name;
	unsigned char st_info; // Use ELF_ST_TYPE to get symbol type
	unsigned char st_other;
	ElfHalf       st_shndx; // Or one of SHN_ABS, SHN_COMMON or SHN_UNDEF.
	ElfAddr       st_value;
	ElfAddr       st_size;
} ElfSymbol;
#else
#error Bad size of void pointer
#endif


#define ELF_ST_BIND(i)   ((i)>>4)
#define ELF_ST_TYPE(i)   ((i)&0xf)


typedef enum {
	EI_MAG0 = 0,
	EI_MAG1 = 1,
	EI_MAG2 = 2,
	EI_MAG3 = 3,
	EI_CLASS = 4,
	EI_DATA = 5
} ElfIdentFields;

typedef enum {
	ELF_FILE_TYPE_NONE = 0,
	ELF_FILE_TYPE_REL = 1,
	ELF_FILE_TYPE_EXEC = 2,
	ELF_FILE_TYPE_DYN = 3,
	ELF_FILE_TYPE_CORE = 4
} ElfFileType;

typedef enum {
	ELF_CLASS_NONE = 0,
	ELF_CLASS_32 = 1,
	ELF_CLASS_64 = 2
} ElfIdentClass;

typedef enum {
	ELF_DATA_NONE = 0,
	ELF_DATA_LSB = 1,
	ELF_DATA_MSB = 2
} ElfIdentData;

typedef enum {
	ELF_SHT_NULL = 0,
	ELF_SHT_PROGBITS = 1,
	ELF_SHT_SYMTAB = 2,
	ELF_SHT_STRTAB = 3,
	ELF_SHT_RELA = 4,
	ELF_SHT_HASH = 5,
	ELF_SHT_DYNAMIC = 6,
	ELF_SHT_NOTE = 7,
	ELF_SHT_NOBITS = 8,
	ELF_SHT_REL = 9,
	ELF_SHT_SHLIB = 10,
	ELF_SHT_DYNSYM = 11
} ElfSectionType;

typedef enum {
	ELF_STT_NOTYPE = 0,
	ELF_STT_OBJECT = 1,
	ELF_STT_FUNC = 2,
	ELF_STT_SECTION = 3,
	ELF_STT_FILE = 4
} ElfSymbolType;

typedef enum {
	ELF_SHF_WRITE = 1,
	ELF_SHF_ALLOC = 2,
	ELF_SHF_EXECINSTR = 4,
} ElfSectionFlags;

#define ELF_SHN_UNDEF       0
#define ELF_SHN_LORESERVE   0xff00
#define ELF_SHN_LOPROC      0xff00
#define ELF_SHN_HIPROC      0xff1f
#define ELF_SHN_ABS         0xfff1
#define ELF_SHN_COMMON      0xfff2
#define ELF_SHN_HIRESERVE   0xffff
/* End of ELF definitions */

typedef struct _ProfilerExecutableFileSectionRegion {
	ProfilerExecutableMemoryRegionData *region;
	guint8 *section_address;
	gsize section_offset;
} ProfilerExecutableFileSectionRegion;

typedef struct _ProfilerExecutableFile {
	guint32 reference_count;
	
	/* Used for mmap and munmap */
	int fd;
	guint8 *data;
	size_t length;
	
	/* File data */
	ElfHeader *header;
	guint8 *symbols_start;
	guint32 symbols_count;
	guint32 symbol_size;
	const char *symbols_string_table;
	const char *main_string_table;
	
	ProfilerExecutableFileSectionRegion *section_regions;
	
	struct _ProfilerExecutableFile *next_new_file;
} ProfilerExecutableFile;

typedef struct _ProfilerExecutableFiles {
	GHashTable *table;
	ProfilerExecutableFile *new_files;
} ProfilerExecutableFiles;


#define CLEANUP_WRITER_THREAD() do {profiler->writer_thread_terminated = TRUE;} while (0)
#define CHECK_WRITER_THREAD() (! profiler->writer_thread_terminated)

#ifndef PLATFORM_WIN32
#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>

#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#define MUTEX_TYPE pthread_mutex_t
#define INITIALIZE_PROFILER_MUTEX() pthread_mutex_init (&(profiler->mutex), NULL)
#define DELETE_PROFILER_MUTEX() pthread_mutex_destroy (&(profiler->mutex))
#define LOCK_PROFILER() do {/*LOG_WRITER_THREAD ("LOCK_PROFILER");*/ pthread_mutex_lock (&(profiler->mutex));} while (0)
#define UNLOCK_PROFILER() do {/*LOG_WRITER_THREAD ("UNLOCK_PROFILER");*/ pthread_mutex_unlock (&(profiler->mutex));} while (0)

#define THREAD_TYPE pthread_t
#define CREATE_WRITER_THREAD(f) pthread_create (&(profiler->data_writer_thread), NULL, ((void*(*)(void*))f), NULL)
#define CREATE_USER_THREAD(f) pthread_create (&(profiler->user_thread), NULL, ((void*(*)(void*))f), NULL)
#define EXIT_THREAD() pthread_exit (NULL);
#define WAIT_WRITER_THREAD() do {\
	if (CHECK_WRITER_THREAD ()) {\
		pthread_join (profiler->data_writer_thread, NULL);\
	}\
} while (0)
#define CURRENT_THREAD_ID() (gsize) pthread_self ()

#ifndef HAVE_KW_THREAD
static pthread_key_t pthread_profiler_key;
static pthread_once_t profiler_pthread_once = PTHREAD_ONCE_INIT;
static void
make_pthread_profiler_key (void) {
    (void) pthread_key_create (&pthread_profiler_key, NULL);
}
#define LOOKUP_PROFILER_THREAD_DATA() ((ProfilerPerThreadData*) pthread_getspecific (pthread_profiler_key))
#define SET_PROFILER_THREAD_DATA(x) (void) pthread_setspecific (pthread_profiler_key, (x))
#define ALLOCATE_PROFILER_THREAD_DATA() (void) pthread_once (&profiler_pthread_once, make_pthread_profiler_key)
#define FREE_PROFILER_THREAD_DATA() (void) pthread_key_delete (pthread_profiler_key)
#endif

#define EVENT_TYPE sem_t
#define WRITER_EVENT_INIT() do {\
	sem_init (&(profiler->enable_data_writer_event), 0, 0);\
	sem_init (&(profiler->wake_data_writer_event), 0, 0);\
	sem_init (&(profiler->done_data_writer_event), 0, 0);\
} while (0)
#define WRITER_EVENT_DESTROY() do {\
	sem_destroy (&(profiler->enable_data_writer_event));\
	sem_destroy (&(profiler->wake_data_writer_event));\
	sem_destroy (&(profiler->done_data_writer_event));\
} while (0)
#define WRITER_EVENT_WAIT() (void) sem_wait (&(profiler->wake_data_writer_event))
#define WRITER_EVENT_RAISE() (void) sem_post (&(profiler->wake_data_writer_event))
#define WRITER_EVENT_ENABLE_WAIT() (void) sem_wait (&(profiler->enable_data_writer_event))
#define WRITER_EVENT_ENABLE_RAISE() (void) sem_post (&(profiler->enable_data_writer_event))
#define WRITER_EVENT_DONE_WAIT() do {\
	if (CHECK_WRITER_THREAD ()) {\
		(void) sem_wait (&(profiler->done_data_writer_event));\
	}\
} while (0)
#define WRITER_EVENT_DONE_RAISE() (void) sem_post (&(profiler->done_data_writer_event))

#if 1
#define FILE_HANDLE_TYPE FILE*
#define OPEN_FILE() profiler->file = fopen (profiler->file_name, "wb");
#define WRITE_BUFFER(b,s) fwrite ((b), 1, (s), profiler->file)
#define FLUSH_FILE() fflush (profiler->file)
#define CLOSE_FILE() fclose (profiler->file)
//#else
//#define FILE_HANDLE_TYPE int
//#define OPEN_FILE() profiler->file = open (profiler->file_name, O_WRONLY|O_CREAT|O_TRUNC, 0664);
//#define WRITE_BUFFER(b,s) write (profiler->file, (b), (s))
//#define FLUSH_FILE() fsync (profiler->file)
//#define CLOSE_FILE() close (profiler->file)
#endif

#else

#include <windows.h>

#define MUTEX_TYPE CRITICAL_SECTION
#define INITIALIZE_PROFILER_MUTEX() InitializeCriticalSection (&(profiler->mutex))
#define DELETE_PROFILER_MUTEX() DeleteCriticalSection (&(profiler->mutex))
#define LOCK_PROFILER() EnterCriticalSection (&(profiler->mutex))
#define UNLOCK_PROFILER() LeaveCriticalSection (&(profiler->mutex))

#define THREAD_TYPE HANDLE
#define CREATE_WRITER_THREAD(f) CreateThread (NULL, (1*1024*1024), (f), NULL, 0, NULL)
#define CREATE_USER_THREAD(f) CreateThread (NULL, (1*1024*1024), (f), NULL, 0, NULL)
#define EXIT_THREAD() ExitThread (0);
#define WAIT_WRITER_THREAD() do {\
	if (CHECK_WRITER_THREAD ()) {\
		 WaitForSingleObject (profiler->data_writer_thread, INFINITE);\
	}\
} while (0)
#define CURRENT_THREAD_ID() (gsize) GetCurrentThreadId ()

#ifndef HAVE_KW_THREAD
static guint32 logging_profiler_thread_id = -1;
#define LOOKUP_PROFILER_THREAD_DATA() ((ProfilerPerThreadData*)TlsGetValue (logging_profiler_thread_id))
#define SET_PROFILER_THREAD_DATA(x) TlsSetValue (logging_profiler_thread_id, (x));
#define ALLOCATE_PROFILER_THREAD_DATA() logging_profiler_thread_id = TlsAlloc ()
#define FREE_PROFILER_THREAD_DATA() TlsFree (logging_profiler_thread_id)
#endif

#define EVENT_TYPE HANDLE
#define WRITER_EVENT_INIT() do {\
	profiler->enable_data_writer_event = CreateEvent (NULL, FALSE, FALSE, NULL);\
	profiler->wake_data_writer_event = CreateEvent (NULL, FALSE, FALSE, NULL);\
	profiler->done_data_writer_event = CreateEvent (NULL, FALSE, FALSE, NULL);\
} while (0)

#define WRITER_EVENT_DESTROY()  do {\
	CloseHandle (profiler->enable_data_writer_event);\
	CloseHandle (profiler->wake_data_writer_event);\
	CloseHandle (profiler->done_data_writer_event);\
} while (0)

#define WRITER_EVENT_WAIT() WaitForSingleObject (profiler->wake_data_writer_event, INFINITE)
#define WRITER_EVENT_RAISE() SetEvent (profiler->wake_data_writer_event)
#define WRITER_EVENT_ENABLE_WAIT() WaitForSingleObject (profiler->enable_data_writer_event, INFINITE)
#define WRITER_EVENT_ENABLE_RAISE() SetEvent (profiler->enable_data_writer_event)
#define WRITER_EVENT_DONE_WAIT() do {\
	if (CHECK_WRITER_THREAD ()) {\
		WaitForSingleObject (profiler->done_data_writer_event, INFINITE);\
	}\
} while (0)
#define WRITER_EVENT_DONE_RAISE() SetEvent (profiler->done_data_writer_event)


/*
===================================
		Profiler日志文件操作宏
===================================
*/
//#define FILE_HANDLE_TYPE FILE*
#define FILE_HANDLE_TYPE HANDLE
//#define OPEN_FILE() profiler->file = fopen (profiler->file_name, "wb");
//#define OPEN_FILE() profiler->file = fopen (profiler->file_name, "ab");
//#define WRITE_BUFFER(b,s) fwrite ((b), 1, (s), profiler->file)
//#define FLUSH_FILE() fflush (profiler->file)
//#define CLOSE_FILE() fclose (profiler->file);
 
#define OPEN_FILE() {\
	LARGE_INTEGER new_file_ptr;\
	LARGE_INTEGER dist_to_mov = {0};\
	profiler->file = \
	CreateFileA( profiler->file_name ,\
	GENERIC_READ | GENERIC_WRITE , \
	FILE_SHARE_READ , NULL , OPEN_ALWAYS ,FILE_ATTRIBUTE_NORMAL  , NULL);\
	if( profiler->file != INVALID_HANDLE_VALUE) SetFilePointerEx( profiler->file , dist_to_mov , &new_file_ptr , FILE_END );\
	}
#define WRITE_BUFFER(b,s)\
	if( profiler->file != INVALID_HANDLE_VALUE)  {\
	DWORD bytes2w = 0;\
	WriteFile( profiler->file , b , s , &bytes2w , NULL );}
#define FLUSH_FILE() FlushFileBuffers(profiler->file)
#define CLOSE_FILE() CloseHandle(profiler->file)

/*
=====================================
*/

#endif

#ifdef HAVE_KW_THREAD
static __thread ProfilerPerThreadData * tls_profiler_per_thread_data;
#define LOOKUP_PROFILER_THREAD_DATA() ((ProfilerPerThreadData*) tls_profiler_per_thread_data)
#define SET_PROFILER_THREAD_DATA(x) tls_profiler_per_thread_data = (x)
#define ALLOCATE_PROFILER_THREAD_DATA() /* nop */
#define FREE_PROFILER_THREAD_DATA() /* nop */
#endif
//分配每个线程所拥有的数据缓存块，并将其挂接到主Profiler结构中
//线程数据块保存在每个线程的TLS中
#define GET_PROFILER_THREAD_DATA(data) do {\
	ProfilerPerThreadData *_result = LOOKUP_PROFILER_THREAD_DATA ();\
	if (!_result) {\
		_result = profiler_per_thread_data_new (profiler->per_thread_buffer_size);\
		LOCK_PROFILER ();\
		_result->next = profiler->per_thread_data;\
		profiler->per_thread_data = _result;\
		UNLOCK_PROFILER ();\
		SET_PROFILER_THREAD_DATA (_result);\
	}\
	(data) = _result;\
} while (0)

#define PROFILER_FILE_WRITE_BUFFER_SIZE (profiler->write_buffer_size)
typedef struct _ProfilerFileWriteBuffer {
	struct _ProfilerFileWriteBuffer *next;
	guint8 buffer [MONO_ZERO_LEN_ARRAY];
} ProfilerFileWriteBuffer;

#define CHECK_PROFILER_ENABLED() do {\
	if (! profiler->profiler_enabled)\
		return;\
} while (0)
struct _MonoProfiler {

	//Profiler数据锁，用来多线程访问
	MUTEX_TYPE mutex;
	
	MonoProfileFlags flags;
	gboolean profiler_enabled;

	//Profiler日志文件信息
	char *file_name;
	char *file_name_suffix;
	FILE_HANDLE_TYPE file;
	
	guint64 start_time;
	guint64 start_counter;
	guint64 end_time;
	guint64 end_counter;
	
	guint64 last_header_counter;
	
	//函数与类查找表
	MethodIdMapping *methods;
	ClassIdMapping *classes;

	//用来记录在运行时生成的所有类信息
	//此表在退出前其中的内容可更新但不可
	//删除，里面是以<class_id,ProfilerClassInfo>
	//对的形式组织的
	GHashTable * classinfos;		
	
	guint32 loaded_element_next_free_id;
	GHashTable *loaded_assemblies;
	GHashTable *loaded_modules;
	GHashTable *loaded_appdomains;
	
	guint32 per_thread_buffer_size;
	guint32 statistical_buffer_size;
	ProfilerPerThreadData* per_thread_data;
	ProfilerStatisticalData *statistical_data;
	ProfilerStatisticalData *statistical_data_ready;
	ProfilerStatisticalData *statistical_data_second_buffer;
	int statistical_call_chain_depth;
	
	ProfilerCodeChunks code_chunks;
	

	//日志线程记录信息
	THREAD_TYPE data_writer_thread;
	THREAD_TYPE user_thread;
	EVENT_TYPE enable_data_writer_event;
	EVENT_TYPE wake_data_writer_event;
	EVENT_TYPE done_data_writer_event;
	gboolean terminate_writer_thread;
	gboolean writer_thread_terminated;
	

	/*
	=================================================
		Profiler日志缓冲区
	日志缓冲区是分块组织的，每次写满一块会再分配一块
	=================================================
	*/
	
	//缓冲区的根块
	ProfilerFileWriteBuffer *write_buffers;
	//当前正在使用的缓冲区块
	ProfilerFileWriteBuffer *current_write_buffer;
	//缓冲区块每一块的大小
	int write_buffer_size;
	//当前正在使用的缓冲区块的写位置
	int current_write_position;
	//已经写满的块数，一般是总块数-1
	int full_write_buffers;
	/*
	================================================= 
	=================================================
	*/

	ProfilerHeapShotWriteJob *heap_shot_write_jobs;

	//Profiler的总堆记录缓存，每次GC的时候会更新一次
	//此缓存在每次Alloc回调时增加对象，每次GC的时候会
	//收集每个线程分配的对象，剔除已释放的对象
	ProfilerHeapShotHeapBuffers heap;
	//用来快速查找堆对象的查找表
	GHashTable * heap_objs_tab;

	MonoObject** references_buf;
	int	references_count;
	int	references_buf_capacity;
	
	int command_port;
	
	int dump_next_heap_snapshots;
	gboolean heap_shot_was_requested;
	guint32 garbage_collection_counter;
	
	ProfilerExecutableMemoryRegions *executable_regions;
	ProfilerExecutableFiles executable_files;
	
	struct {
#if (HAS_OPROFILE)
		gboolean oprofile;
#endif
		gboolean jit_time;
		gboolean unreachable_objects;
		gboolean collection_summary;
		gboolean report_gc_events;
		gboolean heap_shot;
		gboolean track_stack;
		gboolean track_calls;
		gboolean save_allocation_caller;
		gboolean save_allocation_stack;
		gboolean allocations_carry_id;
	} action_flags;
};
static MonoProfiler *profiler;

//由于对象引用在创建时无法预测，故分配一个很大的值（32MB）
//作为引用缓存
#define PROFILER_REFERENCES_BUF_INIT_SIZE (32<<20)

/*
=============================
MonoObject** references_buf;
int	references_count;
int	references_buf_capacity;
用来维护引用缓冲区
=============================
*/

static void 
ensure_references_buf_capacity(int newsize)
{
	if (profiler->references_buf_capacity >= newsize)
		return;

	profiler->references_buf_capacity = newsize;
	profiler->references_buf = g_realloc(profiler->references_buf, sizeof(MonoObject*)*newsize);
}

static void 
clear_references_buf(void)
{
	profiler->references_count = 0;
}

static void 
destroy_references_buf(void)
{
	g_free(profiler->references_buf);
	profiler->references_buf = NULL;
	profiler->references_count = 0;
	profiler->references_buf_capacity = 0;
}

static void 
add_ref_to_references_buf(MonoObject* obj)
{
	int free_slot = profiler->references_count;

	g_assert(profiler->references_buf_capacity > 0);
	g_assert(profiler->references_count < profiler->references_buf_capacity);

	profiler->references_buf[free_slot] = obj;
	profiler->references_count++;
}


/*
============================
			注册的用户控制的内部调用函数
============================
*/
static void
enable_profiler (void) {
	profiler->profiler_enabled = TRUE;
}

static void flush_everything (void);

static void
disable_profiler (void) {
	profiler->profiler_enabled = FALSE;
	flush_everything ();
}

static void
request_heap_snapshot (void) {
	profiler->heap_shot_was_requested = TRUE;
	mono_gc_collect (mono_gc_max_generation ());
}

#define DEBUG_LOAD_EVENTS 0
#define DEBUG_MAPPING_EVENTS 0
#define DEBUG_LOGGING_PROFILER 0
#define DEBUG_HEAP_PROFILER 0
#define DEBUG_CLASS_BITMAPS 0
#define DEBUG_STATISTICAL_PROFILER 0
#define DEBUG_WRITER_THREAD 0
#define DEBUG_USER_THREAD 0
#define DEBUG_FILE_WRITES 0
#if (DEBUG_LOGGING_PROFILER || DEBUG_STATISTICAL_PROFILER || DEBUG_HEAP_PROFILER || DEBUG_WRITER_THREAD || DEBUG_FILE_WRITES)
#define LOG_WRITER_THREAD(m) printf ("WRITER-THREAD-LOG %s\n", m)
#else
#define LOG_WRITER_THREAD(m)
#endif
#if (DEBUG_LOGGING_PROFILER || DEBUG_STATISTICAL_PROFILER || DEBUG_HEAP_PROFILER || DEBUG_USER_THREAD || DEBUG_FILE_WRITES)
#define LOG_USER_THREAD(m) printf ("USER-THREAD-LOG %s\n", m)
#else
#define LOG_USER_THREAD(m)
#endif

#if DEBUG_LOGGING_PROFILER
static int event_counter = 0;
#define EVENT_MARK() printf ("[EVENT:%d]", ++ event_counter)
#endif

void save_heapshot(void);

static void
thread_stack_initialize_empty (ProfilerThreadStack *stack) {
	stack->capacity = 0;
	stack->top = 0;
	stack->last_saved_top = 0;
	stack->last_written_frame = 0;
	stack->stack = NULL;
	stack->method_is_jitted = NULL;
	stack->written_frames = NULL;
}

static void
thread_stack_free (ProfilerThreadStack *stack) {
	stack->capacity = 0;
	stack->top = 0;
	stack->last_saved_top = 0;
	stack->last_written_frame = 0;
	if (stack->stack != NULL) {
		g_free (stack->stack);
		stack->stack = NULL;
	}
	if (stack->method_is_jitted != NULL) {
		g_free (stack->method_is_jitted);
		stack->method_is_jitted = NULL;
	}
	if (stack->written_frames != NULL) {
		g_free (stack->written_frames);
		stack->written_frames = NULL;
	}
}

static void
thread_stack_initialize (ProfilerThreadStack *stack, guint32 capacity) {
	stack->capacity = capacity;
	stack->top = 0;
	stack->last_saved_top = 0;
	stack->last_written_frame = 0;
	stack->stack = g_new0 (MonoMethod*, capacity);
	stack->method_is_jitted = g_new0 (guint8, capacity);
	stack->written_frames = g_new0 (guint32, capacity);
}

static void
thread_stack_push_jitted (ProfilerThreadStack *stack, MonoMethod* method, gboolean method_is_jitted) {
	if (stack->top >= stack->capacity) {
		MonoMethod **old_stack = stack->stack;
		guint8 *old_method_is_jitted = stack->method_is_jitted;
		guint32 *old_written_frames = stack->written_frames;
		guint32 top = stack->top;
		guint32 last_saved_top = stack->last_saved_top;
		guint32 last_written_frame = stack->last_written_frame;
		thread_stack_initialize (stack, stack->capacity * 2);
		memcpy (stack->stack, old_stack, top * sizeof (MonoMethod*));
		memcpy (stack->method_is_jitted, old_method_is_jitted, top * sizeof (guint8));
		memcpy (stack->written_frames, old_written_frames, top * sizeof (guint32));
		g_free (old_stack);
		g_free (old_method_is_jitted);
		g_free (old_written_frames);
		stack->top = top;
		stack->last_saved_top = last_saved_top;
		stack->last_written_frame = last_written_frame;
	}
	stack->stack [stack->top] = method;
	stack->method_is_jitted [stack->top] = method_is_jitted;
	stack->top ++;
}

static inline void
thread_stack_push (ProfilerThreadStack *stack, MonoMethod* method) {
	thread_stack_push_jitted (stack, method, FALSE);
}

static MonoMethod*
thread_stack_pop (ProfilerThreadStack *stack) {
	if (stack->top > 0) {
		stack->top --;
		if (stack->last_saved_top > stack->top) {
			stack->last_saved_top = stack->top;
		}
		return stack->stack [stack->top];
	} else {
		return NULL;
	}
}

static MonoMethod*
thread_stack_top (ProfilerThreadStack *stack) {
	if (stack->top > 0) {
		return stack->stack [stack->top - 1];
	} else {
		return NULL;
	}
}

static gboolean
thread_stack_top_is_jitted (ProfilerThreadStack *stack) {
	if (stack->top > 0) {
		return stack->method_is_jitted [stack->top - 1];
	} else {
		return FALSE;
	}
}

static MonoMethod*
thread_stack_index_from_top (ProfilerThreadStack *stack, int index) {
	if (stack->top > index) {
		return stack->stack [stack->top - (index + 1)];
	} else {
		return NULL;
	}
}

static gboolean
thread_stack_index_from_top_is_jitted (ProfilerThreadStack *stack, int index) {
	if (stack->top > index) {
		return stack->method_is_jitted [stack->top - (index + 1)];
	} else {
		return FALSE;
	}
}

static inline void
thread_stack_push_safely (ProfilerThreadStack *stack, MonoMethod* method) {
	if (stack->stack != NULL) {
		thread_stack_push (stack, method);
	}
}

static inline void
thread_stack_push_jitted_safely (ProfilerThreadStack *stack, MonoMethod* method, gboolean method_is_jitted) {
	if (stack->stack != NULL) {
		thread_stack_push_jitted (stack, method, method_is_jitted);
	}
}

static inline int
thread_stack_count_unsaved_frames (ProfilerThreadStack *stack) {
	int result = stack->top - stack->last_saved_top;
	return (result > 0) ? result : 0;
}

static inline int
thread_stack_get_last_written_frame (ProfilerThreadStack *stack) {
	return stack->last_written_frame;
}

static inline void
thread_stack_set_last_written_frame (ProfilerThreadStack *stack, int last_written_frame) {
	stack->last_written_frame = last_written_frame;
}

static inline guint32
thread_stack_written_frame_at_index (ProfilerThreadStack *stack, int index) {
	return stack->written_frames [index];
}

static inline void
thread_stack_write_frame_at_index (ProfilerThreadStack *stack, int index, guint32 method_id_and_is_jitted) {
	stack->written_frames [index] = method_id_and_is_jitted;
}


static ClassIdMappingElement* class_id_mapping_element_get (MonoClass *klass) 
{
	return  g_hash_table_lookup(profiler->classes->table, (gconstpointer)klass );
}

static MethodIdMappingElement*
method_id_mapping_element_get (MonoMethod *method) 
{
	return g_hash_table_lookup(profiler->methods->table, (gconstpointer)method);
}

static MonoObject*
query_heap_object_info( MonoObject* object )
{
	return g_hash_table_lookup( profiler->heap_objs_tab , (gconstpointer)object );
}

#define BITS_TO_BYTES(v) do {\
	(v) += 7;\
	(v) &= ~7;\
	(v) >>= 3;\
} while (0)

static ClassIdMappingElement*
class_id_mapping_element_new (MonoClass *klass) {
	ClassIdMappingElement *result = g_new0 (ClassIdMappingElement, 1);
	GString *class_name = NULL;
	MonoImage *image = mono_class_get_image (klass);
	MonoAssembly *assembly = mono_image_get_assembly (image);
	guint32 assembly_id = loaded_element_get_id (profiler->loaded_assemblies, assembly);

	result->name = mono_type_full_name (mono_class_get_type (klass));

	//若类型名为空则使用类名作为类型名
	if(!strcmp(result->name,""))
	{ 
		g_free(result->name);
		result->name = NULL;
		class_name = g_string_new ("");
		append_class_name (class_name, klass, TRUE);
		result->name = g_string_free (class_name, FALSE);
	}

	result->klass = klass;
	result->next_unwritten = profiler->classes->unwritten;
	profiler->classes->unwritten = result;
	result->id = profiler->classes->next_id;
	profiler->classes->next_id ++;

	
	result->data.assembly_id = assembly_id;
	result->data.alive = 1;
	result->data.bitmap.compact = 0;
	result->data.bitmap.extended = 0;
	result->data.layout.slots = CLASS_LAYOUT_NOT_INITIALIZED;
	result->data.layout.references = CLASS_LAYOUT_NOT_INITIALIZED;
	
	g_hash_table_insert (profiler->classes->table, klass, result);


	return result;
}

 
static void
class_id_mapping_element_build_layout_bitmap (MonoClass *klass, ClassIdMappingElement *klass_id) {
	MonoClass *parent_class = mono_class_get_parent (klass);

	//引用字段数量
	int number_of_reference_fields = 0;
	int max_offset_of_reference_fields = 0;
	ClassIdMappingElement *parent_id;
	gpointer iter;
	MonoClassField *field;
		
	if (parent_class != NULL) {
		parent_id = class_id_mapping_element_get (parent_class);
		g_assert (parent_id != NULL);
		
		if (parent_id->data.layout.slots == CLASS_LAYOUT_NOT_INITIALIZED) {
			class_id_mapping_element_build_layout_bitmap (parent_class, parent_id);
		}
	} else {
		parent_id = NULL;
	}
	
	iter = NULL;
	while ((field = mono_class_get_fields (klass, &iter)) != NULL) {
		MonoType* field_type = mono_field_get_type (field);
		//跳过静态字段
		if (mono_field_get_flags (field) & 0x0010 /*FIELD_ATTRIBUTE_STATIC*/)
			continue;
		
		//若为引用类型字段
		if (MONO_TYPE_IS_REFERENCE (field_type)) {
			int field_offset = mono_field_get_offset (field) - sizeof (MonoObject);
			if (field_offset > max_offset_of_reference_fields) {
				max_offset_of_reference_fields = field_offset;
			}
			number_of_reference_fields ++;
		} else {//若字段为值类型
			MonoClass *field_class = mono_class_from_mono_type (field_type);
			if (field_class && mono_class_is_valuetype (field_class)) 
			{
				ClassIdMappingElement *field_id = class_id_mapping_element_get (field_class);
				g_assert (field_id != NULL);
				
				if (field_id->data.layout.slots == CLASS_LAYOUT_NOT_INITIALIZED) 
				{
					if (field_id != klass_id) 
					{
						class_id_mapping_element_build_layout_bitmap (field_class, field_id);
					} 
					else 
					{
						klass_id->data.bitmap.compact = 0;
						klass_id->data.layout.slots = 0;
						klass_id->data.layout.references = 0;
					}
				}
				
				if (field_id->data.layout.references > 0) 
				{
					int field_offset = mono_field_get_offset (field) - sizeof (MonoObject);
					int max_offset_reference_in_field = (field_id->data.layout.slots - 1) * sizeof (gpointer);
					
					if ((field_offset + max_offset_reference_in_field) > max_offset_of_reference_fields) {
						max_offset_of_reference_fields = field_offset + max_offset_reference_in_field;
					}
					
					number_of_reference_fields += field_id->data.layout.references;
				}
			}//if (field_class && mono_class_is_valuetype(field_class)) {
		}
	}//while ((field = mono_class_get_fields (klass, &iter)) != NULL) {
	

	if ((number_of_reference_fields == 0) && ((parent_id == NULL) || (parent_id->data.layout.references == 0))) 
	{
		klass_id->data.bitmap.compact = 0;
		klass_id->data.layout.slots = 0;
		klass_id->data.layout.references = 0;
	} else {
		if ((parent_id != NULL) && (parent_id->data.layout.references > 0)) {
			klass_id->data.layout.slots = parent_id->data.layout.slots;
			klass_id->data.layout.references = parent_id->data.layout.references;
		} else {
			klass_id->data.layout.slots = 0;
			klass_id->data.layout.references = 0;
		}
		
		if (number_of_reference_fields > 0) {
			klass_id->data.layout.slots += ((max_offset_of_reference_fields / sizeof (gpointer)) + 1);
			klass_id->data.layout.references += number_of_reference_fields;
		}
		
		if (klass_id->data.layout.slots <= CLASS_LAYOUT_PACKED_BITMAP_SIZE) { 
				klass_id->data.bitmap.compact = 0;
			if ((parent_id != NULL) && (parent_id->data.layout.references > 0)) { 
				klass_id->data.bitmap.compact = parent_id->data.bitmap.compact;
			}
		} else {
			int size_of_bitmap = klass_id->data.layout.slots;
			BITS_TO_BYTES (size_of_bitmap); 
			klass_id->data.bitmap.extended = g_malloc0 (size_of_bitmap);
			if ((parent_id != NULL) && (parent_id->data.layout.references > 0)) {
				int size_of_father_bitmap = parent_id->data.layout.slots;
				if (size_of_father_bitmap <= CLASS_LAYOUT_PACKED_BITMAP_SIZE) {
					int father_slot; 
					for (father_slot = 0; father_slot < size_of_father_bitmap; father_slot ++) {
						if (parent_id->data.bitmap.compact & (((guint64)1) << father_slot)) {
							klass_id->data.bitmap.extended [father_slot >> 3] |= (1 << (father_slot & 7));
						}
					}
				} else {
					BITS_TO_BYTES (size_of_father_bitmap); 
					memcpy (klass_id->data.bitmap.extended, parent_id->data.bitmap.extended, size_of_father_bitmap);
				}
			}
		}
	}
	 
	iter = NULL;
	while ((field = mono_class_get_fields (klass, &iter)) != NULL) {
		MonoType* field_type = mono_field_get_type (field);
		// For now, skip static fields
		if (mono_field_get_flags (field) & 0x0010 /*FIELD_ATTRIBUTE_STATIC*/)
			continue;
		 
		if (MONO_TYPE_IS_REFERENCE (field_type)) {
			int field_offset = mono_field_get_offset (field) - sizeof (MonoObject);
			int field_slot;
			g_assert ((field_offset % sizeof (gpointer)) == 0);
			field_slot = field_offset / sizeof (gpointer);
			if (klass_id->data.layout.slots <= CLASS_LAYOUT_PACKED_BITMAP_SIZE) {
				klass_id->data.bitmap.compact |= (((guint64)1) << field_slot);
			} else {
				klass_id->data.bitmap.extended [field_slot >> 3] |= (1 << (field_slot & 7));
			} 
		} else {
			MonoClass *field_class = mono_class_from_mono_type (field_type);
			if (field_class && mono_class_is_valuetype (field_class)) {
				ClassIdMappingElement *field_id = class_id_mapping_element_get (field_class);
				int field_offset;
				int field_slot;
				
				g_assert (field_id != NULL);
				field_offset = mono_field_get_offset (field) - sizeof (MonoObject);
				g_assert ((field_id->data.layout.references == 0) || ((field_offset % sizeof (gpointer)) == 0));
				field_slot = field_offset / sizeof (gpointer);
#if (DEBUG_CLASS_BITMAPS)
				printf ("[value type at offset %d, slot %d, with %d references in %d slots]", field_offset, field_slot, field_id->data.layout.references, field_id->data.layout.slots);
#endif
				
				if (field_id->data.layout.references > 0) {
					int sub_field_slot;
					if (field_id->data.layout.slots <= CLASS_LAYOUT_PACKED_BITMAP_SIZE) {
						for (sub_field_slot = 0; sub_field_slot < field_id->data.layout.slots; sub_field_slot ++) {
							if (field_id->data.bitmap.compact & (((guint64)1) << sub_field_slot)) {
								int actual_slot = field_slot + sub_field_slot;
								if (klass_id->data.layout.slots <= CLASS_LAYOUT_PACKED_BITMAP_SIZE) {
									klass_id->data.bitmap.compact |= (((guint64)1) << actual_slot);
								} else {
									klass_id->data.bitmap.extended [actual_slot >> 3] |= (1 << (actual_slot & 7));
								}
							}
						}
					} else {
						for (sub_field_slot = 0; sub_field_slot < field_id->data.layout.slots; sub_field_slot ++) {
							if (field_id->data.bitmap.extended [sub_field_slot >> 3] & (1 << (sub_field_slot & 7))) {
								int actual_slot = field_slot + sub_field_slot;
								if (klass_id->data.layout.slots <= CLASS_LAYOUT_PACKED_BITMAP_SIZE) {
									klass_id->data.bitmap.compact |= (((guint64)1) << actual_slot);
								} else {
									klass_id->data.bitmap.extended [actual_slot >> 3] |= (1 << (actual_slot & 7));
								}
							}
						}
					}
				}
			}
		}
	}

}

//调用此函数前Class的layout必须构建完成
void
class_id_mapping_element_build_refs(ClassIdMappingElement * class_id )
{
	int references = 0;
	int slot = 0; 
	MonoClass* klass = class_id->klass;

	if( class_id->data.layout.slots == CLASS_LAYOUT_NOT_INITIALIZED )
		return;

	//统计此类所有引用
	for (slot = 0; slot < class_id->data.layout.slots; slot++) 
	{
		gboolean slot_has_reference;
		if (class_id->data.layout.slots <= CLASS_LAYOUT_PACKED_BITMAP_SIZE) {
			if (class_id->data.bitmap.compact & (((guint64)1) << slot)) {
				slot_has_reference = TRUE;
			} else {
				slot_has_reference = FALSE;
			}
		} else {
			if (class_id->data.bitmap.extended[slot >> 3] & (1 << (slot & 7))) {
				slot_has_reference = TRUE;
			} else {
				slot_has_reference = FALSE;
			}
		} 
		if (slot_has_reference)
		{ 
			references++; 
		}
	}
	class_id->data.refs_count = references;
}


static 
	ProfilerClassInfo* query_classinfo_by_id( guint32 id )
{
	return (ProfilerClassInfo*)g_hash_table_lookup( profiler->classinfos , GUINT_TO_POINTER(id) );
}
 

//用ClassIdMappingElement的信息更新ClassInfo
static 
	void update_classinfo( ProfilerClassInfo* classinfo , ClassIdMappingElement* class_id )
{
	g_assert( classinfo );
	g_assert( class_id );

	classinfo->class_id = class_id->id;
	classinfo->elem_class_id = 0;

	classinfo->rank = mono_class_get_rank( class_id->klass );
	//若类为数组类型
	if( classinfo->rank )
	{
		//获得ElementClass的ID
		MonoClass* elem_class = mono_class_get_element_class(class_id->klass);
		ClassIdMappingElement* elem_class_id = class_id_mapping_element_get(elem_class);
		g_assert( elem_class->inited );
		g_assert(elem_class_id); 
		classinfo->elem_class_id = elem_class_id->id;
		//数组类元素是否为值类型
		classinfo->elem_is_valuetype = 
			mono_class_is_valuetype( mono_class_get_element_class(class_id->klass));

		if( classinfo->elem_is_valuetype )
		{
			classinfo->array_elem_size = mono_array_element_size( class_id->klass );
		}else{
			classinfo->array_elem_size = 0;
		}
	}

	if( classinfo->name )
	{
		GString* class_name = NULL;
		g_free(classinfo->name);
		class_name = g_string_new (class_id->name); 
		classinfo->name = g_string_free (class_name, FALSE);
	}

	 
	if( classinfo->layout.slots > CLASS_LAYOUT_PACKED_BITMAP_SIZE)
	{
		if( classinfo->bitmap.extended )
		{
			g_free( classinfo->bitmap.extended );
			classinfo->bitmap.extended = NULL;
		}
	}

	//若布局的bitmap超过了64位，则拷贝扩展bitmap
	if( class_id->data.layout.slots > CLASS_LAYOUT_PACKED_BITMAP_SIZE )
	{
		int size_of_bitmap = class_id->data.layout.slots; 
		BITS_TO_BYTES (size_of_bitmap);  
		classinfo->bitmap.extended = g_malloc0 (size_of_bitmap);
		memcpy( classinfo->bitmap.extended , class_id->data.bitmap.extended , size_of_bitmap);
	}else{
		classinfo->bitmap.compact = class_id->data.bitmap.compact;
	}

	classinfo->layout.slots = class_id->data.layout.slots;
	classinfo->layout.references = class_id->data.layout.references;
}

//先查询classinfo表中是否存在同id对象，若不存在创建并返回
static 
	ProfilerClassInfo* request_a_classinfo( ClassIdMappingElement* elem )
{
	ProfilerClassInfo* classinfo = NULL;
	g_assert( elem );
	classinfo = query_classinfo_by_id( elem->id );
	if( !classinfo )
	{
		classinfo = g_new0(ProfilerClassInfo,1);
		g_hash_table_insert( profiler->classinfos , GUINT_TO_POINTER(elem->id) , classinfo );
	}
	return classinfo;
}


static void write_mapping_block (gsize thread_id);
 

//尝试向profiler类注册表里加入指定类
static void
	try_insert_class_id_mapping_element( MonoClass* klass )
{ 
	ClassIdMappingElement *element = class_id_mapping_element_get (klass);
	if (element == NULL) 
	{ //在类表中未找到当前类
		element = class_id_mapping_element_new (klass);
		

	}else{//此处需要验证新加入的类是否与已存在的类相同
		//若已经在查找表中存在，且重新载入有可能类中的成员有变故需重新
		//计算类布局

		MonoImage *image = mono_class_get_image (klass);
		MonoAssembly *assembly = mono_image_get_assembly (image);
		guint32 assembly_id = loaded_element_get_id (profiler->loaded_assemblies, assembly);
		//表中类已经过期!
		if( element->data.assembly_id != assembly_id || 
			element->data.alive )
		{
			//重置名称
			if( element->name )
			{
				g_free( element->name );
				element->name = NULL;
			}

			element->name = mono_type_full_name (mono_class_get_type (klass)); 
			//若类型名为空则使用类名作为类型名
			if(!strcmp(element->name,""))
			{ 
				GString *class_name = NULL;
				g_free(element->name);
				element->name = NULL;
				class_name = g_string_new ("");
				append_class_name (class_name, klass, TRUE);
				element->name = g_string_free (class_name, FALSE);
			}
			element->data.assembly_id = assembly_id;
			element->data.bitmap.compact = 0;
			if( element->data.bitmap.extended )
			{
				g_free( element->data.bitmap.extended );
				element->data.bitmap.extended = 0;
			}

			element->data.alive =1; 
			element->next_unwritten = profiler->classes->unwritten;
			profiler->classes->unwritten = element;
			element->id = profiler->classes->next_id;
			profiler->classes->next_id ++;
			write_mapping_block(0); 
		}

		//需要重新构建布局
		if( element->data.alive )
		{
			element->data.layout.slots = CLASS_LAYOUT_NOT_INITIALIZED;
			element->data.layout.references = CLASS_LAYOUT_NOT_INITIALIZED;
		}
	}
	 
}

void mark_all_unload_assembly_class_dead_cb(gpointer key, gpointer value, gpointer user_data)
{
	int assembly_id = *((int*)user_data);
	ClassIdMappingElement *class_elem = value;  
	if( class_elem->data.assembly_id == assembly_id )
	{
		class_elem->data.alive = 0;
	}
}

static void
mark_all_unload_assembly_class_dead( int assembly_id )
{
	g_hash_table_foreach(profiler->classes->table, mark_all_unload_assembly_class_dead_cb, &assembly_id); 
}

//查询某类对象的最大引用数量
static int 
query_class_id_mapping_element_references_count(MonoClass* klass)
{
	ClassIdMappingElement* class_elem =  class_id_mapping_element_get(klass);
	if (class_elem)
	{
		//若该类的bitmap已经初始化，则直接返回引用数
		if (class_elem->data.layout.slots != CLASS_LAYOUT_NOT_INITIALIZED)
		{
			return class_elem->data.refs_count;
		} 
	}
	return 0;
}

//扫描整个堆，计算所有对象的引用数量加和
//此过程只访问profiler内部数据
static int 
calc_heap_total_references_buf_size(void)
{
	int total_ref_count = 0;
	ProfilerHeapShotHeapBuffer *current_buffer = profiler->heap.buffers;
	ProfilerHeapObjectInfo* current_slot = current_buffer->start_slot;

	//对Profiler记录的堆中的所有对象，查看其是否还活着
	while (current_slot != profiler->heap.first_free_slot)
	{
		ProfilerHeapObjectInfo obj_info = *current_slot;
		ClassIdMappingElement* class_id = NULL;
		//统计每个对象的引用数
		total_ref_count += query_class_id_mapping_element_references_count(obj_info.klass); 
		current_slot++;
		if (current_slot == current_buffer->end_slot)
		{
			current_buffer = current_buffer->next;
			g_assert(current_buffer != NULL);
			current_slot = current_buffer->start_slot;
		}
	}
	return total_ref_count;
}

static MethodIdMappingElement*
method_id_mapping_element_new (MonoMethod *method) {
	MethodIdMappingElement *result = g_new (MethodIdMappingElement, 1);
	char *signature = mono_signature_get_desc (mono_method_signature (method), TRUE);
	
	result->name = g_strdup_printf ("%s (%s)", mono_method_get_name (method), signature);
	g_free (signature);
	result->method = method;
	result->next_unwritten = profiler->methods->unwritten;
	profiler->methods->unwritten = result;
	result->id = profiler->methods->next_id;
	profiler->methods->next_id ++;
	g_hash_table_insert (profiler->methods->table, method, result);
	
	result->data.code_start = NULL;
	result->data.code_size = 0;
	
#if (DEBUG_MAPPING_EVENTS)
	printf ("Created new METHOD mapping element \"%s\" (%p)[%d]\n", result->name, method, result->id);
#endif
	return result;
}


//销毁函数映射表元素
static void
method_id_mapping_element_destroy (gpointer element) {
	MethodIdMappingElement *e = (MethodIdMappingElement*) element;
	if (e->name)
		g_free (e->name);
	g_free (element);
}
 
//销毁类映射表元素
static void
class_id_mapping_element_destroy (gpointer element) {
	ClassIdMappingElement *e = (ClassIdMappingElement*) element;
	if (e->name)
		g_free (e->name);
	if ((e->data.layout.slots != CLASS_LAYOUT_NOT_INITIALIZED) && (e->data.layout.slots > CLASS_LAYOUT_PACKED_BITMAP_SIZE))
		g_free (e->data.bitmap.extended);
	g_free (element);
}

static void 
heap_object_info_element_destroy(gpointer element )
{ 
}

//创建函数映射表
static MethodIdMapping*
method_id_mapping_new (void) {
	MethodIdMapping *result = g_new (MethodIdMapping, 1);
	//result->table = g_hash_table_new_full (mono_aligned_addr_hash, NULL, NULL, method_id_mapping_element_destroy);
	result->table = g_hash_table_new_full (g_direct_hash, NULL, NULL, method_id_mapping_element_destroy);
	result->unwritten = NULL;
	result->next_id = 1;
	return result;
}

//创建类映射表
static ClassIdMapping*
class_id_mapping_new (void) {
	ClassIdMapping *result = g_new (ClassIdMapping, 1);
	//result->table = g_hash_table_new_full (mono_aligned_addr_hash, NULL, NULL, class_id_mapping_element_destroy);
	result->table = g_hash_table_new_full (g_direct_hash, NULL, NULL, class_id_mapping_element_destroy);
	result->unwritten = NULL;
	result->next_id = 1;
	return result;
}

static void
method_id_mapping_destroy (MethodIdMapping *map) {
	g_hash_table_destroy (map->table);
	g_free (map);
}


static void
class_id_mapping_destroy (ClassIdMapping *map) {
	g_hash_table_destroy (map->table);
	g_free (map);
}

#if (DEBUG_LOAD_EVENTS)
static void
print_load_event (const char *event_name, GHashTable *table, gpointer item, LoadedElement *element);
#endif

static LoadedElement*
loaded_element_load_start (GHashTable *table, gpointer item) {
	LoadedElement *element = g_new0 (LoadedElement, 1);
	element->id = profiler->loaded_element_next_free_id;
	profiler->loaded_element_next_free_id ++;
#if (DEBUG_LOAD_EVENTS)
	print_load_event ("LOAD START", table, item, element);
#endif
	MONO_PROFILER_GET_CURRENT_COUNTER (element->load_start_counter);
	g_hash_table_insert (table, item, element);
	return element;
}

static LoadedElement*
loaded_element_load_end (GHashTable *table, gpointer item, char *name) {
	LoadedElement *element = g_hash_table_lookup (table, item);
#if (DEBUG_LOAD_EVENTS)
	print_load_event ("LOAD END", table, item, element);
#endif
	g_assert (element != NULL);
	MONO_PROFILER_GET_CURRENT_COUNTER (element->load_end_counter);
	element->name = name;
	element->loaded = TRUE;
	return element;
}

static LoadedElement*
loaded_element_unload_start (GHashTable *table, gpointer item) {
	LoadedElement *element = g_hash_table_lookup (table, item);
#if (DEBUG_LOAD_EVENTS)
	print_load_event ("UNLOAD START", table, item, element);
#endif
	g_assert (element != NULL);
	MONO_PROFILER_GET_CURRENT_COUNTER (element->unload_start_counter);
	return element;
}

static LoadedElement*
loaded_element_unload_end (GHashTable *table, gpointer item) {
	LoadedElement *element = g_hash_table_lookup (table, item);
#if (DEBUG_LOAD_EVENTS)
	print_load_event ("UNLOAD END", table, item, element);
#endif
	g_assert (element != NULL);
	MONO_PROFILER_GET_CURRENT_COUNTER (element->unload_end_counter);
	element->unloaded = TRUE;
	return element;
}

static LoadedElement*
loaded_element_find (GHashTable *table, gpointer item) {
	LoadedElement *element = g_hash_table_lookup (table, item);
	return element;
}

static guint32
loaded_element_get_id (GHashTable *table, gpointer item) {
	LoadedElement *element = loaded_element_find (table, item);
	if (element != NULL) {
		return element->id;
	} else {
		return 0;
	}
}

static void
loaded_element_destroy (gpointer element) {
	if (((LoadedElement*)element)->name)
		g_free (((LoadedElement*)element)->name);
	g_free (element);
}

//销毁类信息
static void 
class_info_destroy( gpointer element )
{
	ProfilerClassInfo* classinfo = (ProfilerClassInfo*)element;
	g_free( classinfo->name );

	if( classinfo->layout.slots >  CLASS_LAYOUT_PACKED_BITMAP_SIZE)
	{
		g_free(classinfo->bitmap.extended);
		classinfo->bitmap.extended = 0;
	}
	g_free(element);
}

#if (DEBUG_LOAD_EVENTS)
static void
print_load_event (const char *event_name, GHashTable *table, gpointer item, LoadedElement *element) {
	const char* item_name;
	char* item_info;
	
	if (table == profiler->loaded_assemblies) {
		//item_info = g_strdup_printf("ASSEMBLY %p (dynamic %d)", item, mono_image_is_dynamic (mono_assembly_get_image((MonoAssembly*)item)));
		item_info = g_strdup_printf("ASSEMBLY %p", item);
	} else if (table == profiler->loaded_modules) {
		//item_info = g_strdup_printf("MODULE %p (dynamic %d)", item, mono_image_is_dynamic ((MonoImage*)item));
		item_info = g_strdup_printf("MODULE %p", item);
	} else if (table == profiler->loaded_appdomains) {
		item_info = g_strdup_printf("APPDOMAIN %p (id %d)", item, mono_domain_get_id ((MonoDomain*)item));
	} else {
		item_info = NULL;
		g_assert_not_reached ();
	}
	
	if (element != NULL) {
		item_name = element->name;
	} else {
		item_name = "<NULL>";
	}
	
	printf ("%s EVENT for %s (%s [id %d])\n", event_name, item_info, item_name, element->id);
	g_free (item_info);
}
#endif

static void
profiler_heap_shot_object_buffers_destroy (ProfilerHeapShotObjectBuffer *buffer) {
	while (buffer != NULL) {
		ProfilerHeapShotObjectBuffer *next = buffer->next;
		g_free (buffer);
		buffer = next;
	}
}

static ProfilerHeapShotObjectBuffer*
profiler_heap_shot_object_buffer_new (ProfilerPerThreadData *data) {

	//初始化对象记录缓存块，将此块挂接在逐线程数据区对象缓存链表头。
	ProfilerHeapShotObjectBuffer *buffer = NULL;
	ProfilerHeapShotObjectBuffer *result = g_new (ProfilerHeapShotObjectBuffer, 1);
	result->next_free_slot = & (result->buffer [0]);
	result->end = & (result->buffer [PROFILER_HEAP_SHOT_OBJECT_BUFFER_SIZE]);
	result->first_unprocessed_slot = & (result->buffer [0]);
	result->next = data->heap_shot_object_buffers;
	data->heap_shot_object_buffers = result; 
	 
	for (buffer = result; buffer != NULL; buffer = buffer->next) {
		ProfilerHeapShotObjectBuffer *last = buffer->next;
		if ((last != NULL) && (last->first_unprocessed_slot == last->end)) {
			buffer->next = NULL;
			profiler_heap_shot_object_buffers_destroy (last);
		}
	}
	
	return result;
}

static ProfilerHeapShotWriteJob*
profiler_heap_shot_write_job_new (gboolean heap_shot_was_requested, gboolean dump_heap_data, guint32 collection) {
	ProfilerHeapShotWriteJob *job = g_new (ProfilerHeapShotWriteJob, 1);
	job->next = NULL;
	job->next_unwritten = NULL;
	
	if (profiler->action_flags.unreachable_objects || dump_heap_data) {
		job->buffers = g_new (ProfilerHeapShotWriteBuffer, 1);
		job->buffers->next = NULL;
		job->last_next = & (job->buffers->next);
		job->start = & (job->buffers->buffer [0]);
		job->cursor = job->start;
		job->end = & (job->buffers->buffer [PROFILER_HEAP_SHOT_WRITE_BUFFER_SIZE]);
	} else {
		job->buffers = NULL;
		job->last_next = NULL;
		job->start = NULL;
		job->cursor = NULL;
		job->end = NULL;
	}
	job->full_buffers = 0;
	
	if (profiler->action_flags.collection_summary) {
		//获得记录类总量
		job->summary.capacity = profiler->classes->next_id;
		job->summary.per_class_data = g_new0 (ProfilerHeapShotClassSummary, job->summary.capacity);
	} else {
		job->summary.capacity = 0;
		job->summary.per_class_data = NULL;
	}

	job->heap_shot_was_requested = heap_shot_was_requested;
	job->collection = collection;
	job->dump_heap_data = dump_heap_data; 
	return job;
}

static gboolean
profiler_heap_shot_write_job_has_data (ProfilerHeapShotWriteJob *job) {
	return ((job->buffers != NULL) || (job->summary.capacity > 0));
}

static void
profiler_heap_shot_write_job_add_buffer (ProfilerHeapShotWriteJob *job, gpointer value) {
	ProfilerHeapShotWriteBuffer *buffer = g_new (ProfilerHeapShotWriteBuffer, 1);
	buffer->next = NULL;
	*(job->last_next) = buffer;
	job->last_next = & (buffer->next);
	job->full_buffers ++;
	buffer->buffer [0] = value;
	job->start = & (buffer->buffer [0]);
	job->cursor = & (buffer->buffer [1]);
	job->end = & (buffer->buffer [PROFILER_HEAP_SHOT_WRITE_BUFFER_SIZE]);
#if DEBUG_HEAP_PROFILER
	printf ("profiler_heap_shot_write_job_add_buffer: in job %p, added buffer %p(%p-%p) with value %p at address %p (cursor now %p)\n", job, buffer, job->start, job->end, value, &(buffer->buffer [0]), job->cursor);
	do {
		ProfilerHeapShotWriteBuffer *current_buffer;
		for (current_buffer = job->buffers; current_buffer != NULL; current_buffer = current_buffer->next) {
			printf ("profiler_heap_shot_write_job_add_buffer: now job %p has buffer %p\n", job, current_buffer);
		}
	} while (0);
#endif
}

static void
profiler_heap_shot_write_job_free_buffers (ProfilerHeapShotWriteJob *job) {
	ProfilerHeapShotWriteBuffer *buffer = job->buffers;
	
	while (buffer != NULL) {
		ProfilerHeapShotWriteBuffer *next = buffer->next;
#if DEBUG_HEAP_PROFILER
		printf ("profiler_heap_shot_write_job_free_buffers: in job %p, freeing buffer %p\n", job, buffer);
#endif
		g_free (buffer);
		buffer = next;
	}
	
	job->buffers = NULL;
	
	if (job->summary.per_class_data != NULL) {
		g_free (job->summary.per_class_data);
		job->summary.per_class_data = NULL;
	}
	job->summary.capacity = 0;
}

static void
profiler_heap_shot_write_block (ProfilerHeapShotWriteJob *job);

static void
profiler_process_heap_shot_write_jobs (void) {
	gboolean done = FALSE;
	
	while (!done) {
		ProfilerHeapShotWriteJob *current_job = profiler->heap_shot_write_jobs;
		ProfilerHeapShotWriteJob *previous_job = NULL;
		ProfilerHeapShotWriteJob *next_job;
		
		done = TRUE;
		while (current_job != NULL) 
		{
			next_job = current_job->next_unwritten;
			
			if (next_job != NULL) {
				if (profiler_heap_shot_write_job_has_data (current_job)) {
					done = FALSE;
				}
				if (! profiler_heap_shot_write_job_has_data (next_job)) {
					current_job->next_unwritten = NULL;
					next_job = NULL;
				}
			} else {
				if (profiler_heap_shot_write_job_has_data (current_job)) 
				{ 
					profiler_heap_shot_write_block (current_job); 
					if (previous_job != NULL) 
					{
						previous_job->next_unwritten = NULL;
					}
				}
			} 
			previous_job = current_job;
			current_job = next_job;
		}
	}
}

static void
profiler_free_heap_shot_write_jobs (void) {
	ProfilerHeapShotWriteJob *current_job = profiler->heap_shot_write_jobs;
	ProfilerHeapShotWriteJob *next_job;
	
	if (current_job != NULL) {
		while (current_job->next_unwritten != NULL) {
			current_job = current_job->next_unwritten;
		}
		
		next_job = current_job->next;
		current_job->next = NULL;
		current_job = next_job;
		
		while (current_job != NULL) { 
			next_job = current_job->next;
			profiler_heap_shot_write_job_free_buffers (current_job);
			g_free (current_job);
			current_job = next_job;
		}
	}
}

static void
profiler_destroy_heap_shot_write_jobs (void) {
	ProfilerHeapShotWriteJob *current_job = profiler->heap_shot_write_jobs;
	ProfilerHeapShotWriteJob *next_job;
	
	while (current_job != NULL) {
		next_job = current_job->next;
		profiler_heap_shot_write_job_free_buffers (current_job);
		g_free (current_job);
		current_job = next_job;
	}
}

static void
profiler_add_heap_shot_write_job (ProfilerHeapShotWriteJob *job) 
{
	job->next = profiler->heap_shot_write_jobs;
	job->next_unwritten = job->next;
	profiler->heap_shot_write_jobs = job;
}

#if DEBUG_HEAP_PROFILER
#define STORE_ALLOCATED_OBJECT_MESSAGE1(d,o) printf ("STORE_ALLOCATED_OBJECT[TID %ld]: storing object %p at address %p\n", (d)->thread_id, (o), (d)->heap_shot_object_buffers->next_free_slot)
#define STORE_ALLOCATED_OBJECT_MESSAGE2(d,o) printf ("STORE_ALLOCATED_OBJECT[TID %ld]: storing object %p at address %p in new buffer %p\n", (d)->thread_id, (o), buffer->next_free_slot, buffer)
#else
#define STORE_ALLOCATED_OBJECT_MESSAGE1(d,o)
#define STORE_ALLOCATED_OBJECT_MESSAGE2(d,o)
#endif

//将分配的对象指针保存进heap_shot_object_buffers中。
#define STORE_ALLOCATED_OBJECT(d,o) do {\
	if ((d)->heap_shot_object_buffers->next_free_slot < (d)->heap_shot_object_buffers->end) {\
		*((d)->heap_shot_object_buffers->next_free_slot) = (o);\
		(d)->heap_shot_object_buffers->next_free_slot ++;\
	} else {\
		ProfilerHeapShotObjectBuffer *buffer = profiler_heap_shot_object_buffer_new (d);\
		*((buffer)->next_free_slot) = (o);\
		(buffer)->next_free_slot ++;\
	}\
} while (0)

//分配逐线程数据块
static ProfilerPerThreadData*
profiler_per_thread_data_new (guint32 buffer_size)
{
	ProfilerPerThreadData *data = g_new (ProfilerPerThreadData, 1);

	data->events = g_new0 (ProfilerEventData, buffer_size);
	data->next_free_event = data->events;
	data->next_unreserved_event = data->events;
	data->end_event = data->events + (buffer_size - 1);
	data->first_unwritten_event = data->events;
	data->first_unmapped_event = data->events;
	MONO_PROFILER_GET_CURRENT_COUNTER (data->start_event_counter);
	data->last_event_counter = data->start_event_counter;
	data->thread_id = CURRENT_THREAD_ID ();


	data->heap_shot_object_buffers = NULL;
	if ((profiler->action_flags.unreachable_objects == TRUE) ||
			(profiler->action_flags.heap_shot == TRUE) ||
			(profiler->action_flags.collection_summary == TRUE)) {
		profiler_heap_shot_object_buffer_new (data);
	}

	if (profiler->action_flags.track_stack) {
		thread_stack_initialize (&(data->stack), 64);
	} else {
		thread_stack_initialize_empty (&(data->stack));
	}
	return data;
}

static void
profiler_per_thread_data_destroy (ProfilerPerThreadData *data) {
	g_free (data->events);
	profiler_heap_shot_object_buffers_destroy (data->heap_shot_object_buffers);
	thread_stack_free (&(data->stack));
	g_free (data);
}

static ProfilerStatisticalData*
profiler_statistical_data_new (MonoProfiler *profiler) {
	int buffer_size = profiler->statistical_buffer_size * (profiler->statistical_call_chain_depth + 1);
	ProfilerStatisticalData *data = g_new (ProfilerStatisticalData, 1);

	data->hits = g_new0 (ProfilerStatisticalHit, buffer_size);
	data->next_free_index = 0;
	data->end_index = profiler->statistical_buffer_size;
	data->first_unwritten_index = 0;
	
	return data;
}

static void
profiler_statistical_data_destroy (ProfilerStatisticalData *data) {
	g_free (data->hits);
	g_free (data);
}

static ProfilerCodeBufferArray*
profiler_code_buffer_array_new (ProfilerCodeBufferArray *child) {
	ProfilerCodeBufferArray *result = g_new0 (ProfilerCodeBufferArray, 1);
	if (child == NULL) {
		result->level = 0;
	} else {
		result->level = child->level + 1;
		result->number_of_buffers = 1;
		result->buffers [0].info.data.sub_buffers = child;
		result->buffers [0].start = child->buffers [0].start;
		result->buffers [0].end = child->buffers [child->number_of_buffers - 1].end;
	}
	return result;
}

static void
profiler_code_buffer_array_destroy (ProfilerCodeBufferArray *buffers) {
	if (buffers->level > 0) {
		int i;
		for (i = 0; i < buffers->number_of_buffers; i++) {
			ProfilerCodeBufferArray *sub_buffers = buffers->buffers [i].info.data.sub_buffers;
			profiler_code_buffer_array_destroy (sub_buffers);
		}
	}
	g_free (buffers);
}

static gboolean
profiler_code_buffer_array_is_full (ProfilerCodeBufferArray *buffers) {
	while (buffers->level > 0) {
		ProfilerCodeBufferArray *next;
		if (buffers->number_of_buffers < PROFILER_CODE_BUFFER_ARRAY_SIZE) {
			return FALSE;
		}
		next = buffers->buffers [PROFILER_CODE_BUFFER_ARRAY_SIZE - 1].info.data.sub_buffers;
		if (next->level < (buffers->level - 1)) {
			return FALSE;
		}
		buffers = next;
	}
	return (buffers->number_of_buffers == PROFILER_CODE_BUFFER_ARRAY_SIZE);
}

static ProfilerCodeBufferArray*
profiler_code_buffer_add (ProfilerCodeBufferArray *buffers, gpointer *buffer, int size, MonoProfilerCodeBufferType type, void *data) {
	if (buffers == NULL) {
		buffers = profiler_code_buffer_array_new (NULL);
	}
	
	if (profiler_code_buffer_array_is_full (buffers)) {
		ProfilerCodeBufferArray *new_slot = profiler_code_buffer_add (NULL, buffer, size, type, data);
		buffers = profiler_code_buffer_array_new (buffers);
		buffers->buffers [buffers->number_of_buffers].info.data.sub_buffers = new_slot;
		buffers->buffers [buffers->number_of_buffers].start = new_slot->buffers [0].start;
		buffers->buffers [buffers->number_of_buffers].end = new_slot->buffers [new_slot->number_of_buffers - 1].end;
		buffers->number_of_buffers ++;
	} else if (buffers->level > 0) {
		ProfilerCodeBufferArray *new_slot = profiler_code_buffer_add (buffers->buffers [buffers->number_of_buffers - 1].info.data.sub_buffers, buffer, size, type, data);
		buffers->buffers [buffers->number_of_buffers - 1].info.data.sub_buffers = new_slot;
		buffers->buffers [buffers->number_of_buffers - 1].start = new_slot->buffers [0].start;
		buffers->buffers [buffers->number_of_buffers - 1].end = new_slot->buffers [new_slot->number_of_buffers - 1].end;
	} else {
		buffers->buffers [buffers->number_of_buffers].start = buffer;
		buffers->buffers [buffers->number_of_buffers].end = (((guint8*) buffer) + size);
		buffers->buffers [buffers->number_of_buffers].info.type = type;
		switch (type) {
		case MONO_PROFILER_CODE_BUFFER_UNKNOWN:
			buffers->buffers [buffers->number_of_buffers].info.data.data = NULL;
			break;
		case MONO_PROFILER_CODE_BUFFER_METHOD:
			buffers->buffers [buffers->number_of_buffers].info.data.method = data;
			break;
		default:
			buffers->buffers [buffers->number_of_buffers].info.type = MONO_PROFILER_CODE_BUFFER_UNKNOWN;
			buffers->buffers [buffers->number_of_buffers].info.data.data = NULL;
		}
		buffers->number_of_buffers ++;
	}
	return buffers;
}

static ProfilerCodeBuffer*
profiler_code_buffer_find (ProfilerCodeBufferArray *buffers, gpointer *address) {
	if (buffers != NULL) {
		ProfilerCodeBuffer *result = NULL;
		do {
			int low = 0;
			int high = buffers->number_of_buffers - 1;
			
			while (high != low) {
				int middle = low + ((high - low) >> 1);
				
				if ((guint8*) address < (guint8*) buffers->buffers [low].start) {
					return NULL;
				}
				if ((guint8*) address >= (guint8*) buffers->buffers [high].end) {
					return NULL;
				}
				
				if ((guint8*) address < (guint8*) buffers->buffers [middle].start) {
					high = middle - 1;
					if (high < low) {
						high = low;
					}
				} else if ((guint8*) address >= (guint8*) buffers->buffers [middle].end) {
					low = middle + 1;
					if (low > high) {
						low = high;
					}
				} else {
					high = middle;
					low = middle;
				}
			}
			
			if (((guint8*) address >= (guint8*) buffers->buffers [low].start) && ((guint8*) address < (guint8*) buffers->buffers [low].end)) {
				if (buffers->level == 0) {
					result = & (buffers->buffers [low]);
				} else {
					buffers = buffers->buffers [low].info.data.sub_buffers;
				}
			} else {
				return NULL;
			}
		} while (result == NULL);
		return result;
	} else {
		return NULL;
	}
}

static void
profiler_code_chunk_initialize (ProfilerCodeChunk *chunk, gpointer memory, gsize size) {
	chunk->buffers = profiler_code_buffer_array_new (NULL);
	chunk->destroyed = FALSE;
	chunk->start = memory;
	chunk->end = ((guint8*)memory) + size;
}

static void
profiler_code_chunk_cleanup (ProfilerCodeChunk *chunk) {
	if (chunk->buffers != NULL) {
		profiler_code_buffer_array_destroy (chunk->buffers);
		chunk->buffers = NULL;
	}
	chunk->start = NULL;
	chunk->end = NULL;
}

static void
profiler_code_chunks_initialize (ProfilerCodeChunks *chunks) {
	chunks->capacity = 32;
	chunks->chunks = g_new0 (ProfilerCodeChunk, 32);
	chunks->number_of_chunks = 0;
}

static void
profiler_code_chunks_cleanup (ProfilerCodeChunks *chunks) {
	int i;
	for (i = 0; i < chunks->number_of_chunks; i++) {
		profiler_code_chunk_cleanup (& (chunks->chunks [i]));
	}
	chunks->capacity = 0;
	chunks->number_of_chunks = 0;
	g_free (chunks->chunks);
	chunks->chunks = NULL;
}

static int
compare_code_chunks (const void* c1, const void* c2) {
	ProfilerCodeChunk *chunk1 = (ProfilerCodeChunk*) c1;
	ProfilerCodeChunk *chunk2 = (ProfilerCodeChunk*) c2;
	return ((guint8*) chunk1->end < (guint8*) chunk2->start) ? -1 : (((guint8*) chunk1->start >= (guint8*) chunk2->end) ? 1 : 0);
}

static int
compare_address_and_code_chunk (const void* a, const void* c) {
	gpointer address = (gpointer) a;
	ProfilerCodeChunk *chunk = (ProfilerCodeChunk*) c;
	return ((guint8*) address < (guint8*) chunk->start) ? -1 : (((guint8*) address >= (guint8*) chunk->end) ? 1 : 0);
}

static void
profiler_code_chunks_sort (ProfilerCodeChunks *chunks) {
	qsort (chunks->chunks, chunks->number_of_chunks, sizeof (ProfilerCodeChunk), compare_code_chunks);
}

static ProfilerCodeChunk*
profiler_code_chunk_find (ProfilerCodeChunks *chunks, gpointer address) {
	return bsearch (address, chunks->chunks, chunks->number_of_chunks, sizeof (ProfilerCodeChunk), compare_address_and_code_chunk);
}

static ProfilerCodeChunk*
profiler_code_chunk_new (ProfilerCodeChunks *chunks, gpointer memory, gsize size) {
	ProfilerCodeChunk *result;
	
	if (chunks->number_of_chunks == chunks->capacity) {
		ProfilerCodeChunk *new_chunks = g_new0 (ProfilerCodeChunk, chunks->capacity * 2);
		memcpy (new_chunks, chunks->chunks, chunks->capacity * sizeof (ProfilerCodeChunk));
		chunks->capacity *= 2;
		g_free (chunks->chunks);
		chunks->chunks = new_chunks;
	}
	
	result = & (chunks->chunks [chunks->number_of_chunks]);
	chunks->number_of_chunks ++;
	profiler_code_chunk_initialize (result, memory, size);
	profiler_code_chunks_sort (chunks);
	return result;
}

static int
profiler_code_chunk_to_index (ProfilerCodeChunks *chunks, ProfilerCodeChunk *chunk) {
	return (int) (chunk - chunks->chunks);
}

static void
profiler_code_chunk_remove (ProfilerCodeChunks *chunks, ProfilerCodeChunk *chunk) {
	int index = profiler_code_chunk_to_index (chunks, chunk);
	
	profiler_code_chunk_cleanup (chunk);
	if ((index >= 0) && (index < chunks->number_of_chunks)) {
		memmove (chunk, chunk + 1, (chunks->number_of_chunks - index) * sizeof (ProfilerCodeChunk));
	}
}

/* This assumes the profiler lock is held */
static ProfilerCodeBuffer*
profiler_code_buffer_from_address (MonoProfiler *prof, gpointer address) {
	ProfilerCodeChunks *chunks = & (prof->code_chunks);
	
	ProfilerCodeChunk *chunk = profiler_code_chunk_find (chunks, address);
	if (chunk != NULL) {
		return profiler_code_buffer_find (chunk->buffers, address);
	} else {
		return NULL;
	}
}

static void
profiler_code_chunk_new_callback (MonoProfiler *prof, gpointer address, int size) {
	ProfilerCodeChunks *chunks = & (prof->code_chunks);
	
	if (prof->code_chunks.chunks != NULL) {
		LOCK_PROFILER ();
		profiler_code_chunk_new (chunks, address, size);
		UNLOCK_PROFILER ();
	}
}

static void
profiler_code_chunk_destroy_callback  (MonoProfiler *prof, gpointer address) {
	ProfilerCodeChunks *chunks = & (prof->code_chunks);
	ProfilerCodeChunk *chunk;
	
	if (prof->code_chunks.chunks != NULL) {
		LOCK_PROFILER ();
		chunk = profiler_code_chunk_find (chunks, address);
		if (chunk != NULL) {
			profiler_code_chunk_remove (chunks, chunk);
		}
		UNLOCK_PROFILER ();
	}
}

static void
profiler_code_buffer_new_callback  (MonoProfiler *prof, gpointer address, int size, MonoProfilerCodeBufferType type, void *data) {
	ProfilerCodeChunks *chunks = & (prof->code_chunks);
	ProfilerCodeChunk *chunk;
	
	if (prof->code_chunks.chunks != NULL) {
		LOCK_PROFILER ();
		chunk = profiler_code_chunk_find (chunks, address);
		if (chunk != NULL) {
			chunk->buffers = profiler_code_buffer_add (chunk->buffers, address, size, type, data);
		}
		UNLOCK_PROFILER ();
	}
}

static void
profiler_add_write_buffer (void) {
	if (profiler->current_write_buffer->next == NULL) {
		profiler->current_write_buffer->next = g_malloc (sizeof (ProfilerFileWriteBuffer) + PROFILER_FILE_WRITE_BUFFER_SIZE);
		profiler->current_write_buffer->next->next = NULL;
		
		//printf ("Added next buffer %p, to buffer %p\n", profiler->current_write_buffer->next, profiler->current_write_buffer);
		
	}
	profiler->current_write_buffer = profiler->current_write_buffer->next;
	profiler->current_write_position = 0;
	profiler->full_write_buffers ++;
}

static void
profiler_free_write_buffers (void) {
	ProfilerFileWriteBuffer *current_buffer = profiler->write_buffers;
	while (current_buffer != NULL) {
		ProfilerFileWriteBuffer *next_buffer = current_buffer->next;
		
		//printf ("Freeing write buffer %p, next is %p\n", current_buffer, next_buffer);
		
		g_free (current_buffer);
		current_buffer = next_buffer;
	}
}

//往Profiler结构的writebuffer里写一个字节，若不够会王buffer中分配缓存块，
//写缓冲区无上限。
#define WRITE_BYTE(b) do {\
	if (profiler->current_write_position >= PROFILER_FILE_WRITE_BUFFER_SIZE) {\
		profiler_add_write_buffer ();\
	}\
	profiler->current_write_buffer->buffer [profiler->current_write_position] = (b);\
	profiler->current_write_position ++;\
} while (0)

#if (DEBUG_FILE_WRITES)
static int bytes_written = 0;
#endif



/*
==============================================	
		写日志缓冲区工具函数
==============================================
*/

//将当前所有Profiler日志缓存块写入文件，并重置所有已写入缓存块。
static void
write_current_block (guint16 code) {
	guint32 size = (profiler->full_write_buffers * PROFILER_FILE_WRITE_BUFFER_SIZE) + profiler->current_write_position;
	ProfilerFileWriteBuffer *current_buffer = profiler->write_buffers;
	guint64 current_counter;
	guint32 counter_delta;
	guint8 header [10];
	
	//获得当前时间戳
	MONO_PROFILER_GET_CURRENT_COUNTER (current_counter);

	if (profiler->last_header_counter != 0) {
		counter_delta = current_counter - profiler->last_header_counter;
	} else {
		counter_delta = 0;
	}
	profiler->last_header_counter = current_counter;
	
	header [0] = code & 0xff;
	header [1] = (code >> 8) & 0xff;
	header [2] = size & 0xff;
	header [3] = (size >> 8) & 0xff;
	header [4] = (size >> 16) & 0xff;
	header [5] = (size >> 24) & 0xff;
	header [6] = counter_delta & 0xff;
	header [7] = (counter_delta >> 8) & 0xff;
	header [8] = (counter_delta >> 16) & 0xff;
	header [9] = (counter_delta >> 24) & 0xff;
	
	OPEN_FILE(); 
	WRITE_BUFFER (& (header [0]), 10); 
	while ((current_buffer != NULL) && (profiler->full_write_buffers > 0)) {

		WRITE_BUFFER (& (current_buffer->buffer [0]), PROFILER_FILE_WRITE_BUFFER_SIZE);
		profiler->full_write_buffers --;
		current_buffer = current_buffer->next;
	}
	if (profiler->current_write_position > 0) {

		WRITE_BUFFER (& (current_buffer->buffer [0]), profiler->current_write_position);
	}
	FLUSH_FILE (); 
	//关闭文件以便在截取快照过程中便于用户打开查看
	CLOSE_FILE();
	profiler->current_write_buffer = profiler->write_buffers;
	profiler->current_write_position = 0;
	profiler->full_write_buffers = 0;
}



#define SEVEN_BITS_MASK (0x7f)
#define EIGHT_BIT_MASK (0x80)

static void
write_uint32 (guint32 value) {
	//while (value > SEVEN_BITS_MASK) {
	//	WRITE_BYTE (value & SEVEN_BITS_MASK);
	//	value >>= 7;
	//}
	//WRITE_BYTE (value | EIGHT_BIT_MASK);
	unsigned char* pbyte = (unsigned char*)(&value);

	int i =0;
	for( ; i < 4 ; i++ )
	{
		WRITE_BYTE(*pbyte);
		++pbyte;
	}
}
static void
write_uint64 (guint64 value) {
	//while (value > SEVEN_BITS_MASK) {
	//	WRITE_BYTE (value & SEVEN_BITS_MASK);
	//	value >>= 7;
	//}
	//WRITE_BYTE (value | EIGHT_BIT_MASK);

	unsigned char* pbyte = (unsigned char*)(&value);
	int i = 0 ;
	for( ; i < 8 ; i++ )
	{
		WRITE_BYTE(*pbyte);
		++pbyte;
	}
}
static void
write_string (const char *string) {
	while (*string != 0) {
		WRITE_BYTE (*string);
		string ++;
	}
	WRITE_BYTE (0);
}

static void write_clock_data (void);
static void
write_directives_block (gboolean start) {
	write_clock_data ();
	
	if (start) {
		if (profiler->action_flags.save_allocation_caller) {
			write_uint32 (MONO_PROFILER_DIRECTIVE_ALLOCATIONS_CARRY_CALLER);
		}
		if (profiler->action_flags.save_allocation_stack || profiler->action_flags.track_calls) {
			write_uint32 (MONO_PROFILER_DIRECTIVE_ALLOCATIONS_HAVE_STACK);
		}
		if (profiler->action_flags.allocations_carry_id) {
			write_uint32 (MONO_PROFILER_DIRECTIVE_ALLOCATIONS_CARRY_ID);
		}
		write_uint32 (MONO_PROFILER_DIRECTIVE_LOADED_ELEMENTS_CARRY_ID);
		write_uint32 (MONO_PROFILER_DIRECTIVE_CLASSES_CARRY_ASSEMBLY_ID);
		write_uint32 (MONO_PROFILER_DIRECTIVE_METHODS_CARRY_WRAPPER_FLAG);
	}
	write_uint32 (MONO_PROFILER_DIRECTIVE_END);
	
	write_clock_data ();
	write_current_block (MONO_PROFILER_FILE_BLOCK_KIND_DIRECTIVES);
}

#if DEBUG_HEAP_PROFILER
#define WRITE_HEAP_SHOT_JOB_VALUE_MESSAGE(v,c) printf ("WRITE_HEAP_SHOT_JOB_VALUE: writing value %p at cursor %p\n", (v), (c))
#else
#define WRITE_HEAP_SHOT_JOB_VALUE_MESSAGE(v,c)
#endif
#define WRITE_HEAP_SHOT_JOB_VALUE(j,v) do {\
	if ((j)->cursor < (j)->end) {\
		*((j)->cursor) = (v);\
		(j)->cursor ++;\
	} else {\
		profiler_heap_shot_write_job_add_buffer (j, v);\
	}\
} while (0)


#undef GUINT_TO_POINTER
#undef GPOINTER_TO_UINT
#if (SIZEOF_VOID_P == 4)
#define GUINT_TO_POINTER(u) ((void*)(guint32)(u))
#define GPOINTER_TO_UINT(p) ((guint32)(void*)(p))
#elif (SIZEOF_VOID_P == 8)
#define GUINT_TO_POINTER(u) ((void*)(guint64)(u))
#define GPOINTER_TO_UINT(p) ((guint64)(void*)(p))
#else
#error Bad size of void pointer
#endif

#define WRITE_HEAP_SHOT_JOB_VALUE_WITH_CODE(j,v,c) WRITE_HEAP_SHOT_JOB_VALUE (j, GUINT_TO_POINTER (GPOINTER_TO_UINT (v)|(c)))

#if DEBUG_HEAP_PROFILER
#define UPDATE_JOB_BUFFER_CURSOR_MESSAGE() printf ("profiler_heap_shot_write_block[UPDATE_JOB_BUFFER_CURSOR]: in job %p, moving to buffer %p and cursor %p\n", job, buffer, cursor)
#else
#define UPDATE_JOB_BUFFER_CURSOR_MESSAGE()
#endif
#define UPDATE_JOB_BUFFER_CURSOR() do {\
	cursor++;\
	if (cursor >= end) {\
		buffer = buffer->next;\
		if (buffer != NULL) {\
			cursor = & (buffer->buffer [0]);\
			if (buffer->next != NULL) {\
				end = & (buffer->buffer [PROFILER_HEAP_SHOT_WRITE_BUFFER_SIZE]);\
			} else {\
				end = job->cursor;\
			}\
		} else {\
			cursor = NULL;\
		}\
	}\
	UPDATE_JOB_BUFFER_CURSOR_MESSAGE ();\
} while (0)

static void
profiler_heap_shot_write_data_block (ProfilerHeapShotWriteJob *job) {
	ProfilerHeapShotWriteBuffer *buffer;
	gpointer* cursor;
	gpointer* end;
	guint64 start_counter;
	guint64 start_time;
	guint64 end_counter;
	guint64 end_time;
	
	write_uint64 (job->start_counter);
	write_uint64 (job->start_time);
	write_uint64 (job->end_counter);
	write_uint64 (job->end_time);
	write_uint32 (job->collection);
	MONO_PROFILER_GET_CURRENT_COUNTER (start_counter);
	MONO_PROFILER_GET_CURRENT_TIME (start_time);
	write_uint64 (start_counter);
	write_uint64 (start_time);

	buffer = job->buffers;
	cursor = & (buffer->buffer [0]);
	if (buffer->next != NULL) {
		end = & (buffer->buffer [PROFILER_HEAP_SHOT_WRITE_BUFFER_SIZE]);
	} else {
		end = job->cursor;
	}
	if (cursor >= end) {
		cursor = NULL;
	}

	while (cursor != NULL) {
		gpointer value = *cursor;
		HeapProfilerJobValueCode code = GPOINTER_TO_UINT (value); 
		UPDATE_JOB_BUFFER_CURSOR ();
		value = *cursor;

		if (code == HEAP_CODE_FREE_OBJECT_CLASS) {
			guint32 class_id = GPOINTER_TO_UINT (value) ;  
			guint32 size; 
			UPDATE_JOB_BUFFER_CURSOR (); 
			 
			write_uint32(HEAP_CODE_FREE_OBJECT_CLASS);
			write_uint32 (class_id);
			
			size = GPOINTER_TO_UINT (*cursor);
			UPDATE_JOB_BUFFER_CURSOR ();
			write_uint32 (size);
		} else if (code == HEAP_CODE_OBJECT) {
			MonoObject *object = 0;  
			guint32 class_id = 0; 
			guint32 size = 0;
			guint32 references = 0;
			
			object = GUINT_TO_POINTER (GPOINTER_TO_UINT (value) );  
			UPDATE_JOB_BUFFER_CURSOR();
			class_id =GPOINTER_TO_UINT(*cursor);
			UPDATE_JOB_BUFFER_CURSOR();
			size = GPOINTER_TO_UINT(*cursor);
			UPDATE_JOB_BUFFER_CURSOR ();
			references = GPOINTER_TO_UINT (*cursor);
			UPDATE_JOB_BUFFER_CURSOR ();
			  
			write_uint32(HEAP_CODE_OBJECT);
			write_uint32(GPOINTER_TO_UINT(object)); 
			write_uint32 (class_id);
			write_uint32 (size);
			write_uint32 (references);
			while (references > 0) 
			{
				gpointer reference = *cursor;
				write_uint32 (GPOINTER_TO_UINT (reference));
				UPDATE_JOB_BUFFER_CURSOR ();
				references --;
			}
		} else {
			g_assert_not_reached ();
		}
	}//while (cursor != NULL) {
	write_uint32 (0);
	
	MONO_PROFILER_GET_CURRENT_COUNTER (end_counter);
	MONO_PROFILER_GET_CURRENT_TIME (end_time);
	write_uint64 (end_counter);
	write_uint64 (end_time);
	
	write_current_block (MONO_PROFILER_FILE_BLOCK_KIND_HEAP_DATA);
}
static void
profiler_heap_shot_write_summary_block (ProfilerHeapShotWriteJob *job) {
	guint64 start_counter;
	guint64 start_time;
	guint64 end_counter;
	guint64 end_time;
	int id;
	
#if DEBUG_HEAP_PROFILER
	printf ("profiler_heap_shot_write_summary_block: start writing job %p...\n", job);
#endif
	MONO_PROFILER_GET_CURRENT_COUNTER (start_counter);
	MONO_PROFILER_GET_CURRENT_TIME (start_time);
	write_uint64 (start_counter);
	write_uint64 (start_time);
	
	write_uint32 (job->collection);
	
	for (id = 0; id < job->summary.capacity; id ++) {
		if ((job->summary.per_class_data [id].reachable.instances > 0) || (job->summary.per_class_data [id].unreachable.instances > 0)) {
			write_uint32 (id);
			write_uint32 (job->summary.per_class_data [id].reachable.instances);
			write_uint32 (job->summary.per_class_data [id].reachable.bytes);
			write_uint32 (job->summary.per_class_data [id].unreachable.instances);
			write_uint32 (job->summary.per_class_data [id].unreachable.bytes);
		}
	}
	write_uint32 (0);
	
	MONO_PROFILER_GET_CURRENT_COUNTER (end_counter);
	MONO_PROFILER_GET_CURRENT_TIME (end_time);
	write_uint64 (end_counter);
	write_uint64 (end_time);
	
	write_current_block (MONO_PROFILER_FILE_BLOCK_KIND_HEAP_SUMMARY);
#if DEBUG_HEAP_PROFILER
	printf ("profiler_heap_shot_write_summary_block: writing job %p done.\n", job);
#endif
}

static void
profiler_heap_shot_write_block (ProfilerHeapShotWriteJob *job) { 
	
	if (profiler->action_flags.collection_summary == TRUE) {
		profiler_heap_shot_write_summary_block (job);
	}
	
	if ((profiler->action_flags.unreachable_objects == TRUE) || (profiler->action_flags.heap_shot == TRUE)) {
		profiler_heap_shot_write_data_block (job);
	}
	
	profiler_heap_shot_write_job_free_buffers (job); 
}

static void
write_element_load_block (LoadedElement *element, guint8 kind, gsize thread_id, gpointer item) {
	WRITE_BYTE (kind);
	write_uint64 (element->load_start_counter);
	write_uint64 (element->load_end_counter);
	write_uint64 (thread_id);
	write_uint32 (element->id);
	write_string (element->name);
	if (kind & MONO_PROFILER_LOADED_EVENT_ASSEMBLY) {
		MonoImage *image = mono_assembly_get_image ((MonoAssembly*) item);
		MonoAssemblyName aname;
		if (mono_assembly_fill_assembly_name (image, &aname)) {
			write_string (aname.name);
			write_uint32 (aname.major);
			write_uint32 (aname.minor);
			write_uint32 (aname.build);
			write_uint32 (aname.revision);
			write_string (aname.culture && *aname.culture? aname.culture: "neutral");
			write_string (aname.public_key_token [0] ? (char *)aname.public_key_token : "null");
			/* Retargetable flag */
			write_uint32 ((aname.flags & 0x00000100) ? 1 : 0);
		} else {
			write_string ("UNKNOWN");
			write_uint32 (0);
			write_uint32 (0);
			write_uint32 (0);
			write_uint32 (0);
			write_string ("neutral");
			write_string ("null");
			write_uint32 (0);
		}
	}
	write_current_block (MONO_PROFILER_FILE_BLOCK_KIND_LOADED);
	element->load_written = TRUE;
}

static void
write_element_unload_block (LoadedElement *element, guint8 kind, gsize thread_id) {
	WRITE_BYTE (kind);
	write_uint64 (element->unload_start_counter);
	write_uint64 (element->unload_end_counter);
	write_uint64 (thread_id);
	write_uint32 (element->id);
	write_string (element->name);
	write_current_block (MONO_PROFILER_FILE_BLOCK_KIND_UNLOADED);
	element->unload_written = TRUE;
}

static void
write_clock_data (void) {
	guint64 counter;
	guint64 time;
	
	MONO_PROFILER_GET_CURRENT_COUNTER (counter);
	MONO_PROFILER_GET_CURRENT_TIME (time);
	
	write_uint64 (counter);
	write_uint64 (time);
}

static void
write_mapping_block (gsize thread_id) {
	ClassIdMappingElement *current_class;
	MethodIdMappingElement *current_method;
	
	if ((profiler->classes->unwritten == NULL) && (profiler->methods->unwritten == NULL))
		return;
 	
	write_clock_data ();
	write_uint64 (thread_id);
	
	for (current_class = profiler->classes->unwritten; current_class != NULL; current_class = current_class->next_unwritten) { 
		write_uint32 (current_class->id);
		write_uint32 (current_class->data.assembly_id); 
		write_string (current_class->name); 

		g_free (current_class->name);
		current_class->name = NULL;
	}
	write_uint32 (0);
	profiler->classes->unwritten = NULL;
	
	for (current_method = profiler->methods->unwritten; current_method != NULL; current_method = current_method->next_unwritten) 
	{
		MonoMethod *method = current_method->method;
		MonoClass *klass = mono_method_get_class (method);
		ClassIdMappingElement *class_element = class_id_mapping_element_get (klass);
		g_assert (class_element != NULL);
		write_uint32 (current_method->id);
		write_uint32 (class_element->id);
		if (method->wrapper_type != 0) {
			write_uint32 (1);
		} else {
			write_uint32 (0);
		}
		write_string (current_method->name);
 
		g_free (current_method->name);
		current_method->name = NULL;
	}
	write_uint32 (0);
	profiler->methods->unwritten = NULL;
	
	write_clock_data ();
	write_current_block (MONO_PROFILER_FILE_BLOCK_KIND_MAPPING);
 }

typedef enum {
	MONO_PROFILER_PACKED_EVENT_CODE_METHOD_ENTER = 1,
	MONO_PROFILER_PACKED_EVENT_CODE_METHOD_EXIT_IMPLICIT = 2,
	MONO_PROFILER_PACKED_EVENT_CODE_METHOD_EXIT_EXPLICIT = 3,
	MONO_PROFILER_PACKED_EVENT_CODE_CLASS_ALLOCATION = 4,
	MONO_PROFILER_PACKED_EVENT_CODE_METHOD_EVENT = 5,
	MONO_PROFILER_PACKED_EVENT_CODE_CLASS_EVENT = 6,
	MONO_PROFILER_PACKED_EVENT_CODE_OTHER_EVENT = 7
} MonoProfilerPackedEventCode;
#define MONO_PROFILER_PACKED_EVENT_CODE_BITS 3
#define MONO_PROFILER_PACKED_EVENT_DATA_BITS (8-MONO_PROFILER_PACKED_EVENT_CODE_BITS)
#define MONO_PROFILER_PACKED_EVENT_DATA_MASK ((1<<MONO_PROFILER_PACKED_EVENT_DATA_BITS)-1)

#define MONO_PROFILER_EVENT_MAKE_PACKED_CODE(result,data,base) do {\
	result = ((base)|((data & MONO_PROFILER_PACKED_EVENT_DATA_MASK) << MONO_PROFILER_PACKED_EVENT_CODE_BITS));\
	data >>= MONO_PROFILER_PACKED_EVENT_DATA_BITS;\
} while (0)
#define MONO_PROFILER_EVENT_MAKE_FULL_CODE(result,code,kind,base) do {\
	result = ((base)|((((kind)<<4) | (code)) << MONO_PROFILER_PACKED_EVENT_CODE_BITS));\
} while (0)

static void
rewrite_last_written_stack (ProfilerThreadStack *stack) {
	guint8 event_code;
	int i = thread_stack_get_last_written_frame (stack);
	
	MONO_PROFILER_EVENT_MAKE_FULL_CODE (event_code, MONO_PROFILER_EVENT_STACK_SECTION, 0, MONO_PROFILER_PACKED_EVENT_CODE_OTHER_EVENT);
	WRITE_BYTE (event_code);
	write_uint32 (0);
	write_uint32 (i);
	
	while (i > 0) {
		i--;
		write_uint32 (thread_stack_written_frame_at_index (stack, i));
	}
}


static ProfilerEventData*
write_stack_section_event (ProfilerEventData *events, ProfilerPerThreadData *data) {
	int last_saved_frame = events->data.number;
	int saved_frames = events->value;
	guint8 event_code;
	int i;
	
	MONO_PROFILER_EVENT_MAKE_FULL_CODE (event_code, MONO_PROFILER_EVENT_STACK_SECTION, 0, MONO_PROFILER_PACKED_EVENT_CODE_OTHER_EVENT);
	WRITE_BYTE (event_code);
	write_uint32 (last_saved_frame);
	write_uint32 (saved_frames);
	thread_stack_set_last_written_frame (&(data->stack), last_saved_frame + saved_frames);
	events++;
	
	for (i = 0; i < saved_frames; i++) {
		guint8 code = events->code;
		guint32 jit_flag;
		MethodIdMappingElement *method;
		guint32 frame_value;
		
		if (code == MONO_PROFILER_EVENT_METHOD_ALLOCATION_CALLER) {
			jit_flag = 0;
		} else if (code == MONO_PROFILER_EVENT_METHOD_ALLOCATION_JIT_TIME_CALLER) {
			jit_flag = 1;
		} else {
			g_assert_not_reached ();
			jit_flag = 0;
		}
		
		method = method_id_mapping_element_get (events->data.address);
		g_assert (method != NULL);
		frame_value = (method->id << 1) | jit_flag;
		write_uint32 (frame_value);
		thread_stack_write_frame_at_index (&(data->stack), last_saved_frame + saved_frames - (1 + i), frame_value);
		events ++;
	}
	
	return events;
}

static ProfilerEventData*
write_event (ProfilerEventData *event, ProfilerPerThreadData *data) {
	ProfilerEventData *next = event + 1;
	gboolean write_event_value = TRUE;
	guint8 event_code;
	guint64 event_data;
	guint64 event_value;
	gboolean write_event_value_extension_1 = FALSE;
	guint64 event_value_extension_1 = 0;
	gboolean write_event_value_extension_2 = FALSE;
	guint64 event_value_extension_2 = 0;

	event_value = event->value;
	if (event_value == MAX_EVENT_VALUE) {
		event_value = *((guint64*)next);
		next ++;
	}
	
	if (event->data_type == MONO_PROFILER_EVENT_DATA_TYPE_METHOD) {
		MethodIdMappingElement *element = method_id_mapping_element_get (event->data.address);
		g_assert (element != NULL);
		event_data = element->id;
		
		if (event->code == MONO_PROFILER_EVENT_METHOD_CALL) {
			if (event->kind == MONO_PROFILER_EVENT_KIND_START) {
				MONO_PROFILER_EVENT_MAKE_PACKED_CODE (event_code, event_data, MONO_PROFILER_PACKED_EVENT_CODE_METHOD_ENTER);
			} else {
				MONO_PROFILER_EVENT_MAKE_PACKED_CODE (event_code, event_data, MONO_PROFILER_PACKED_EVENT_CODE_METHOD_EXIT_EXPLICIT);
			}
		} else {
			MONO_PROFILER_EVENT_MAKE_FULL_CODE (event_code, event->code, event->kind, MONO_PROFILER_PACKED_EVENT_CODE_METHOD_EVENT); 
		}
	} else if (event->data_type == MONO_PROFILER_EVENT_DATA_TYPE_CLASS) {
		ClassIdMappingElement *element = class_id_mapping_element_get (event->data.address);
		g_assert (element != NULL);
		event_data = element->id;
		
		if (event->code == MONO_PROFILER_EVENT_CLASS_ALLOCATION) {
			if ((! profiler->action_flags.save_allocation_caller) || (! (next->code == MONO_PROFILER_EVENT_METHOD_ALLOCATION_JIT_TIME_CALLER))) {
				MONO_PROFILER_EVENT_MAKE_PACKED_CODE (event_code, event_data, MONO_PROFILER_PACKED_EVENT_CODE_CLASS_ALLOCATION);
			} else {
				MONO_PROFILER_EVENT_MAKE_FULL_CODE (event_code, MONO_PROFILER_EVENT_JIT_TIME_ALLOCATION, event->kind, MONO_PROFILER_PACKED_EVENT_CODE_OTHER_EVENT);
			}
			
			if (profiler->action_flags.save_allocation_caller) {
				MonoMethod *caller_method = next->data.address;
				
				if ((next->code != MONO_PROFILER_EVENT_METHOD_ALLOCATION_CALLER) && (next->code != MONO_PROFILER_EVENT_METHOD_ALLOCATION_JIT_TIME_CALLER)) {
					g_assert_not_reached ();
				}
				
				if (caller_method != NULL) {
					MethodIdMappingElement *caller = method_id_mapping_element_get (caller_method);
					g_assert (caller != NULL);
					event_value_extension_1 = caller->id;
				}

				write_event_value_extension_1 = TRUE;
				next ++;
			}
			
			if (profiler->action_flags.allocations_carry_id) {
				event_value_extension_2  = GPOINTER_TO_UINT (next->data.address);
				
				if (next->code != MONO_PROFILER_EVENT_ALLOCATION_OBJECT_ID) {
					g_assert_not_reached ();
				}
				
				write_event_value_extension_2 = TRUE;
				next ++;
			}
		} else if (event->code == MONO_PROFILER_EVENT_CLASS_MONITOR) {
			g_assert (next->code == MONO_PROFILER_EVENT_OBJECT_MONITOR);
			
			MONO_PROFILER_EVENT_MAKE_FULL_CODE (event_code, event->code, event->kind, MONO_PROFILER_PACKED_EVENT_CODE_CLASS_EVENT);
			event_value_extension_1 = next->value;
			write_event_value_extension_1 = TRUE;
			event_value_extension_2  = GPOINTER_TO_UINT (next->data.address);
			write_event_value_extension_2 = TRUE;
			next ++;
		} else {
			MONO_PROFILER_EVENT_MAKE_FULL_CODE (event_code, event->code, event->kind, MONO_PROFILER_PACKED_EVENT_CODE_CLASS_EVENT);
		}
	} else {
		if (event->code == MONO_PROFILER_EVENT_STACK_SECTION) {
			return write_stack_section_event (event, data);
		} else {
			event_data = event->data.number;
			MONO_PROFILER_EVENT_MAKE_FULL_CODE (event_code, event->code, event->kind, MONO_PROFILER_PACKED_EVENT_CODE_OTHER_EVENT);
		}
	}
	
	/* Skip writing JIT events if the user did not ask for them */
	if ((event->code == MONO_PROFILER_EVENT_METHOD_JIT) && ! profiler->action_flags.jit_time) {
		return next;
	}
	
#if (DEBUG_LOGGING_PROFILER)
	EVENT_MARK ();
	printf ("writing EVENT[%p] data_type:%d, kind:%d, code:%d (%d:%ld:%ld)\n", event,
			event->data_type, event->kind, event->code,
			event_code, event_data, event_value);
#endif
	
	WRITE_BYTE (event_code);
	write_uint64 (event_data);
	if (write_event_value) {
		write_uint64 (event_value);
		if (write_event_value_extension_1) {
			write_uint64 (event_value_extension_1);
		}
		if (write_event_value_extension_2) {
			write_uint64 (event_value_extension_2);
		}
	}
	
	return next;
}

static void
write_thread_data_block (ProfilerPerThreadData *data) {
	ProfilerEventData *start = data->first_unwritten_event;
	ProfilerEventData *end = data->first_unmapped_event;
	
	if (start == end)
		return;
	write_clock_data ();
	write_uint64 (data->thread_id);
	
	write_uint64 (data->start_event_counter);
	
	/* If we are tracking the stack, make sure that stack sections */
	/* can be fully reconstructed even reading only one block */
	if (profiler->action_flags.track_stack) {
		rewrite_last_written_stack (&(data->stack));
	}
	
	while (start < end) {
		start = write_event (start, data);
	}
	WRITE_BYTE (0);
	data->first_unwritten_event = end;
	
	write_clock_data ();
	write_current_block (MONO_PROFILER_FILE_BLOCK_KIND_EVENTS);
}

static ProfilerExecutableMemoryRegionData*
profiler_executable_memory_region_new (gpointer *start, gpointer *end, guint32 file_offset, char *file_name, guint32 id) {
	ProfilerExecutableMemoryRegionData *result = g_new (ProfilerExecutableMemoryRegionData, 1);
	result->start = start;
	result->end = end;
	result->file_offset = file_offset;
	result->file_name = g_strdup (file_name);
	result->id = id;
	result->is_new = TRUE;
	
	result->file = NULL;
	result->file_region_reference = NULL;
	result->symbols_capacity = id;
	result->symbols_count = id;
	result->symbols = NULL;
	
	return result;
}

static void
executable_file_close (ProfilerExecutableMemoryRegionData *region);

static void
profiler_executable_memory_region_destroy (ProfilerExecutableMemoryRegionData *data) {
	if (data->file != NULL) {
		executable_file_close (data);
		data->file = NULL;
	}
	if (data->symbols != NULL) {
		g_free (data->symbols);
		data->symbols = NULL;
	}
	if (data->file_name != NULL) {
		g_free (data->file_name);
		data->file_name = NULL;
	}
	g_free (data);
}

static ProfilerExecutableMemoryRegions*
profiler_executable_memory_regions_new (int next_id, int next_unmanaged_function_id) {
	ProfilerExecutableMemoryRegions *result = g_new (ProfilerExecutableMemoryRegions, 1);
	result->regions = g_new0 (ProfilerExecutableMemoryRegionData*, 32);
	result->regions_capacity = 32;
	result->regions_count = 0;
	result->next_id = next_id;
	result->next_unmanaged_function_id = next_unmanaged_function_id;
	return result;
}

static void
profiler_executable_memory_regions_destroy (ProfilerExecutableMemoryRegions *regions) {
	int i;
	
	for (i = 0; i < regions->regions_count; i++) {
		profiler_executable_memory_region_destroy (regions->regions [i]);
	}
	g_free (regions->regions);
	g_free (regions);
}

static ProfilerExecutableMemoryRegionData*
find_address_region (ProfilerExecutableMemoryRegions *regions, gpointer address) {
	int low_index = 0;
	int high_index = regions->regions_count;
	int middle_index = 0;
	ProfilerExecutableMemoryRegionData *middle_region = regions->regions [0];
	
	if ((regions->regions_count == 0) || (regions->regions [low_index]->start > address) || (regions->regions [high_index - 1]->end < address)) {
		return NULL;
	}
	
	//printf ("find_address_region: Looking for address %p in %d regions (from %p to %p)\n", address, regions->regions_count, regions->regions [low_index]->start, regions->regions [high_index - 1]->end);
	
	while (low_index != high_index) {
		middle_index = low_index + ((high_index - low_index) / 2);
		middle_region = regions->regions [middle_index];
		
		//printf ("find_address_region: Looking for address %p, considering index %d[%p-%p] (%d-%d)\n", address, middle_index, middle_region->start, middle_region->end, low_index, high_index);
		
		if (middle_region->start > address) {
			if (middle_index > 0) {
				high_index = middle_index;
			} else {
				return NULL;
			}
		} else if (middle_region->end < address) {
			if (middle_index < regions->regions_count - 1) {
				low_index = middle_index + 1;
			} else {
				return NULL;
			}
		} else {
			return middle_region;
		}
	}
	
	if ((middle_region == NULL) || (middle_region->start > address) || (middle_region->end < address)) {
		return NULL;
	} else {
		return middle_region;
	}
}

static void
append_region (ProfilerExecutableMemoryRegions *regions, gpointer *start, gpointer *end, guint32 file_offset, char *file_name) {
	if (regions->regions_count >= regions->regions_capacity) {
		ProfilerExecutableMemoryRegionData **new_regions = g_new0 (ProfilerExecutableMemoryRegionData*, regions->regions_capacity * 2);
		memcpy (new_regions, regions->regions, regions->regions_capacity * sizeof (ProfilerExecutableMemoryRegionData*));
		g_free (regions->regions);
		regions->regions = new_regions;
		regions->regions_capacity = regions->regions_capacity * 2;
	}
	regions->regions [regions->regions_count] = profiler_executable_memory_region_new (start, end, file_offset, file_name, regions->next_id);
	regions->regions_count ++;
	regions->next_id ++;
}

static gboolean
regions_are_equivalent (ProfilerExecutableMemoryRegionData *region1, ProfilerExecutableMemoryRegionData *region2) {
	if ((region1->start == region2->start) &&
			(region1->end == region2->end) &&
			(region1->file_offset == region2->file_offset) &&
			! strcmp (region1->file_name, region2->file_name)) {
		return TRUE;
	} else {
		return FALSE;
	}
}

static int
compare_regions (const void *a1, const void *a2) {
	ProfilerExecutableMemoryRegionData *r1 = * (ProfilerExecutableMemoryRegionData**) a1;
	ProfilerExecutableMemoryRegionData *r2 = * (ProfilerExecutableMemoryRegionData**) a2;
	return (r1->start < r2->start)? -1 : ((r1->start > r2->start)? 1 : 0);
}

static void
restore_old_regions (ProfilerExecutableMemoryRegions *old_regions, ProfilerExecutableMemoryRegions *new_regions) {
	int old_i;
	int new_i;
	
	for (new_i = 0; new_i < new_regions->regions_count; new_i++) {
		ProfilerExecutableMemoryRegionData *new_region = new_regions->regions [new_i];
		for (old_i = 0; old_i < old_regions->regions_count; old_i++) {
			ProfilerExecutableMemoryRegionData *old_region = old_regions->regions [old_i];
			if ( regions_are_equivalent (old_region, new_region)) {
				new_regions->regions [new_i] = old_region;
				old_regions->regions [old_i] = new_region;
				
				// FIXME (sanity check)
				g_assert (new_region->is_new && ! old_region->is_new);
			}
		}
	}
}

static void
sort_regions (ProfilerExecutableMemoryRegions *regions) {
	if (regions->regions_count > 1) {
		int i;
		
		qsort (regions->regions, regions->regions_count, sizeof (ProfilerExecutableMemoryRegionData *), compare_regions);
		
		i = 1;
		while (i < regions->regions_count) {
			ProfilerExecutableMemoryRegionData *current_region = regions->regions [i];
			ProfilerExecutableMemoryRegionData *previous_region = regions->regions [i - 1];
			
			if (regions_are_equivalent (previous_region, current_region)) {
				int j;
				
				if (! current_region->is_new) {
					profiler_executable_memory_region_destroy (previous_region);
					regions->regions [i - 1] = current_region;
				} else {
					profiler_executable_memory_region_destroy (current_region);
				}
				
				for (j = i + 1; j < regions->regions_count; j++) {
					regions->regions [j - 1] = regions->regions [j];
				}
				
				regions->regions_count --;
			} else {
				i++;
			}
		}
	}
}

static void
fix_region_references (ProfilerExecutableMemoryRegions *regions) {
	int i;
	for (i = 0; i < regions->regions_count; i++) {
		ProfilerExecutableMemoryRegionData *region = regions->regions [i];
		if (region->file_region_reference != NULL) {
			region->file_region_reference->region = region;
		}
	}
}

static void
executable_file_add_region_reference (ProfilerExecutableFile *file, ProfilerExecutableMemoryRegionData *region) {
	guint8 *section_headers = file->data + file->header->e_shoff;
	int section_index;
	
	for (section_index = 1; section_index < file->header->e_shnum; section_index ++) {
		ElfSection *section_header = (ElfSection*) (section_headers + (file->header->e_shentsize * section_index));
		
		if ((section_header->sh_addr != 0) && (section_header->sh_flags & ELF_SHF_EXECINSTR) &&
				(region->file_offset <= section_header->sh_offset) && (region->file_offset + (((guint8*)region->end)-((guint8*)region->start)) >= (section_header->sh_offset + section_header->sh_size))) {
			ProfilerExecutableFileSectionRegion *section_region = & (file->section_regions [section_index]);
			section_region->region = region;
			section_region->section_address = (gpointer) section_header->sh_addr;
			section_region->section_offset = section_header->sh_offset;
			region->file_region_reference = section_region;
		}
	}
}

static gboolean check_elf_header (ElfHeader* header) {
	guint16 test = 0x0102;
	
	if ((header->e_ident [EI_MAG0] != 0x7f) || (header->e_ident [EI_MAG1] != 'E') ||
			(header->e_ident [EI_MAG2] != 'L') || (header->e_ident [EI_MAG3] != 'F')) {
		return FALSE;
	}

	if (sizeof (gsize) == 4) {
		if (header->e_ident [EI_CLASS] != ELF_CLASS_32) {
			g_warning ("Class is not ELF_CLASS_32 with gsize size %d", (int) sizeof (gsize));
			return FALSE;
		}
	} else if (sizeof (gsize) == 8) {
		if (header->e_ident [EI_CLASS] != ELF_CLASS_64) {
			g_warning ("Class is not ELF_CLASS_64 with gsize size %d", (int) sizeof (gsize));
			return FALSE;
		}
	} else {
		g_warning ("Absurd gsize size %d", (int) sizeof (gsize));
		return FALSE;
	}

	if ((*(guint8*)(&test)) == 0x01) {
		if (header->e_ident [EI_DATA] != ELF_DATA_MSB) {
			g_warning ("Data is not ELF_DATA_MSB with first test byte 0x01");
			return FALSE;
		}
	} else if ((*(guint8*)(&test)) == 0x02) {
		if (header->e_ident [EI_DATA] != ELF_DATA_LSB) {
			g_warning ("Data is not ELF_DATA_LSB with first test byte 0x02");
			return FALSE;
		}
	} else {
		g_warning ("Absurd test byte value");
		return FALSE;
	}
	
	return TRUE;
}

static gboolean check_elf_file (int fd) {
	void *header = malloc (sizeof (ElfHeader));
	ssize_t read_result = read (fd, header, sizeof (ElfHeader));
	gboolean result;
	
	if (read_result != sizeof (ElfHeader)) {
		result = FALSE;
	} else {
		result = check_elf_header ((ElfHeader*) header);
	}
	
	free (header);
	return result;
}

static ProfilerExecutableFile*
executable_file_open (ProfilerExecutableMemoryRegionData *region) {
	ProfilerExecutableFiles *files = & (profiler->executable_files);
	ProfilerExecutableFile *file = region->file;
	
	if (file == NULL) {
		file = (ProfilerExecutableFile*) g_hash_table_lookup (files->table, region->file_name);
		
		if (file == NULL) {
			struct stat stat_buffer;
			int symtab_index = 0;
			int strtab_index = 0;
			int dynsym_index = 0;
			int dynstr_index = 0;
			ElfHeader *header;
			guint8 *section_headers;
			int section_index;
			int strings_index;
			
			file = g_new0 (ProfilerExecutableFile, 1);
			region->file = file;
			g_hash_table_insert (files->table, region->file_name, file);
			file->reference_count ++;
			file->next_new_file = files->new_files;
			files->new_files = file; 
			file->fd = open (region->file_name, O_RDONLY);
			if (file->fd == -1) {
				//g_warning ("Cannot open file '%s': '%s'", region->file_name, strerror (errno));
				return file;
			} else {
				if (fstat (file->fd, &stat_buffer) != 0) {
					//g_warning ("Cannot stat file '%s': '%s'", region->file_name, strerror (errno));
					return file;
				} else if (! check_elf_file (file->fd)) {
					return file;
				} else {
					size_t region_length = ((guint8*)region->end) - ((guint8*)region->start);
					file->length = stat_buffer.st_size;
					
					if (file->length == region_length) {
						file->data = region->start;
						close (file->fd);
						file->fd = -1;
					} else { 
						//file->data = mmap (NULL, file->length, PROT_READ, MAP_PRIVATE, file->fd, 0);
						
						//if (file->data == MAP_FAILED) {
							close (file->fd);
							//g_warning ("Cannot map file '%s': '%s'", region->file_name, strerror (errno));
							file->data = NULL;
							return file;
						//}
					}
				}
			}
			
			/* OK, this is a usable elf file, and we mmapped it... */
			header = (ElfHeader*) file->data;
			file->header = header;
			section_headers = file->data + file->header->e_shoff;
			file->main_string_table = ((const char*) file->data) + (((ElfSection*) (section_headers + (header->e_shentsize * header->e_shstrndx)))->sh_offset);
			
			for (section_index = 0; section_index < header->e_shnum; section_index ++) {
				ElfSection *section_header = (ElfSection*) (section_headers + (header->e_shentsize * section_index));
				
				if (section_header->sh_type == ELF_SHT_SYMTAB) {
					symtab_index = section_index;
				} else if (section_header->sh_type == ELF_SHT_DYNSYM) {
					dynsym_index = section_index;
				} else if (section_header->sh_type == ELF_SHT_STRTAB) {
					if (! strcmp (file->main_string_table + section_header->sh_name, ".strtab")) {
						strtab_index = section_index;
					} else if (! strcmp (file->main_string_table + section_header->sh_name, ".dynstr")) {
						dynstr_index = section_index;
					}
				}
			}
			
			if ((symtab_index != 0) && (strtab_index != 0)) {
				section_index = symtab_index;
				strings_index = strtab_index;
			} else if ((dynsym_index != 0) && (dynstr_index != 0)) {
				section_index = dynsym_index;
				strings_index = dynstr_index;
			} else {
				section_index = 0;
				strings_index = 0;
			}
			
			if (section_index != 0) {
				ElfSection *section_header = (ElfSection*) (section_headers + (header->e_shentsize * section_index));
				file->symbol_size = section_header->sh_entsize;
				file->symbols_count = (guint32) (section_header->sh_size / section_header->sh_entsize);
				file->symbols_start = file->data + section_header->sh_offset;
				file->symbols_string_table = ((const char*) file->data) + (((ElfSection*) (section_headers + (header->e_shentsize * strings_index)))->sh_offset);
			}
			
			file->section_regions = g_new0 (ProfilerExecutableFileSectionRegion, file->header->e_shnum);
		} else {
			region->file = file;
			file->reference_count ++;
		}
	}
	
	if (file->header != NULL) {
		executable_file_add_region_reference (file, region);
	}
	
	return file;
}

static void
executable_file_free (ProfilerExecutableFile* file) {
	if (file->fd != -1) {
		if (close (file->fd) != 0) {
			g_warning ("Cannot close file: '%s'", strerror (errno));
		}
		if (file->data != NULL) {
			/*if (munmap (file->data, file->length) != 0) {
				g_warning ("Cannot unmap file: '%s'", strerror (errno));
			}*/
		}
	}
	if (file->section_regions != NULL) {
		g_free (file->section_regions);
		file->section_regions = NULL;
	}
	g_free (file);
}

static void
executable_file_close (ProfilerExecutableMemoryRegionData *region) {
	region->file->reference_count --;
	
	if ((region->file_region_reference != NULL) && (region->file_region_reference->region == region)) {
		region->file_region_reference->region = NULL;
		region->file_region_reference->section_address = 0;
		region->file_region_reference->section_offset = 0;
	}
	
	if (region->file->reference_count <= 0) {
		ProfilerExecutableFiles *files = & (profiler->executable_files);
		g_hash_table_remove (files->table, region->file_name);
		executable_file_free (region->file);
		region->file = NULL;
	}
}

static void
executable_file_count_symbols (ProfilerExecutableFile *file) {
	int symbol_index;
	
	for (symbol_index = 0; symbol_index < file->symbols_count; symbol_index ++) {
		ElfSymbol *symbol = (ElfSymbol*) (file->symbols_start + (symbol_index * file->symbol_size));
		
		if ((ELF_ST_TYPE (symbol->st_info) == ELF_STT_FUNC) &&
				(symbol->st_shndx > 0) &&
				(symbol->st_shndx < file->header->e_shnum)) {
			int symbol_section_index = symbol->st_shndx;
			ProfilerExecutableMemoryRegionData *region = file->section_regions [symbol_section_index].region;
			if ((region != NULL) && (region->symbols == NULL)) {
				region->symbols_count ++;
			}
		}
	}
}

static void
executable_memory_regions_prepare_symbol_tables (ProfilerExecutableMemoryRegions *regions) {
	int i;
	for (i = 0; i < regions->regions_count; i++) {
		ProfilerExecutableMemoryRegionData *region = regions->regions [i];
		if ((region->symbols_count > 0) && (region->symbols == NULL)) {
			region->symbols = g_new (ProfilerUnmanagedSymbol, region->symbols_count);
			region->symbols_capacity = region->symbols_count;
			region->symbols_count = 0;
		}
	}
}

static const char*
executable_region_symbol_get_name (ProfilerExecutableMemoryRegionData *region, ProfilerUnmanagedSymbol *symbol) {
	ElfSymbol *elf_symbol = (ElfSymbol*) (region->file->symbols_start + (symbol->index * region->file->symbol_size));
	return region->file->symbols_string_table + elf_symbol->st_name;
}

static void
executable_file_build_symbol_tables (ProfilerExecutableFile *file) {
	int symbol_index;
	
	for (symbol_index = 0; symbol_index < file->symbols_count; symbol_index ++) {
		ElfSymbol *symbol = (ElfSymbol*) (file->symbols_start + (symbol_index * file->symbol_size));
		
		if ((ELF_ST_TYPE (symbol->st_info) == ELF_STT_FUNC) &&
				(symbol->st_shndx > 0) &&
				(symbol->st_shndx < file->header->e_shnum)) {
			int symbol_section_index = symbol->st_shndx;
			ProfilerExecutableFileSectionRegion *section_region = & (file->section_regions [symbol_section_index]);
			ProfilerExecutableMemoryRegionData *region = section_region->region;
			
			if (region != NULL) {
				ProfilerUnmanagedSymbol *new_symbol = & (region->symbols [region->symbols_count]);
				region->symbols_count ++;
				
				new_symbol->id = 0;
				new_symbol->index = symbol_index;
				new_symbol->size = symbol->st_size;
				new_symbol->offset = (((guint8*) symbol->st_value) - section_region->section_address) - (region->file_offset - section_region->section_offset);
			}
		}
	}
}

static int
compare_region_symbols (const void *p1, const void *p2) {
	const ProfilerUnmanagedSymbol *s1 = p1;
	const ProfilerUnmanagedSymbol *s2 = p2;
	return (s1->offset < s2->offset)? -1 : ((s1->offset > s2->offset)? 1 : 0);
}

static void
executable_memory_regions_sort_symbol_tables (ProfilerExecutableMemoryRegions *regions) {
	int i;
	for (i = 0; i < regions->regions_count; i++) {
		ProfilerExecutableMemoryRegionData *region = regions->regions [i];
		if ((region->is_new) && (region->symbols != NULL)) {
			qsort (region->symbols, region->symbols_count, sizeof (ProfilerUnmanagedSymbol), compare_region_symbols);
		}
	}
}

static void
build_symbol_tables (ProfilerExecutableMemoryRegions *regions, ProfilerExecutableFiles *files) {
	int i;
	ProfilerExecutableFile *file;
	
	for (i = 0; i < regions->regions_count; i++) {
		ProfilerExecutableMemoryRegionData *region = regions->regions [i];
		if ((region->is_new) && (region->file == NULL)) {
			executable_file_open (region);
		}
	}
	
	for (file = files->new_files; file != NULL; file = file->next_new_file) {
		executable_file_count_symbols (file);
	}
	
	executable_memory_regions_prepare_symbol_tables (regions);
	
	for (file = files->new_files; file != NULL; file = file->next_new_file) {
		executable_file_build_symbol_tables (file);
	}
	
	executable_memory_regions_sort_symbol_tables (regions);
	
	file = files->new_files;
	while (file != NULL) {
		ProfilerExecutableFile *next_file = file->next_new_file;
		file->next_new_file = NULL;
		file = next_file;
	}
	files->new_files = NULL;
}

static ProfilerUnmanagedSymbol*
executable_memory_region_find_symbol (ProfilerExecutableMemoryRegionData *region, guint32 offset) {
	if (region->symbols_count > 0) {
		ProfilerUnmanagedSymbol *low = region->symbols;
		ProfilerUnmanagedSymbol *high = region->symbols + (region->symbols_count - 1);
		int step = region->symbols_count >> 1;
		ProfilerUnmanagedSymbol *current = region->symbols + step;
		
		do {
			step = (high - low) >> 1;
			
			if (offset < current->offset) {
				high = current;
				current = high - step;
			} else if (offset >= current->offset) {
				if (offset >= (current->offset + current->size)) {
					low = current;
					current = low + step;
				} else {
					return current;
				}
			}
		} while (step > 0);
		
		if ((offset >= current->offset) && (offset < (current->offset + current->size))) {
			return current;
		} else {
			return NULL;
		}
	} else {
		return NULL;
	}
}

//FIXME: make also Win32 and BSD variants
#define MAPS_BUFFER_SIZE 4096
#define MAPS_FILENAME_SIZE 2048

static gboolean
update_regions_buffer (int fd, char *buffer) {
	ssize_t result = read (fd, buffer, MAPS_BUFFER_SIZE);
	
	if (result == MAPS_BUFFER_SIZE) {
		return TRUE;
	} else if (result >= 0) {
		*(buffer + result) = 0;
		return FALSE;
	} else {
		*buffer = 0;
		return FALSE;
	}
}

#define GOTO_NEXT_CHAR(c,b,fd) do {\
	(c)++;\
	if (((c) - (b) >= MAPS_BUFFER_SIZE) || ((*(c) == 0) && ((c) != (b)))) {\
		update_regions_buffer ((fd), (b));\
		(c) = (b);\
	}\
} while (0);

static int hex_digit_value (char c) {
	if ((c >= '0') && (c <= '9')) {
		return c - '0';
	} else if ((c >= 'a') && (c <= 'f')) {
		return c - 'a' + 10;
	} else if ((c >= 'A') && (c <= 'F')) {
		return c - 'A' + 10;
	} else {
		return 0;
	}
}

/*
 * Start address
 * -
 * End address
 * (space)
 * Permissions
 * Offset
 * (space)
 * Device
 * (space)
 * Inode
 * (space)
 * File
 * \n
 */
typedef enum {
	MAP_LINE_PARSER_STATE_INVALID,
	MAP_LINE_PARSER_STATE_START_ADDRESS,
	MAP_LINE_PARSER_STATE_END_ADDRESS,
	MAP_LINE_PARSER_STATE_PERMISSIONS,
	MAP_LINE_PARSER_STATE_OFFSET,
	MAP_LINE_PARSER_STATE_DEVICE,
	MAP_LINE_PARSER_STATE_INODE,
	MAP_LINE_PARSER_STATE_BLANK_BEFORE_FILENAME,
	MAP_LINE_PARSER_STATE_FILENAME,
	MAP_LINE_PARSER_STATE_DONE
} MapLineParserState;

const char *map_line_parser_state [] = {
	"INVALID",
	"START_ADDRESS",
	"END_ADDRESS",
	"PERMISSIONS",
	"OFFSET",
	"DEVICE",
	"INODE",
	"BLANK_BEFORE_FILENAME",
	"FILENAME",
	"DONE"
};

static char*
parse_map_line (ProfilerExecutableMemoryRegions *regions, int fd, char *buffer, char *filename, char *current) {
	MapLineParserState state = MAP_LINE_PARSER_STATE_START_ADDRESS;
	gsize start_address = 0;
	gsize end_address = 0;
	guint32 offset = 0;
	int filename_index = 0;
	gboolean is_executable = FALSE;
	gboolean done = FALSE;
	
	char c = *current;
	
	while (1) {
		switch (state) {
		case MAP_LINE_PARSER_STATE_START_ADDRESS:
			if (isxdigit (c)) {
				start_address <<= 4;
				start_address |= hex_digit_value (c);
			} else if (c == '-') {
				state = MAP_LINE_PARSER_STATE_END_ADDRESS;
			} else {
				state = MAP_LINE_PARSER_STATE_INVALID;
			}
			break;
		case MAP_LINE_PARSER_STATE_END_ADDRESS:
			if (isxdigit (c)) {
				end_address <<= 4;
				end_address |= hex_digit_value (c);
			} else if (isspace (c)) { 
				state = MAP_LINE_PARSER_STATE_PERMISSIONS;
			} else {
				state = MAP_LINE_PARSER_STATE_INVALID;
			}
			break;
		case MAP_LINE_PARSER_STATE_PERMISSIONS:
			if (c == 'x') {
				is_executable = TRUE;
			} else if (isspace (c)) {
				state = MAP_LINE_PARSER_STATE_OFFSET;
			} else if ((c != '-') && ! isalpha (c)) {
				state = MAP_LINE_PARSER_STATE_INVALID;
			}
			break;
		case MAP_LINE_PARSER_STATE_OFFSET:
			if (isxdigit (c)) {
				offset <<= 4;
				offset |= hex_digit_value (c);
			} else if (isspace (c)) {
				state = MAP_LINE_PARSER_STATE_DEVICE;
			} else {
				state = MAP_LINE_PARSER_STATE_INVALID;
			}
			break;
		case MAP_LINE_PARSER_STATE_DEVICE:
			if (isspace (c)) {
				state = MAP_LINE_PARSER_STATE_INODE;
			} else if ((c != ':') && ! isxdigit (c)) {
				state = MAP_LINE_PARSER_STATE_INVALID;
			}
			break;
		case MAP_LINE_PARSER_STATE_INODE:
			if (isspace (c)) {
				state = MAP_LINE_PARSER_STATE_BLANK_BEFORE_FILENAME;
			} else if (! isdigit (c)) {
				state = MAP_LINE_PARSER_STATE_INVALID;
			}
			break;
		case MAP_LINE_PARSER_STATE_BLANK_BEFORE_FILENAME:
			if ((c == '/') || (c == '[')) {
				state = MAP_LINE_PARSER_STATE_FILENAME;
				filename [filename_index] = *current;
				filename_index ++;
			} else if (! isspace (c)) {
				state = MAP_LINE_PARSER_STATE_INVALID;
			}
			break;
		case MAP_LINE_PARSER_STATE_FILENAME:
			if (filename_index < MAPS_FILENAME_SIZE) {
				if (c == '\n') {
					state = MAP_LINE_PARSER_STATE_DONE;
					done = TRUE;
					filename [filename_index] = 0;
				} else {
					filename [filename_index] = *current;
					filename_index ++;
				}
			} else {
				filename [filename_index] = 0;
				g_warning ("ELF filename too long: \"%s\"...\n", filename);
			}
			break;
		case MAP_LINE_PARSER_STATE_DONE:
			if (done && is_executable) {
				filename [filename_index] = 0;
				append_region (regions, (gpointer) start_address, (gpointer) end_address, offset, filename);
			}
			return current;
		case MAP_LINE_PARSER_STATE_INVALID:
			if (c == '\n') {
				state = MAP_LINE_PARSER_STATE_DONE;
			}
			break;
		}
		
		if (c == 0) {
			return NULL;
		} else if (c == '\n') {
			state = MAP_LINE_PARSER_STATE_DONE;
		}
		
		GOTO_NEXT_CHAR(current, buffer, fd);
		c = *current;
	}
}

static gboolean
scan_process_regions (ProfilerExecutableMemoryRegions *regions) {
	char *buffer;
	char *filename;
	char *current;
	int fd;
	
#ifdef PLATFORM_MACOSX
	return FALSE;
#endif

	fd = open ("/proc/self/maps", O_RDONLY);
	if (fd == -1) {
		return FALSE;
	}
	
	buffer = malloc (MAPS_BUFFER_SIZE);
	filename = malloc (MAPS_FILENAME_SIZE);
	update_regions_buffer (fd, buffer);
	current = buffer;
	while (current != NULL) {
		current = parse_map_line (regions, fd, buffer, filename, current);
	}
	
	free (buffer);
	free (filename);
	
	close (fd);
	return TRUE;
}
//End of Linux code

typedef enum {
	MONO_PROFILER_STATISTICAL_CODE_END = 0,
	MONO_PROFILER_STATISTICAL_CODE_METHOD = 1,
	MONO_PROFILER_STATISTICAL_CODE_UNMANAGED_FUNCTION_ID = 2,
	MONO_PROFILER_STATISTICAL_CODE_UNMANAGED_FUNCTION_NEW_ID = 3,
	MONO_PROFILER_STATISTICAL_CODE_UNMANAGED_FUNCTION_OFFSET_IN_REGION = 4,
	MONO_PROFILER_STATISTICAL_CODE_CALL_CHAIN = 5,
	MONO_PROFILER_STATISTICAL_CODE_REGIONS = 7
} MonoProfilerStatisticalCode;

static void
refresh_memory_regions (void) {
	ProfilerExecutableMemoryRegions *old_regions = profiler->executable_regions;
	ProfilerExecutableMemoryRegions *new_regions = profiler_executable_memory_regions_new (old_regions->next_id, old_regions->next_unmanaged_function_id);
	int i;
	
	LOG_WRITER_THREAD ("Refreshing memory regions...");
	scan_process_regions (new_regions);
	sort_regions (new_regions);
	restore_old_regions (old_regions, new_regions);
	fix_region_references (new_regions);
	LOG_WRITER_THREAD ("Refreshed memory regions.");
	
	LOG_WRITER_THREAD ("Building symbol tables...");
	build_symbol_tables (new_regions, & (profiler->executable_files));
#if 0
	printf ("Symbol tables done!\n");
	printf ("Region summary...\n");
	for (i = 0; i < new_regions->regions_count; i++) {
		ProfilerExecutableMemoryRegionData *region = new_regions->regions [i];
		printf ("Region %d[%d][NEW:%d] (%p-%p) at %d in file %s\n", i, region->id, region->is_new,
				region->start, region->end, region->file_offset, region->file_name);
	}
	printf ("New symbol tables dump...\n");
	for (i = 0; i < new_regions->regions_count; i++) {
		ProfilerExecutableMemoryRegionData *region = new_regions->regions [i];
		
		if (region->is_new) {
			int symbol_index;
			
			printf ("Region %d[%d][NEW:%d] (%p-%p) at %d in file %s\n", i, region->id, region->is_new,
					region->start, region->end, region->file_offset, region->file_name);
			for (symbol_index = 0; symbol_index < region->symbols_count; symbol_index ++) {
				ProfilerUnmanagedSymbol *symbol = & (region->symbols [symbol_index]);
				printf ("  [%d] Symbol %s (offset %d, size %d)\n", symbol_index,
						executable_region_symbol_get_name (region, symbol),
						symbol->offset, symbol->size);
			}
		}
	}
#endif
	LOG_WRITER_THREAD ("Built symbol tables.");
	
	// This marks the region "sub-block"
	write_uint32 (MONO_PROFILER_STATISTICAL_CODE_REGIONS);
	
	// First write the "removed" regions 
	for (i = 0; i < old_regions->regions_count; i++) {
		ProfilerExecutableMemoryRegionData *region = old_regions->regions [i];
		if (! region->is_new) {
#if DEBUG_STATISTICAL_PROFILER
			printf ("[refresh_memory_regions] Invalidated region %d\n", region->id);
#endif
			write_uint32 (region->id);
		}
	}
	write_uint32 (0);
	
	// Then write the new ones
	for (i = 0; i < new_regions->regions_count; i++) {
		ProfilerExecutableMemoryRegionData *region = new_regions->regions [i];
		if (region->is_new) {
			region->is_new = FALSE;
			
#if DEBUG_STATISTICAL_PROFILER
			printf ("[refresh_memory_regions] Wrote region %d (%p-%p[%d] '%s')\n", region->id, region->start, region->end, region->file_offset, region->file_name);
#endif
			write_uint32 (region->id);
			write_uint64 (GPOINTER_TO_UINT (region->start));
			write_uint32 (GPOINTER_TO_UINT (region->end) - GPOINTER_TO_UINT (region->start));
			write_uint32 (region->file_offset);
			write_string (region->file_name);
		}
	}
	write_uint32 (0);
	
	// Finally, free the old ones, and replace them
	profiler_executable_memory_regions_destroy (old_regions);
	profiler->executable_regions = new_regions;
}

static gboolean
write_statistical_hit (MonoDomain *domain, gpointer address, gboolean regions_refreshed) {
	ProfilerCodeBuffer *code_buffer = profiler_code_buffer_from_address (profiler, address);
	
	if ((code_buffer != NULL) && (code_buffer->info.type == MONO_PROFILER_CODE_BUFFER_METHOD)) {
		MonoMethod *method = code_buffer->info.data.method;
		MethodIdMappingElement *element = method_id_mapping_element_get (method);
		
		if (element != NULL) {
#if DEBUG_STATISTICAL_PROFILER
			printf ("[write_statistical_hit] Wrote method %d\n", element->id);
#endif
			write_uint32 ((element->id << 3) | MONO_PROFILER_STATISTICAL_CODE_METHOD);
		} else {
#if DEBUG_STATISTICAL_PROFILER
			printf ("[write_statistical_hit] Wrote unknown method %p\n", method);
#endif
			write_uint32 (MONO_PROFILER_STATISTICAL_CODE_METHOD);
		}
	} else {
		ProfilerExecutableMemoryRegionData *region = find_address_region (profiler->executable_regions, address);
		
		if (region == NULL && ! regions_refreshed) {
#if DEBUG_STATISTICAL_PROFILER
			printf ("[write_statistical_hit] Cannot find region for address %p, refreshing...\n", address);
#endif
			refresh_memory_regions ();
			regions_refreshed = TRUE;
			region = find_address_region (profiler->executable_regions, address);
		}
		
		if (region != NULL) {
			guint32 offset = ((guint8*)address) - ((guint8*)region->start);
			ProfilerUnmanagedSymbol *symbol = executable_memory_region_find_symbol (region, offset);
			
			if (symbol != NULL) {
				if (symbol->id > 0) {
#if DEBUG_STATISTICAL_PROFILER
					printf ("[write_statistical_hit] Wrote unmanaged symbol %d\n", symbol->id);
#endif
					write_uint32 ((symbol->id << 3) | MONO_PROFILER_STATISTICAL_CODE_UNMANAGED_FUNCTION_ID);
				} else {
					ProfilerExecutableMemoryRegions *regions = profiler->executable_regions;
					const char *symbol_name = executable_region_symbol_get_name (region, symbol);
					symbol->id = regions->next_unmanaged_function_id;
					regions->next_unmanaged_function_id ++;
#if DEBUG_STATISTICAL_PROFILER
					printf ("[write_statistical_hit] Wrote new unmanaged symbol in region %d[%d]\n", region->id, offset);
#endif
					write_uint32 ((region->id << 3) | MONO_PROFILER_STATISTICAL_CODE_UNMANAGED_FUNCTION_NEW_ID);
					write_uint32 (symbol->id);
					write_string (symbol_name);
				}
			} else {
#if DEBUG_STATISTICAL_PROFILER
				printf ("[write_statistical_hit] Wrote unknown unmanaged hit in region %d[%d] (address %p)\n", region->id, offset, address);
#endif
				write_uint32 ((region->id << 3) | MONO_PROFILER_STATISTICAL_CODE_UNMANAGED_FUNCTION_OFFSET_IN_REGION);
				write_uint32 (offset);
			}
		} else {
#if DEBUG_STATISTICAL_PROFILER
			printf ("[write_statistical_hit] Wrote unknown unmanaged hit %p\n", address);
#endif
			write_uint32 (MONO_PROFILER_STATISTICAL_CODE_UNMANAGED_FUNCTION_OFFSET_IN_REGION);
			write_uint64 (GPOINTER_TO_UINT (address));
		}
	}
	
	return regions_refreshed;
}

static void
flush_all_mappings (void);

static void
write_statistical_data_block (ProfilerStatisticalData *data) {
	MonoThread *current_thread = mono_thread_current ();
	int start_index = data->first_unwritten_index;
	int end_index = data->next_free_index;
	gboolean regions_refreshed = FALSE;
	int call_chain_depth = profiler->statistical_call_chain_depth;
	int index;
	
	if (end_index > data->end_index)
		end_index = data->end_index;
	
	if (start_index == end_index)
		return;
	
	data->first_unwritten_index = end_index;
	
	write_clock_data ();
	
#if DEBUG_STATISTICAL_PROFILER
	printf ("[write_statistical_data_block] Starting loop at index %d\n", start_index);
#endif
	
	for (index = start_index; index < end_index; index ++) {
		int base_index = index * (call_chain_depth + 1);
		ProfilerStatisticalHit hit = data->hits [base_index];
		int callers_count;
		
		regions_refreshed = write_statistical_hit ((current_thread != NULL) ? hit.domain : NULL, hit.address, regions_refreshed);
		base_index ++;
		
		for (callers_count = 0; callers_count < call_chain_depth; callers_count ++) {
			hit = data->hits [base_index + callers_count];
			if (hit.address == NULL) {
				break;
			}
		}
		
		if (callers_count > 0) {
			write_uint32 ((callers_count << 3) | MONO_PROFILER_STATISTICAL_CODE_CALL_CHAIN);
			
			for (callers_count = 0; callers_count < call_chain_depth; callers_count ++) {
				hit = data->hits [base_index + callers_count];
				if (hit.address != NULL) {
					regions_refreshed = write_statistical_hit ((current_thread != NULL) ? hit.domain : NULL, hit.address, regions_refreshed);
				} else {
					break;
				}
			}
		}
	}
	write_uint32 (MONO_PROFILER_STATISTICAL_CODE_END);
	
#if DEBUG_STATISTICAL_PROFILER
	printf ("[write_statistical_data_block] Ending loop at index %d\n", end_index);
#endif
	write_clock_data ();
	
	write_current_block (MONO_PROFILER_FILE_BLOCK_KIND_STATISTICAL);
}

static void
write_intro_block (void) {
	write_uint32 (1);
	write_string ("mono");
	write_uint32 (profiler->flags);
	write_uint64 (profiler->start_counter);
	write_uint64 (profiler->start_time);
	write_current_block (MONO_PROFILER_FILE_BLOCK_KIND_INTRO);
}

static void
write_end_block (void) {
	write_uint32 (1);
	write_uint64 (profiler->end_counter);
	write_uint64 (profiler->end_time);
	write_current_block (MONO_PROFILER_FILE_BLOCK_KIND_END);
}

//将ProfilerPerThreadData中未放入映射表中的事件数据都放入相应的映射表中
static void
update_mapping (ProfilerPerThreadData *data) {
	ProfilerEventData *start = data->first_unmapped_event;
	ProfilerEventData *end = data->next_free_event;
	data->first_unmapped_event = end;
 

	//对于所有未在映射表里的事件，都放入相应的映射表中。
	while (start < end) 
	{ 
		//注册新类
		if (start->data_type == MONO_PROFILER_EVENT_DATA_TYPE_CLASS) {
			ClassIdMappingElement *element = class_id_mapping_element_get (start->data.address);
			if (element == NULL) 
			{
				MonoClass *klass = start->data.address;
				class_id_mapping_element_new (klass);
			}
			//注册新方法
		} else if (start->data_type == MONO_PROFILER_EVENT_DATA_TYPE_METHOD) {
			MethodIdMappingElement *element = method_id_mapping_element_get (start->data.address);
			if (element == NULL) 
			{
				MonoMethod *method = start->data.address;
				if (method != NULL) 
				{
					method_id_mapping_element_new (method);
				}
			}
		} 
		//略过event值超过额定最大事件值的附加块
		if (start->value == MAX_EVENT_VALUE) {
			start ++;
		}
		start ++;
	} 
}

//对于每个线程Profile数据将未在映射表
//中注册的EventData注册，并将映射表写
//进文件。
static void
flush_all_mappings (void) 
{
	ProfilerPerThreadData *data; 
	for (data = profiler->per_thread_data; data != NULL; data = data->next) 
	{
		update_mapping (data);
	}
	for (data = profiler->per_thread_data; data != NULL; data = data->next) 
	{
		write_mapping_block (data->thread_id);
	}
}

static void
flush_full_event_data_buffer (ProfilerPerThreadData *data) {
	LOCK_PROFILER ();
	
	// We flush all mappings because some id definitions could come
	// from other threads
	flush_all_mappings ();
	g_assert (data->first_unmapped_event >= data->next_free_event);
	
	write_thread_data_block (data);
	
	data->next_free_event = data->events;
	data->next_unreserved_event = data->events;
	data->first_unwritten_event = data->events;
	data->first_unmapped_event = data->events;
	MONO_PROFILER_GET_CURRENT_COUNTER (data->start_event_counter);
	data->last_event_counter = data->start_event_counter;
	
	UNLOCK_PROFILER ();
}

//若空闲的event块无法满足分配数量的要求，则将event写入
/* The ">=" operator is intentional, to leave one spare slot for "extended values" */
#define RESERVE_EVENTS(d,e,count) do {\
	if ((d)->next_unreserved_event >= ((d)->end_event - (count))) {\
		flush_full_event_data_buffer (d);\
	}\
	(e) = (d)->next_unreserved_event;\
	(d)->next_unreserved_event += (count);\
} while (0)
#define GET_NEXT_FREE_EVENT(d,e) RESERVE_EVENTS ((d),(e),1)
#define COMMIT_RESERVED_EVENTS(d) do {\
	data->next_free_event = data->next_unreserved_event;\
} while (0)

static void
flush_everything (void) {
	ProfilerPerThreadData *data;
	
	flush_all_mappings ();
	for (data = profiler->per_thread_data; data != NULL; data = data->next) {
		write_thread_data_block (data);
	}
	write_statistical_data_block (profiler->statistical_data);
}

#define RESULT_TO_LOAD_CODE(r) (((r)==MONO_PROFILE_OK)?MONO_PROFILER_LOADED_EVENT_SUCCESS:MONO_PROFILER_LOADED_EVENT_FAILURE)
static void
appdomain_start_load (MonoProfiler *profiler, MonoDomain *domain) {

	LOCK_PROFILER ();
	loaded_element_load_start (profiler->loaded_appdomains, domain);
	UNLOCK_PROFILER ();
}

static void
appdomain_end_load (MonoProfiler *profiler, MonoDomain *domain, int result) {
	char *name;
	LoadedElement *element;

	LOCK_PROFILER ();
	name = g_strdup_printf ("%d", mono_domain_get_id (domain));
	element = loaded_element_load_end (profiler->loaded_appdomains, domain, name);
	write_element_load_block (element, MONO_PROFILER_LOADED_EVENT_APPDOMAIN | RESULT_TO_LOAD_CODE (result), CURRENT_THREAD_ID (), domain);
	UNLOCK_PROFILER ();
}

static void
appdomain_start_unload (MonoProfiler *profiler, MonoDomain *domain) {

	LOCK_PROFILER ();
	loaded_element_unload_start (profiler->loaded_appdomains, domain);
	//flush_everything ();
	UNLOCK_PROFILER ();
}

static void
appdomain_end_unload (MonoProfiler *profiler, MonoDomain *domain) {
	LoadedElement *element;

	LOCK_PROFILER ();
	element = loaded_element_unload_end (profiler->loaded_appdomains, domain);
	//write_element_unload_block (element, MONO_PROFILER_LOADED_EVENT_APPDOMAIN, CURRENT_THREAD_ID ());
	UNLOCK_PROFILER ();
}

static void
module_start_load (MonoProfiler *profiler, MonoImage *module) {
	LOCK_PROFILER ();
	loaded_element_load_start (profiler->loaded_modules, module);
	UNLOCK_PROFILER ();
}

static void
module_end_load (MonoProfiler *profiler, MonoImage *module, int result) {
	char *name;
	MonoAssemblyName aname;
	LoadedElement *element;

	if (mono_assembly_fill_assembly_name (module, &aname)) {
		name = mono_stringify_assembly_name (&aname);
	} else {
		name = g_strdup_printf ("Dynamic module \"%p\"", module);
	}
	LOCK_PROFILER ();
	element = loaded_element_load_end (profiler->loaded_modules, module, name);
	write_element_load_block (element, MONO_PROFILER_LOADED_EVENT_MODULE | RESULT_TO_LOAD_CODE (result), CURRENT_THREAD_ID (), module);
	UNLOCK_PROFILER ();
}

static void
module_start_unload (MonoProfiler *profiler, MonoImage *module) {
	LOCK_PROFILER ();

	loaded_element_unload_start (profiler->loaded_modules, module);
	flush_everything ();
	UNLOCK_PROFILER ();
}

static void
module_end_unload (MonoProfiler *profiler, MonoImage *module) {
	LoadedElement *element;
	
	LOCK_PROFILER ();
	element = loaded_element_unload_end (profiler->loaded_modules, module);
	write_element_unload_block (element, MONO_PROFILER_LOADED_EVENT_MODULE, CURRENT_THREAD_ID ());
	UNLOCK_PROFILER ();
}

static void
assembly_start_load (MonoProfiler *profiler, MonoAssembly *assembly) {
	LOCK_PROFILER (); 
	loaded_element_load_start (profiler->loaded_assemblies, assembly);
	UNLOCK_PROFILER ();
}

static void
assembly_end_load (MonoProfiler *profiler, MonoAssembly *assembly, int result) {
	char *name;
	MonoAssemblyName aname;
	LoadedElement *element;
	LOCK_PROFILER ();
	if (mono_assembly_fill_assembly_name (mono_assembly_get_image (assembly), &aname)) {
		name = mono_stringify_assembly_name (&aname);
	} else {
		name = g_strdup_printf ("Dynamic assembly \"%p\"", assembly);
	} 
	element = loaded_element_load_end (profiler->loaded_assemblies, assembly, name);
	//write_element_load_block (element, MONO_PROFILER_LOADED_EVENT_ASSEMBLY | RESULT_TO_LOAD_CODE (result), CURRENT_THREAD_ID (), assembly);
	UNLOCK_PROFILER ();
}

static void
assembly_start_unload (MonoProfiler *profiler, MonoAssembly *assembly) {
	LOCK_PROFILER ();
	loaded_element_unload_start (profiler->loaded_assemblies, assembly);
	//flush_everything ();
	UNLOCK_PROFILER ();
}
static void
assembly_end_unload (MonoProfiler *profiler, MonoAssembly *assembly) {
	LoadedElement *element; 
	LOCK_PROFILER ();
	
	element = loaded_element_unload_end (profiler->loaded_assemblies, assembly);
	mark_all_unload_assembly_class_dead( element->id );
	//write_element_unload_block (element, MONO_PROFILER_LOADED_EVENT_ASSEMBLY, CURRENT_THREAD_ID ());
	UNLOCK_PROFILER ();
}

#if (DEBUG_LOGGING_PROFILER)		
static const char*
class_event_code_to_string (MonoProfilerClassEvents code) {
	switch (code) {
	case MONO_PROFILER_EVENT_CLASS_LOAD: return "LOAD";
	case MONO_PROFILER_EVENT_CLASS_UNLOAD: return "UNLOAD";
	case MONO_PROFILER_EVENT_CLASS_ALLOCATION: return "ALLOCATION";
	case MONO_PROFILER_EVENT_CLASS_EXCEPTION: return "EXCEPTION";
	default: g_assert_not_reached (); return "";
	}
}
static const char*
method_event_code_to_string (MonoProfilerMethodEvents code) {
	switch (code) {
	case MONO_PROFILER_EVENT_METHOD_CALL: return "CALL";
	case MONO_PROFILER_EVENT_METHOD_JIT: return "JIT";
	case MONO_PROFILER_EVENT_METHOD_FREED: return "FREED";
	case MONO_PROFILER_EVENT_METHOD_ALLOCATION_CALLER: return "ALLOCATION_CALLER";
	case MONO_PROFILER_EVENT_METHOD_ALLOCATION_JIT_TIME_CALLER: return "ALLOCATION_JIT_TIME_CALLER";
	case MONO_PROFILER_EVENT_ALLOCATION_OBJECT_ID: return "ALLOCATION_OBJECT_ID";
	default: g_assert_not_reached (); return "";
	}
}
static const char*
number_event_code_to_string (MonoProfilerEvents code) {
	switch (code) {
	case MONO_PROFILER_EVENT_THREAD: return "THREAD";
	case MONO_PROFILER_EVENT_GC_COLLECTION: return "GC_COLLECTION";
	case MONO_PROFILER_EVENT_GC_MARK: return "GC_MARK";
	case MONO_PROFILER_EVENT_GC_SWEEP: return "GC_SWEEP";
	case MONO_PROFILER_EVENT_GC_RESIZE: return "GC_RESIZE";
	case MONO_PROFILER_EVENT_GC_STOP_WORLD: return "GC_STOP_WORLD";
	case MONO_PROFILER_EVENT_GC_START_WORLD: return "GC_START_WORLD";
	case MONO_PROFILER_EVENT_JIT_TIME_ALLOCATION: return "JIT_TIME_ALLOCATION";
	case MONO_PROFILER_EVENT_STACK_SECTION: return "STACK_SECTION";
	case MONO_PROFILER_EVENT_ALLOCATION_OBJECT_ID: return "ALLOCATION_OBJECT_ID";
	default: g_assert_not_reached (); return "";
	}
}
static const char*
event_result_to_string (MonoProfilerEventResult code) {
	switch (code) {
	case MONO_PROFILER_EVENT_RESULT_SUCCESS: return "SUCCESS";
	case MONO_PROFILER_EVENT_RESULT_FAILURE: return "FAILURE";
	default: g_assert_not_reached (); return "";
	}
}
static const char*
event_kind_to_string (MonoProfilerEventKind code) {
	switch (code) {
	case MONO_PROFILER_EVENT_KIND_START: return "START";
	case MONO_PROFILER_EVENT_KIND_END: return "END";
	default: g_assert_not_reached (); return "";
	}
}
static void
print_event_data (ProfilerPerThreadData *data, ProfilerEventData *event, guint64 value) {
	if (event->data_type == MONO_PROFILER_EVENT_DATA_TYPE_CLASS) {
		printf ("STORE EVENT [TID %ld][EVENT %ld] CLASS[%p] %s:%s:%s[%d-%d-%d] %ld (%s.%s)\n",
				data->thread_id,
				event - data->events,
				event->data.address,
				class_event_code_to_string (event->code & ~MONO_PROFILER_EVENT_RESULT_MASK),
				event_result_to_string (event->code & MONO_PROFILER_EVENT_RESULT_MASK),
				event_kind_to_string (event->kind),
				event->data_type,
				event->kind,
				event->code,
				value,
				mono_class_get_namespace ((MonoClass*) event->data.address),
				mono_class_get_name ((MonoClass*) event->data.address));
	} else if (event->data_type == MONO_PROFILER_EVENT_DATA_TYPE_METHOD) {
		printf ("STORE EVENT [TID %ld][EVENT %ld]  METHOD[%p] %s:%s:%s[%d-%d-%d] %ld (%s.%s:%s (?))\n",
				data->thread_id,
				event - data->events,
				event->data.address,
				method_event_code_to_string (event->code & ~MONO_PROFILER_EVENT_RESULT_MASK),
				event_result_to_string (event->code & MONO_PROFILER_EVENT_RESULT_MASK),
				event_kind_to_string (event->kind),
				event->data_type,
				event->kind,
				event->code,
				value,
				(event->data.address != NULL) ? mono_class_get_namespace (mono_method_get_class ((MonoMethod*) event->data.address)) : "<NULL>",
				(event->data.address != NULL) ? mono_class_get_name (mono_method_get_class ((MonoMethod*) event->data.address)) : "<NULL>",
				(event->data.address != NULL) ? mono_method_get_name ((MonoMethod*) event->data.address) : "<NULL>");
	} else {
		printf ("STORE EVENT [TID %ld][EVENT %ld]  NUMBER[%ld] %s:%s[%d-%d-%d] %ld\n",
				data->thread_id,
				event - data->events,
				(guint64) event->data.number,
				number_event_code_to_string (event->code),
				event_kind_to_string (event->kind),
				event->data_type,
				event->kind,
				event->code,
				value);
	}
}
#define LOG_EVENT(data,ev,val) print_event_data ((data),(ev),(val))
#else
#define LOG_EVENT(data,ev,val)
#endif

#define RESULT_TO_EVENT_CODE(r) (((r)==MONO_PROFILE_OK)?MONO_PROFILER_EVENT_RESULT_SUCCESS:MONO_PROFILER_EVENT_RESULT_FAILURE)

#define STORE_EVENT_ITEM_COUNTER(event,p,i,dt,c,k) do {\
	guint64 counter;\
	guint64 delta;\
	MONO_PROFILER_GET_CURRENT_COUNTER (counter);\
	(event)->data.address = (i);\
	(event)->data_type = (dt);\
	(event)->code = (c);\
	(event)->kind = (k);\
	delta = counter - data->last_event_counter;\
	if (delta < MAX_EVENT_VALUE) {\
		(event)->value = delta;\
	} else {\
		ProfilerEventData *extension = data->next_unreserved_event;\
		data->next_unreserved_event ++;\
		(event)->value = MAX_EVENT_VALUE;\
		*(guint64*)extension = delta;\
	}\
	data->last_event_counter = counter;\
	LOG_EVENT (data, (event), delta);\
} while (0);

//将事件信息保存至指定的event，若event的value超过value所
//能保存的最大值，将下一个待待分配event结构的前64位作为
//空间保存该event的值。（注：使用该宏前需要声明ProfilerPerThreadData *data，
//并获取该线程的ProfilerPerThreadData）
#define STORE_EVENT_ITEM_VALUE(event,p,i,dt,c,k,v) do {\
	(event)->data.address = (i);\
	(event)->data_type = (dt);\
	(event)->code = (c);\
	(event)->kind = (k);\
	if ((v) < MAX_EVENT_VALUE) {\
		(event)->value = (v);\
	} else {\
		ProfilerEventData *extension = data->next_unreserved_event;\
		data->next_unreserved_event ++;\
		(event)->value = MAX_EVENT_VALUE;\
		*(guint64*)extension = (v);\
	}\
	LOG_EVENT (data, (event), (v));\
}while (0);
#define STORE_EVENT_NUMBER_COUNTER(event,p,n,dt,c,k) do {\
	guint64 counter;\
	guint64 delta;\
	MONO_PROFILER_GET_CURRENT_COUNTER (counter);\
	(event)->data.number = (n);\
	(event)->data_type = (dt);\
	(event)->code = (c);\
	(event)->kind = (k);\
	delta = counter - data->last_event_counter;\
	if (delta < MAX_EVENT_VALUE) {\
		(event)->value = delta;\
	} else {\
		ProfilerEventData *extension = data->next_unreserved_event;\
		data->next_unreserved_event ++;\
		(event)->value = MAX_EVENT_VALUE;\
		*(guint64*)extension = delta;\
	}\
	data->last_event_counter = counter;\
	LOG_EVENT (data, (event), delta);\
}while (0);
#define STORE_EVENT_NUMBER_VALUE(event,p,n,dt,c,k,v) do {\
	(event)->data.number = (n);\
	(event)->data_type = (dt);\
	(event)->code = (c);\
	(event)->kind = (k);\
	if ((v) < MAX_EVENT_VALUE) {\
		(event)->value = (v);\
	} else {\
		ProfilerEventData *extension = data->next_unreserved_event;\
		data->next_unreserved_event ++;\
		(event)->value = MAX_EVENT_VALUE;\
		*(guint64*)extension = (v);\
	}\
	LOG_EVENT (data, (event), (v));\
}while (0);
#define INCREMENT_EVENT(event) do {\
	if ((event)->value != MAX_EVENT_VALUE) {\
		(event) ++;\
	} else {\
		(event) += 2;\
	}\
}while (0);



static void
class_start_load (MonoProfiler *profiler, MonoClass *klass) 
{ 
}
static void
class_end_load (MonoProfiler *profiler, MonoClass *klass, int result) 
{ 
	//若类没有加载成功忽略此类
	if( result != MONO_PROFILE_OK )
		return;
	 
	mono_loader_lock();
	LOCK_PROFILER(); 
	try_insert_class_id_mapping_element( klass );
	UNLOCK_PROFILER();
	mono_loader_unlock();
}
static void
class_start_unload (MonoProfiler *profiler, MonoClass *klass) 
{ 
}
static void
class_end_unload (MonoProfiler *profiler, MonoClass *klass) 
{ 
}

static void
method_start_jit (MonoProfiler *profiler, MonoMethod *method) {
	ProfilerPerThreadData *data;
	ProfilerEventData *event;

	GET_PROFILER_THREAD_DATA (data);
	GET_NEXT_FREE_EVENT (data, event);
	thread_stack_push_jitted_safely (&(data->stack), method, TRUE);
	STORE_EVENT_ITEM_COUNTER (event, profiler, method, MONO_PROFILER_EVENT_DATA_TYPE_METHOD, MONO_PROFILER_EVENT_METHOD_JIT, MONO_PROFILER_EVENT_KIND_START);
	COMMIT_RESERVED_EVENTS (data);
}
static void
method_end_jit (MonoProfiler *profiler, MonoMethod *method, int result) {
	ProfilerPerThreadData *data;
	ProfilerEventData *event;

	GET_PROFILER_THREAD_DATA (data);
	GET_NEXT_FREE_EVENT (data, event);
	STORE_EVENT_ITEM_COUNTER (event, profiler, method, MONO_PROFILER_EVENT_DATA_TYPE_METHOD, MONO_PROFILER_EVENT_METHOD_JIT | RESULT_TO_EVENT_CODE (result), MONO_PROFILER_EVENT_KIND_END);
	thread_stack_pop (&(data->stack));
	COMMIT_RESERVED_EVENTS (data);
}

#if (HAS_OPROFILE)
static void
method_jit_result (MonoProfiler *prof, MonoMethod *method, MonoJitInfo* jinfo, int result) {
	if (profiler->action_flags.oprofile && (result == MONO_PROFILE_OK)) {
		MonoClass *klass = mono_method_get_class (method);
		char *signature = mono_signature_get_desc (mono_method_signature (method), TRUE);
		char *name = g_strdup_printf ("%s.%s:%s (%s)", mono_class_get_namespace (klass), mono_class_get_name (klass), mono_method_get_name (method), signature);
		gpointer code_start = mono_jit_info_get_code_start (jinfo);
		int code_size = mono_jit_info_get_code_size (jinfo);
		
		if (op_write_native_code (name, code_start, code_size)) {
			g_warning ("Problem calling op_write_native_code\n");
		}
		
		g_free (signature);
		g_free (name);
	}
}
#endif


static void
method_enter (MonoProfiler *profiler, MonoMethod *method) {
	
	ProfilerPerThreadData *data;
	
	CHECK_PROFILER_ENABLED ();
	GET_PROFILER_THREAD_DATA (data);
	if (profiler->action_flags.track_calls) {
		ProfilerEventData *event;
		GET_NEXT_FREE_EVENT (data, event);
		STORE_EVENT_ITEM_COUNTER (event, profiler, method, MONO_PROFILER_EVENT_DATA_TYPE_METHOD, MONO_PROFILER_EVENT_METHOD_CALL, MONO_PROFILER_EVENT_KIND_START);
		COMMIT_RESERVED_EVENTS (data);
	}
	if (profiler->action_flags.track_stack) {
		thread_stack_push_safely (&(data->stack), method);
	}
}
static void
method_leave (MonoProfiler *profiler, MonoMethod *method) {
	ProfilerPerThreadData *data;
	
	CHECK_PROFILER_ENABLED ();
	GET_PROFILER_THREAD_DATA (data);
	if (profiler->action_flags.track_calls) {
		ProfilerEventData *event;
		GET_NEXT_FREE_EVENT (data, event);
		STORE_EVENT_ITEM_COUNTER (event, profiler, method, MONO_PROFILER_EVENT_DATA_TYPE_METHOD, MONO_PROFILER_EVENT_METHOD_CALL, MONO_PROFILER_EVENT_KIND_END);
		COMMIT_RESERVED_EVENTS (data);
	}
	if (profiler->action_flags.track_stack) {
		thread_stack_pop (&(data->stack));
	}
}

static void
method_free (MonoProfiler *profiler, MonoMethod *method) {
	ProfilerPerThreadData *data;
	ProfilerEventData *event;

	GET_PROFILER_THREAD_DATA (data);
	GET_NEXT_FREE_EVENT (data, event);
	STORE_EVENT_ITEM_COUNTER (event, profiler, method, MONO_PROFILER_EVENT_DATA_TYPE_METHOD, MONO_PROFILER_EVENT_METHOD_FREED, 0);
	COMMIT_RESERVED_EVENTS (data);
}

static void
thread_start (MonoProfiler *profiler, gsize tid) {
	ProfilerPerThreadData *data;
	ProfilerEventData *event;

	GET_PROFILER_THREAD_DATA (data);
	GET_NEXT_FREE_EVENT (data, event);
	STORE_EVENT_NUMBER_COUNTER (event, profiler, tid, MONO_PROFILER_EVENT_DATA_TYPE_OTHER, MONO_PROFILER_EVENT_THREAD, MONO_PROFILER_EVENT_KIND_START);
	COMMIT_RESERVED_EVENTS (data);
}
static void
thread_end (MonoProfiler *profiler, gsize tid) {
	ProfilerPerThreadData *data;
	ProfilerEventData *event;
	
	GET_PROFILER_THREAD_DATA (data);
	GET_NEXT_FREE_EVENT (data, event);
	STORE_EVENT_NUMBER_COUNTER (event, profiler, tid, MONO_PROFILER_EVENT_DATA_TYPE_OTHER, MONO_PROFILER_EVENT_THREAD, MONO_PROFILER_EVENT_KIND_END);
	COMMIT_RESERVED_EVENTS (data);
}

static ProfilerEventData*
save_stack_delta (MonoProfiler *profiler, ProfilerPerThreadData *data, ProfilerEventData *events, int unsaved_frames) {
	int i;
	
	/* In this loop it is safe to simply increment "events" because MAX_EVENT_VALUE cannot be reached. */
	STORE_EVENT_NUMBER_VALUE (events, profiler, data->stack.last_saved_top, MONO_PROFILER_EVENT_DATA_TYPE_OTHER, MONO_PROFILER_EVENT_STACK_SECTION, 0, unsaved_frames);
	events++;
	for (i = 0; i < unsaved_frames; i++) {
		if (! thread_stack_index_from_top_is_jitted (&(data->stack), i)) {
			STORE_EVENT_ITEM_VALUE (events, profiler, thread_stack_index_from_top (&(data->stack), i), MONO_PROFILER_EVENT_DATA_TYPE_METHOD, MONO_PROFILER_EVENT_METHOD_ALLOCATION_CALLER, 0, 0);
		} else {
			STORE_EVENT_ITEM_VALUE (events, profiler, thread_stack_index_from_top (&(data->stack), i), MONO_PROFILER_EVENT_DATA_TYPE_METHOD, MONO_PROFILER_EVENT_METHOD_ALLOCATION_JIT_TIME_CALLER, 0, 0);
		}
		events ++;
	}
	
	data->stack.last_saved_top = data->stack.top;
	
	return events;
}

static void profiler_heap_add_object (ProfilerHeapShotHeapBuffers *heap, MonoObject *obj,ClassIdMappingElement* class_elem) ;

//对象分配回调，此回调会在分配线程中调用
static void
object_allocated (MonoProfiler *profiler, MonoObject *obj, MonoClass *klass) 
{

	ClassIdMappingElement* class_id = NULL; 
	ProfilerClassInfo* classinfo = NULL;
	ProfilerClassInfo* elem_classinfo = NULL;
	mono_loader_lock();
	LOCK_PROFILER();

	//对象分配，说明对应类已经初始化完毕
	class_id = class_id_mapping_element_get(klass);
	g_assert( class_id != NULL );

	//类是否还存活
	if( class_id->data.alive )
	{
		g_assert( klass->inited );

		//创建更新主类相应的ProfilerClassInfo
		classinfo = request_a_classinfo( class_id ); 
		if( class_id->data.layout.slots == CLASS_LAYOUT_NOT_INITIALIZED )
		{
			class_id_mapping_element_build_layout_bitmap( klass , class_id ); 
			class_id_mapping_element_build_refs( class_id);	 
			update_classinfo( classinfo , class_id );
		}


		//若类为数组类型
		if( mono_class_get_rank(klass) )
		{
			MonoClass* element_class = mono_class_get_element_class(klass);
			ClassIdMappingElement* elem_class_id = NULL;
			g_assert( element_class != NULL );
			g_assert( element_class->inited ); 

			elem_class_id = class_id_mapping_element_get( element_class );
			g_assert( elem_class_id != NULL );

			//创建并更新元素类的ProfilerClassInfo
			elem_classinfo = request_a_classinfo( elem_class_id ); 
			if( elem_class_id->data.layout.slots == CLASS_LAYOUT_NOT_INITIALIZED )
			{
				class_id_mapping_element_build_layout_bitmap( element_class , elem_class_id );
				class_id_mapping_element_build_refs(elem_class_id ); 
				update_classinfo( elem_classinfo , elem_class_id );
			}  
		}

		//将对象放入记录中
		profiler_heap_add_object( &(profiler->heap) , obj , class_id );   

	} //if( class_id->data.alive )
	 
	UNLOCK_PROFILER();
	mono_loader_unlock(); 
}

static void
monitor_event (MonoProfiler *profiler, MonoObject *obj, MonoProfilerMonitorEvent event) {
	ProfilerPerThreadData *data;
	ProfilerEventData *events;
	MonoClass *klass;
	int unsaved_frames;
	int event_slot_count;
	
	CHECK_PROFILER_ENABLED ();
	
	GET_PROFILER_THREAD_DATA (data);
	klass = mono_object_get_class (obj);
	
	unsaved_frames = thread_stack_count_unsaved_frames (&(data->stack));
	if (unsaved_frames > 0) {
		event_slot_count = unsaved_frames + 3;
	} else {
		event_slot_count = 2;
	}
	
	RESERVE_EVENTS (data, events, event_slot_count);
	if (unsaved_frames > 0) {
		events = save_stack_delta (profiler, data, events, unsaved_frames);
	}
	STORE_EVENT_ITEM_COUNTER (events, profiler, klass, MONO_PROFILER_EVENT_DATA_TYPE_CLASS, MONO_PROFILER_EVENT_CLASS_MONITOR, MONO_PROFILER_EVENT_KIND_START);
	INCREMENT_EVENT (events);
	STORE_EVENT_ITEM_VALUE (events, profiler, obj, MONO_PROFILER_EVENT_DATA_TYPE_OTHER, MONO_PROFILER_EVENT_OBJECT_MONITOR, 0, event);
	COMMIT_RESERVED_EVENTS (data);
}

static void
statistical_call_chain (MonoProfiler *profiler, int call_chain_depth, guchar **ips, void *context) {
	
	MonoDomain *domain = mono_domain_get ();
	ProfilerStatisticalData *data;
	unsigned int index;
	
	CHECK_PROFILER_ENABLED ();
	do {
		data = profiler->statistical_data;
		index = InterlockedIncrement ((int*) &data->next_free_index);
		
		if (index <= data->end_index) {
			unsigned int base_index = (index - 1) * (profiler->statistical_call_chain_depth + 1);
			unsigned int call_chain_index = 0;
			
			//printf ("[statistical_call_chain] (%d)\n", call_chain_depth);
			while (call_chain_index < call_chain_depth) {
				ProfilerStatisticalHit *hit = & (data->hits [base_index + call_chain_index]);
				//printf ("[statistical_call_chain] [%d] = %p\n", base_index + call_chain_index, ips [call_chain_index]);
				hit->address = (gpointer) ips [call_chain_index];
				hit->domain = domain;
				call_chain_index ++;
			}
			while (call_chain_index <= profiler->statistical_call_chain_depth) {
				ProfilerStatisticalHit *hit = & (data->hits [base_index + call_chain_index]);
				//printf ("[statistical_call_chain] [%d] = NULL\n", base_index + call_chain_index);
				hit->address = NULL;
				hit->domain = NULL;
				call_chain_index ++;
			}
		} else {
			/* Check if we are the one that must swap the buffers */
			if (index == data->end_index + 1) {
				ProfilerStatisticalData *new_data;

				/* In the *impossible* case that the writer thread has not finished yet, */
				/* loop waiting for it and meanwhile lose all statistical events... */
				do {
					/* First, wait that it consumed the ready buffer */
					while (profiler->statistical_data_ready != NULL);
					/* Then, wait that it produced the free buffer */
					new_data = profiler->statistical_data_second_buffer;
				} while (new_data == NULL);

				profiler->statistical_data_ready = data;
				profiler->statistical_data = new_data;
				profiler->statistical_data_second_buffer = NULL;
				WRITER_EVENT_RAISE ();
				/* Otherwise exit from the handler and drop the event... */
			} else {
				break;
			}
			
			/* Loop again, hoping to acquire a free slot this time (otherwise the event will be dropped) */
			data = NULL;
		}
	} while (data == NULL);
}

static void
statistical_hit (MonoProfiler *profiler, guchar *ip, void *context) {
	MonoDomain *domain = mono_domain_get ();
	ProfilerStatisticalData *data;
	unsigned int index;
	
	CHECK_PROFILER_ENABLED ();
	do {
		data = profiler->statistical_data;
		index = InterlockedIncrement ((int*) &data->next_free_index);
		
		if (index <= data->end_index) {
			ProfilerStatisticalHit *hit = & (data->hits [index - 1]);
			hit->address = (gpointer) ip;
			hit->domain = domain;
		} else {
			/* Check if we are the one that must swap the buffers */
			if (index == data->end_index + 1) {
				ProfilerStatisticalData *new_data;

				/* In the *impossible* case that the writer thread has not finished yet, */
				/* loop waiting for it and meanwhile lose all statistical events... */
				do {
					/* First, wait that it consumed the ready buffer */
					while (profiler->statistical_data_ready != NULL);
					/* Then, wait that it produced the free buffer */
					new_data = profiler->statistical_data_second_buffer;
				} while (new_data == NULL);
				
				profiler->statistical_data_ready = data;
				profiler->statistical_data = new_data;
				profiler->statistical_data_second_buffer = NULL;
				WRITER_EVENT_RAISE ();
			}
			
			/* Loop again, hoping to acquire a free slot this time */
			data = NULL;
		}
	} while (data == NULL);
}

static MonoProfilerEvents
gc_event_code_from_profiler_event (MonoGCEvent event) {
	switch (event) {
	case MONO_GC_EVENT_START:
	case MONO_GC_EVENT_END:
		return MONO_PROFILER_EVENT_GC_COLLECTION;
	case MONO_GC_EVENT_MARK_START:
	case MONO_GC_EVENT_MARK_END:
		return MONO_PROFILER_EVENT_GC_MARK;
	case MONO_GC_EVENT_RECLAIM_START:
	case MONO_GC_EVENT_RECLAIM_END:
		return MONO_PROFILER_EVENT_GC_SWEEP;
	case MONO_GC_EVENT_PRE_STOP_WORLD:
	case MONO_GC_EVENT_POST_STOP_WORLD:
		return MONO_PROFILER_EVENT_GC_STOP_WORLD;
	case MONO_GC_EVENT_PRE_START_WORLD:
	case MONO_GC_EVENT_POST_START_WORLD:
		return MONO_PROFILER_EVENT_GC_START_WORLD;
	default:
		g_assert_not_reached ();
		return 0;
	}
}

static MonoProfilerEventKind
gc_event_kind_from_profiler_event (MonoGCEvent event) {
	switch (event) {
	case MONO_GC_EVENT_START:
	case MONO_GC_EVENT_MARK_START:
	case MONO_GC_EVENT_RECLAIM_START:
	case MONO_GC_EVENT_PRE_STOP_WORLD:
	case MONO_GC_EVENT_PRE_START_WORLD:
		return MONO_PROFILER_EVENT_KIND_START;
	case MONO_GC_EVENT_END:
	case MONO_GC_EVENT_MARK_END:
	case MONO_GC_EVENT_RECLAIM_END:
	case MONO_GC_EVENT_POST_START_WORLD:
	case MONO_GC_EVENT_POST_STOP_WORLD:
		return MONO_PROFILER_EVENT_KIND_END;
	default:
		g_assert_not_reached ();
		return 0;
	}
}

static gboolean
dump_current_heap_snapshot (void) {
	gboolean result;
	
	if (profiler->heap_shot_was_requested) {
		result = TRUE;
	} else {
		if (profiler->dump_next_heap_snapshots > 0) {
			profiler->dump_next_heap_snapshots--;
			result = TRUE;
		} else if (profiler->dump_next_heap_snapshots < 0) {
			result = TRUE;
		} else {
			result = FALSE;
		}
	}
	
	return result;
}

static void
profiler_heap_buffers_setup (ProfilerHeapShotHeapBuffers *heap) 
{
	heap->buffers = g_new (ProfilerHeapShotHeapBuffer, 1);
	heap->buffers->previous = NULL;
	heap->buffers->next = NULL;
	heap->buffers->start_slot = &(heap->buffers->buffer [0]);
	heap->buffers->end_slot = &(heap->buffers->buffer [PROFILER_HEAP_SHOT_HEAP_BUFFER_SIZE]);
	heap->last = heap->buffers;
	heap->current = heap->buffers;
	heap->first_free_slot = & (heap->buffers->buffer [0]);
}
static void
profiler_heap_buffers_clear (ProfilerHeapShotHeapBuffers *heap) {
	heap->buffers = NULL;
	heap->last = NULL;
	heap->current = NULL;
	heap->first_free_slot = NULL;
}
static void
profiler_heap_buffers_free (ProfilerHeapShotHeapBuffers *heap) {
	ProfilerHeapShotHeapBuffer *current = heap->buffers;
	while (current != NULL) {
		ProfilerHeapShotHeapBuffer *next = current->next;
		g_free (current);
		current = next;
	}
	profiler_heap_buffers_clear (heap);
}



static int
report_object_references (gpointer *start, ClassIdMappingElement *layout, ProfilerHeapShotWriteJob *job) {
	int reported_references = 0;
	int slot;
	
	for (slot = 0; slot < layout->data.layout.slots; slot ++) {
		gboolean slot_has_reference;
		if (layout->data.layout.slots <= CLASS_LAYOUT_PACKED_BITMAP_SIZE) {
			if (layout->data.bitmap.compact & (((guint64)1) << slot)) {
				slot_has_reference = TRUE;
			} else {
				slot_has_reference = FALSE;
			}
		} else {
			if (layout->data.bitmap.extended [slot >> 3] & (1 << (slot & 7))) {
				slot_has_reference = TRUE;
			} else {
				slot_has_reference = FALSE;
			}
		}
		
		if (slot_has_reference) 
		{
			gpointer field = start [slot];
			
			if ((field != NULL) && mono_object_is_alive(field)) 
			{
				reported_references ++;
				WRITE_HEAP_SHOT_JOB_VALUE (job, field);
			}
		}
	}
	
	return reported_references;
}

static int
record_object_field_references(gpointer *start, ProfilerClassInfo *classinfo)
{
	int references_counter = 0;
	int slot;

	for (slot = 0; slot < classinfo->layout.slots; slot++)
	{
		gboolean slot_has_reference;
		if (classinfo->layout.slots <= CLASS_LAYOUT_PACKED_BITMAP_SIZE) 
		{
			if (classinfo->bitmap.compact & (((guint64)1) << slot)) {
				slot_has_reference = TRUE;
			} else {
				slot_has_reference = FALSE;
			}
		}
		else {
			if (classinfo->bitmap.extended[slot >> 3] & (1 << (slot & 7))) 
			{
				slot_has_reference = TRUE;
			}
			else {
				slot_has_reference = FALSE;
			}
		}

		if (slot_has_reference)
		{
			gpointer field = start[slot];

			if( field != NULL )
			{  
				//查看field是否在堆记录中存在
				if( query_heap_object_info(field) )
				{
					if ( mono_object_is_alive(field))
					{
						add_ref_to_references_buf(field);
						references_counter++;
					}
				}
			}
		}
	}

	return references_counter;
}

static int 
record_object_references( ProfilerHeapObjectInfo* obj_info)
{
	MonoClass *klass = NULL;
	MonoObject *obj = NULL;
	int reference_counter = 0; 
	ProfilerClassInfo* classinfo = NULL;
	  
	obj = obj_info->obj;
	klass = obj_info->klass; 
	classinfo = query_classinfo_by_id( obj_info->class_id );

	//若为数组类型
	if (classinfo->rank)
	{
		MonoArray *array = (MonoArray *)obj; 
		ProfilerClassInfo* elem_classinfo = query_classinfo_by_id( classinfo->elem_class_id);
 
		//Element不为内置值类型
		if (!classinfo->elem_is_valuetype)
		{
			int length = mono_array_length(array);
			int i;
			for (i = 0; i < length; i++)
			{
				MonoObject *array_element = mono_array_get(array, MonoObject*, i);
				if ((array_element != NULL) && (mono_object_is_alive(array_element) == 1))
				{
					add_ref_to_references_buf(array_element);
					reference_counter++; 
				}
			}
		}
		else if (elem_classinfo->layout.references > 0)
		{//若元素为值类型且有引用 
			int length = mono_array_length(array);
			int array_element_size = classinfo->array_elem_size;
			int i;
			for (i = 0; i < length; i++)
			{
				gpointer array_element_address = mono_array_addr_with_size(array, array_element_size, i);
				reference_counter += record_object_field_references( (gpointer)(((char*)array_element_address)+sizeof(MonoObject)), elem_classinfo);
			}
		}
	} else { //若为对象类型
		if (classinfo->layout.references > 0)
		{
			reference_counter += record_object_field_references((gpointer)(((char*)obj) + sizeof (MonoObject)), classinfo);
		}
	}

	return reference_counter;
}




static void
profiler_heap_report_object_reachable (ProfilerHeapShotWriteJob *job, ProfilerHeapObjectInfo *obj_info ) 
{
		MonoObject* obj = NULL; 
		ProfilerClassInfo* classinfo = NULL;
		int iref = 0;
		int ref_offset = 0; 
		obj = obj_info->obj;  

		if( NULL == job )
			return;
	  
		//更新对象总结信息
		if (job->summary.capacity > 0) 
		{
			guint32 id = obj_info->class_id;
			g_assert (id < job->summary.capacity);
			
			//记录可达对象数量
			job->summary.per_class_data [id].reachable.instances ++;
			job->summary.per_class_data[id].reachable.bytes += obj_info->size;
		}

		if (profiler->action_flags.heap_shot && job->dump_heap_data) 
		{

			int reference_counter = 0;  
			WRITE_HEAP_SHOT_JOB_VALUE(job,GUINT_TO_POINTER(HEAP_CODE_OBJECT));
			WRITE_HEAP_SHOT_JOB_VALUE(job,obj);
			WRITE_HEAP_SHOT_JOB_VALUE(job, GUINT_TO_POINTER(obj_info->class_id)); 
			WRITE_HEAP_SHOT_JOB_VALUE(job,GUINT_TO_POINTER(obj_info->size));
			WRITE_HEAP_SHOT_JOB_VALUE(job, GINT_TO_POINTER(obj_info->refs_count)); 
			 
			//向job中写入所有引用
			ref_offset = obj_info->ref_start_indx; 
			for (iref = 0; iref < obj_info->refs_count; iref++)
			{
				WRITE_HEAP_SHOT_JOB_VALUE(job, profiler->references_buf[ref_offset+iref]);
			} 
		}
}
static void
profiler_heap_report_object_unreachable(ProfilerHeapShotWriteJob *job, ProfilerHeapObjectInfo *obj_info) {
	if (obj_info != NULL) 
	{
		MonoObject* obj = obj_info->obj;
		MonoClass *klass = obj_info->klass;
		guint32 size = obj_info->size;
		
		if (job->summary.capacity > 0) 
		{ 
			guint32 id = obj_info->class_id;
			g_assert (id < job->summary.capacity);
			
			job->summary.per_class_data [id].unreachable.instances ++;
			job->summary.per_class_data [id].unreachable.bytes += size;
		}

		if (profiler->action_flags.unreachable_objects && job->dump_heap_data) 
		{ 
			WRITE_HEAP_SHOT_JOB_VALUE(job,GUINT_TO_POINTER(HEAP_CODE_FREE_OBJECT_CLASS));
			WRITE_HEAP_SHOT_JOB_VALUE(job,GUINT_TO_POINTER(obj_info->class_id)); 
			WRITE_HEAP_SHOT_JOB_VALUE (job, GUINT_TO_POINTER (size));
		}
	}
}

//向堆中添加一个对象记录
static void
profiler_heap_add_object (ProfilerHeapShotHeapBuffers *heap, MonoObject *obj , ClassIdMappingElement* class_elem ) 
{ 
	LoadedElement *appdomain_elem = NULL; 
	g_hash_table_insert( profiler->heap_objs_tab , obj , obj );

	//初始化对象信息
	heap->first_free_slot->obj = obj;
	heap->first_free_slot->klass = obj->vtable->klass;
	heap->first_free_slot->class_id = class_elem->id;
	heap->first_free_slot->size = mono_object_get_size(obj);
	heap->first_free_slot->domain = obj->vtable->domain ;
	heap->first_free_slot->ref_start_indx = 0;
	heap->first_free_slot->refs_count = 0;
	heap->first_free_slot ++;

	//最后一个槽位不使用
	if (heap->first_free_slot >= heap->current->end_slot) 
	{
		if (heap->current->next != NULL) 
		{
			heap->current = heap->current->next;
		} else {
			ProfilerHeapShotHeapBuffer *buffer = g_new0(ProfilerHeapShotHeapBuffer, 1);
			buffer->previous = heap->last;
			buffer->next = NULL;
			buffer->start_slot = &(buffer->buffer [0]);
			buffer->end_slot = &(buffer->buffer [PROFILER_HEAP_SHOT_HEAP_BUFFER_SIZE]);
			heap->current = buffer;
			heap->last->next = buffer;
			heap->last = buffer;
		}
		heap->first_free_slot = &(heap->current->buffer [0]);
	} 
}

//此函数从对象链表队尾弹出已死亡的对象，若发现在current_slot前还有存活对象
//则返回此存活对象以覆盖待删除的current_slot对象。
static ProfilerHeapObjectInfo
profiler_heap_pop_object_from_end (ProfilerHeapShotHeapBuffers *heap,  ProfilerHeapObjectInfo* current_slot) 
{
	ProfilerHeapObjectInfo objinfo;
	ProfilerHeapObjectInfo null_obj_info;
	null_obj_info.obj = 0;
	null_obj_info.klass =0;
	null_obj_info.alive = 0;

	while (heap->first_free_slot != current_slot) 
	{ 
		if (heap->first_free_slot > heap->current->start_slot) 
		{
			heap->first_free_slot --;
		} else {
			heap->current = heap->current->previous;
			g_assert (heap->current != NULL);
			heap->first_free_slot = heap->current->end_slot - 1;
		}
		
		objinfo = *(heap->first_free_slot); 
		if ( objinfo.alive ) 
		{
			return objinfo;
		}
	}
	return null_obj_info;
}

static void
profiler_heap_scan (ProfilerHeapShotHeapBuffers *heap) 
{
	ProfilerHeapShotHeapBuffer *current_buffer = heap->buffers;
	ProfilerHeapObjectInfo* current_slot = current_buffer->start_slot;
	
	//对Profiler记录的堆中的所有对象，查看其是否还活着
	while (current_slot != heap->first_free_slot) 
	{ 

		if (mono_object_is_alive(current_slot->obj))
		{
			ClassIdMappingElement* class_id = (ClassIdMappingElement*)class_id_mapping_element_get( current_slot->klass );
			LoadedElement* domain_elem = (LoadedElement*)g_hash_table_lookup(profiler->loaded_appdomains , current_slot->domain );
			 
			if( !(domain_elem->unloaded) && 
				class_id && 
				class_id->data.alive  )
			{ 
				(*current_slot).alive = 1;
				//记录此对象的所有引用
				current_slot->ref_start_indx = profiler->references_count;
				//若有构造函数才可统计其refs
				current_slot->refs_count = record_object_references(current_slot);
			}else{
				(*current_slot).alive = 0;
			}
		} else { 
			(*current_slot).alive = 0; 
		}
		 
		current_slot ++;	
		if (current_slot == current_buffer->end_slot) 
		{
			current_buffer = current_buffer->next;
			g_assert (current_buffer != NULL);
			current_slot = current_buffer->start_slot;
		}
	}
	 
}

//查看是否需要创建一个heap_shot_write_job，参考是用户在创建profiler时的启动选项
static inline gboolean
heap_shot_write_job_should_be_created (gboolean dump_heap_data) {
	return dump_heap_data || profiler->action_flags.unreachable_objects || profiler->action_flags.collection_summary;
}

static void
process_gc_event (MonoProfiler *profiler, gboolean do_heap_profiling, MonoGCEvent ev) {
	static gboolean dump_heap_data;
	
	int total_refs_capacity = 0;
	float refs_factor = 1.5f;

	switch (ev) {
	case MONO_GC_EVENT_PRE_STOP_WORLD:
		//此时所有托管线程尚未停止
		LOCK_PROFILER(); 
		//计算堆中所有对象的引用总数
		total_refs_capacity = calc_heap_total_references_buf_size(); 
		total_refs_capacity = (int)(refs_factor*((float)total_refs_capacity)); 
		//确保对象引用缓冲区大小满足
		ensure_references_buf_capacity(total_refs_capacity);
		clear_references_buf();
		break;
	case MONO_GC_EVENT_POST_STOP_WORLD: 
		break;
		 
	case MONO_GC_EVENT_MARK_END:   
		//此阶段Profiler内不可有任何内存分配行为，否则在托管
		//多线程环境下造成死锁 
		//此阶段只标记已在记录缓存中的对象
		profiler_heap_scan(&(profiler->heap)); 
		break;  
	case MONO_GC_EVENT_PRE_START_WORLD:
		break;
	case MONO_GC_EVENT_POST_START_WORLD:
		save_heapshot();
		//TODO:此处进行类信息与对象信息文件写入 
		UNLOCK_PROFILER();
		break; 
	default:
		break;
	}
}

static gboolean 
is_heap_snapshot_request(void)
{
	FILE* pFile = NULL;
	unsigned int hs_request = 0;
	unsigned int reset_request = 0;

	fopen_s(&pFile , "heapshotctrl" ,"rb+");
	if(pFile)
	{
		fread(&hs_request , sizeof(hs_request) , 1 , pFile );
		if( hs_request )
		{
			fseek(pFile,0,SEEK_SET); 
			fwrite(&reset_request,sizeof(reset_request),1,pFile);
		}
		fclose(pFile);

		if(hs_request)
			return TRUE;
		else
			return FALSE;
	}//if(pFile)

	return FALSE;
}

static void 
clear_dead_objects_from_heap( void )
{
	ProfilerHeapShotHeapBuffer *current_buffer = NULL;
	ProfilerHeapObjectInfo* current_slot = NULL;

	current_buffer = profiler->heap.buffers;
	current_slot = current_buffer->start_slot;

	while (current_slot != profiler->heap.first_free_slot) 
	{ 
		if ( (*current_slot).alive == 0 ) 
		{ 
			g_hash_table_remove( profiler->heap_objs_tab ,  (*current_slot).obj );
			*current_slot = profiler_heap_pop_object_from_end( &(profiler->heap) , current_slot );
		} 

		if( current_slot->obj != NULL )
		{ 
			current_slot ++;	
			if (current_slot == current_buffer->end_slot) 
			{
				current_buffer = current_buffer->next;
				g_assert (current_buffer != NULL);
				current_slot = current_buffer->start_slot;
			}
		}
	}
}


static void 
save_heapshot(void)
{
	ProfilerHeapShotWriteJob *job = NULL;
	ProfilerPerThreadData *data = NULL;
	ProfilerStatisticalData *statistical_data = NULL;

	ProfilerHeapShotHeapBuffer *current_buffer = NULL;
	ProfilerHeapObjectInfo* current_slot = NULL;
	
	//由于已经将类信息在Alloced 函数写入主Profiler
	//类记录表，当前直接将其写入文件即可
	write_mapping_block(0);
	 
	if(is_heap_snapshot_request()) 
	{ 
		job = profiler_heap_shot_write_job_new(profiler->heap_shot_was_requested, TRUE, profiler->garbage_collection_counter);

		if( job )
		{ 
			profiler->heap_shot_was_requested = FALSE;
			MONO_PROFILER_GET_CURRENT_COUNTER(job->start_counter);
			MONO_PROFILER_GET_CURRENT_TIME(job->start_time);

			current_buffer = profiler->heap.buffers;
			current_slot = current_buffer->start_slot;
			 
			while (current_slot != profiler->heap.first_free_slot) 
			{
				if ( (*current_slot).alive ) 
				{
					profiler_heap_report_object_reachable( job , current_slot );
				} else { 
					profiler_heap_report_object_unreachable( job , current_slot );  
				} 
				 
				current_slot ++;	
				if (current_slot == current_buffer->end_slot) 
				{
					current_buffer = current_buffer->next;
					g_assert (current_buffer != NULL);
					current_slot = current_buffer->start_slot;
				} 
			}//while (current_slot != profiler->heap.first_free_slot) 


			MONO_PROFILER_GET_CURRENT_COUNTER(job->end_counter);
			MONO_PROFILER_GET_CURRENT_TIME(job->end_time);

			//将heapshotjob加入列表
			profiler_add_heap_shot_write_job(job);
			profiler_free_heap_shot_write_jobs();   
			//写入HeapShotJob
			profiler_process_heap_shot_write_jobs ();
			//flush_everything();
		}
	} 

	clear_dead_objects_from_heap(); 
}

static void
gc_event (MonoProfiler *profiler, MonoGCEvent ev, int generation) {
	ProfilerPerThreadData *data;
	ProfilerEventData *event;
	gboolean do_heap_profiling = profiler->action_flags.unreachable_objects || profiler->action_flags.heap_shot || profiler->action_flags.collection_summary;
	guint32 event_value;
	
	if (ev == MONO_GC_EVENT_START) {
		profiler->garbage_collection_counter ++;
	}
	 
	process_gc_event (profiler, do_heap_profiling, ev);  
}

static void
gc_resize (MonoProfiler *profiler, gint64 new_size) {
	ProfilerPerThreadData *data;
	ProfilerEventData *event;

	GET_PROFILER_THREAD_DATA (data);
	GET_NEXT_FREE_EVENT (data, event);
	profiler->garbage_collection_counter ++;
	STORE_EVENT_NUMBER_VALUE (event, profiler, new_size, MONO_PROFILER_EVENT_DATA_TYPE_OTHER, MONO_PROFILER_EVENT_GC_RESIZE, 0, profiler->garbage_collection_counter);
	COMMIT_RESERVED_EVENTS (data);
}

static void
runtime_initialized (MonoProfiler *profiler) {
	LOG_WRITER_THREAD ("runtime_initialized: initializing internal calls.\n");
	mono_add_internal_call ("Mono.Profiler.RuntimeControls::EnableProfiler", enable_profiler);
	mono_add_internal_call ("Mono.Profiler.RuntimeControls::DisableProfiler", disable_profiler);
	mono_add_internal_call ("Mono.Profiler.RuntimeControls::TakeHeapSnapshot", request_heap_snapshot);
	LOG_WRITER_THREAD ("runtime_initialized: initialized internal calls.\n");
}


#define MAX_COMMAND_LENGTH (1024)
static int server_socket;
static int command_socket;

static void
write_user_response (const char *response) {
	LOG_USER_THREAD ("write_user_response: writing response:");
	LOG_USER_THREAD (response);
	send (command_socket, response, strlen (response), 0);
}

static void
execute_user_command (char *command) {
	char *line_feed;
	
	LOG_USER_THREAD ("execute_user_command: executing command:");
	LOG_USER_THREAD (command);
	
	/* Ignore leading and trailing '\r' */
	line_feed = strchr (command, '\r');
	if (line_feed == command) {
		command ++;
		line_feed = strchr (command, '\r');
	}
	if ((line_feed != NULL) && (* (line_feed + 1) == 0)) {
		*line_feed = 0;
	}
	
	if (strcmp (command, "enable") == 0) {
		LOG_USER_THREAD ("execute_user_command: enabling profiler");
		enable_profiler ();
		write_user_response ("DONE\n");
	} else if (strcmp (command, "disable") == 0) {
		LOG_USER_THREAD ("execute_user_command: disabling profiler");
		disable_profiler ();
		write_user_response ("DONE\n");
	} else if (strcmp (command, "heap-snapshot") == 0) {
		LOG_USER_THREAD ("execute_user_command: taking heap snapshot");
		profiler->heap_shot_was_requested = TRUE;
		WRITER_EVENT_RAISE ();
		write_user_response ("DONE\n");
	} else if (strstr (command, "heap-snapshot-counter") == 0) {
		char *equals; 
		LOG_USER_THREAD ("execute_user_command: changing heap counter");
		equals = strstr (command, "=");
		if (equals != NULL) {
			equals ++;
			if (strcmp (equals, "all") == 0) {
				LOG_USER_THREAD ("execute_user_command: heap counter is \"all\"");
				profiler->garbage_collection_counter = -1;
			} else if (strcmp (equals, "none") == 0) {
				LOG_USER_THREAD ("execute_user_command: heap counter is \"none\"");
				profiler->garbage_collection_counter = 0;
			} else {
				profiler->garbage_collection_counter = atoi (equals);
			}
			write_user_response ("DONE\n");
		} else {
			write_user_response ("ERROR\n");
		}
		profiler->heap_shot_was_requested = TRUE;
	} else {
		LOG_USER_THREAD ("execute_user_command: command not recognized");
		write_user_response ("ERROR\n");
	}
}

static gboolean
process_user_commands (void) {
	char *command_buffer = malloc (MAX_COMMAND_LENGTH);
	int command_buffer_current_index = 0;
	gboolean loop = TRUE;
	gboolean result = TRUE;
	
	while (loop) {
		int unprocessed_characters;
		
		LOG_USER_THREAD ("process_user_commands: reading from socket...");
		unprocessed_characters = recv (command_socket, command_buffer + command_buffer_current_index, MAX_COMMAND_LENGTH - command_buffer_current_index, 0);
		
		if (unprocessed_characters > 0) {
			char *command_end = NULL;
			
			LOG_USER_THREAD ("process_user_commands: received characters.");
			
			do {
				if (command_end != NULL) {
					*command_end = 0;
					execute_user_command (command_buffer);
					unprocessed_characters -= (((command_end - command_buffer) - command_buffer_current_index) + 1);
					
					if (unprocessed_characters > 0) {
						memmove (command_buffer, command_end + 1, unprocessed_characters);
					}
					command_buffer_current_index = 0;
				}
				
				command_end = memchr (command_buffer, '\n', command_buffer_current_index + unprocessed_characters);
			} while (command_end != NULL);
			
			command_buffer_current_index += unprocessed_characters;
			
		} else if (unprocessed_characters == 0) {
			LOG_USER_THREAD ("process_user_commands: received no character.");
			result = TRUE;
			loop = FALSE;
		} else {
			LOG_USER_THREAD ("process_user_commands: received error.");
			result = FALSE;
			loop = FALSE;
		}
	}
	
	free (command_buffer);
	return result;
}

static DWORD WINAPI
user_thread (LPVOID nothing) {
	struct sockaddr_in server_address;
	
	server_socket = -1;
	command_socket = -1;
	
	LOG_USER_THREAD ("user_thread: starting up...");
	
	server_socket = socket (AF_INET, SOCK_STREAM, 0);
	if (server_socket < 0) {
		LOG_USER_THREAD ("user_thread: error creating socket.");
		return 0;
	}
	memset (& server_address, 0, sizeof (server_address));
	
	server_address.sin_family = AF_INET;
	server_address.sin_addr.s_addr = INADDR_ANY;
	if ((profiler->command_port < 1023) || (profiler->command_port > 65535)) {
		LOG_USER_THREAD ("user_thread: invalid port number.");
		return 0;
	}
	server_address.sin_port = htons (profiler->command_port);
	
	if (bind (server_socket, (struct sockaddr *) &server_address, sizeof(server_address)) < 0) {
		LOG_USER_THREAD ("user_thread: error binding socket.");
		close (server_socket);
		return 0;
	}
	
	LOG_USER_THREAD ("user_thread: listening...\n");
	listen (server_socket, 1);
	command_socket = accept (server_socket, NULL, NULL);
	if (command_socket < 0) {
		LOG_USER_THREAD ("user_thread: error accepting socket.");
		close (server_socket);
		return 0;
	}
	
	LOG_USER_THREAD ("user_thread: processing user commands...");
	process_user_commands ();
	
	LOG_USER_THREAD ("user_thread: exiting cleanly.");
	close (server_socket);
	close (command_socket);
	return 0;
}


/* called at the end of the program */
static void
profiler_shutdown (MonoProfiler *prof)
{
	ProfilerPerThreadData* current_thread_data;
	ProfilerPerThreadData* next_thread_data;
	


	LOG_WRITER_THREAD ("profiler_shutdown: zeroing relevant flags");
	mono_profiler_set_events (0);
	//profiler->flags = 0;
	//profiler->action_flags.unreachable_objects = FALSE;
	//profiler->action_flags.heap_shot = FALSE;
	
	LOG_WRITER_THREAD ("profiler_shutdown: asking stats thread to exit");
	profiler->terminate_writer_thread = TRUE;
	WRITER_EVENT_RAISE ();
	LOG_WRITER_THREAD ("profiler_shutdown: waiting for stats thread to exit");
	WAIT_WRITER_THREAD ();
	LOG_WRITER_THREAD ("profiler_shutdown: stats thread should be dead now");
	WRITER_EVENT_DESTROY ();
	
	LOCK_PROFILER ();
	flush_everything ();
	MONO_PROFILER_GET_CURRENT_TIME (profiler->end_time);
	MONO_PROFILER_GET_CURRENT_COUNTER (profiler->end_counter);
	write_end_block ();
 
	mono_profiler_install_code_chunk_new (NULL);
	mono_profiler_install_code_chunk_destroy (NULL);
	mono_profiler_install_code_buffer_new (NULL);
	profiler_code_chunks_cleanup (& (profiler->code_chunks));
	UNLOCK_PROFILER ();
	
	g_free (profiler->file_name);
	if (profiler->file_name_suffix != NULL) {
		g_free (profiler->file_name_suffix);
	}
	
	method_id_mapping_destroy (profiler->methods);
	class_id_mapping_destroy (profiler->classes);
	g_hash_table_destroy (profiler->loaded_assemblies);
	g_hash_table_destroy (profiler->loaded_modules);
	g_hash_table_destroy (profiler->loaded_appdomains); 

	//销毁类信息表
	g_hash_table_destroy(profiler->classinfos);
	
	FREE_PROFILER_THREAD_DATA ();
	
	for (current_thread_data = profiler->per_thread_data; current_thread_data != NULL; current_thread_data = next_thread_data) {
		next_thread_data = current_thread_data->next;
		profiler_per_thread_data_destroy (current_thread_data);
	}
	if (profiler->statistical_data != NULL) {
		profiler_statistical_data_destroy (profiler->statistical_data);
	}
	if (profiler->statistical_data_ready != NULL) {
		profiler_statistical_data_destroy (profiler->statistical_data_ready);
	}
	if (profiler->statistical_data_second_buffer != NULL) {
		profiler_statistical_data_destroy (profiler->statistical_data_second_buffer);
	}
	if (profiler->executable_regions != NULL) {
		profiler_executable_memory_regions_destroy (profiler->executable_regions);
	}
	
	profiler_heap_buffers_free (&(profiler->heap));
	//销毁堆对象信息查找表
	g_hash_table_destroy (profiler->heap_objs_tab);
	//销毁堆对象引用缓冲区
	destroy_references_buf();
	
	profiler_free_write_buffers ();
	profiler_destroy_heap_shot_write_jobs ();
	
	DELETE_PROFILER_MUTEX ();
	
#if (HAS_OPROFILE)
	if (profiler->action_flags.oprofile) {
		op_close_agent ();
	}
#endif
	
	g_free (profiler);
	profiler = NULL;
}

#define FAIL_ARGUMENT_CHECK(message) do {\
	failure_message = (message);\
	goto failure_handling;\
} while (0)
#define FAIL_PARSING_VALUED_ARGUMENT FAIL_ARGUMENT_CHECK("cannot parse valued argument %s")
#define FAIL_PARSING_FLAG_ARGUMENT FAIL_ARGUMENT_CHECK("cannot parse flag argument %s")
#define CHECK_CONDITION(condition,message) do {\
	gboolean result = (condition);\
	if (result) {\
		FAIL_ARGUMENT_CHECK (message);\
	}\
} while (0)
#define FAIL_IF_HAS_MINUS CHECK_CONDITION(has_minus,"minus ('-') modifier not allowed for argument %s")
#define TRUE_IF_NOT_MINUS ((!has_minus)?TRUE:FALSE)

#define DEFAULT_ARGUMENTS "s"
static void
setup_user_options (const char *arguments) {
	gchar **arguments_array, **current_argument;
	detect_fast_timer ();
	
	profiler->file_name = NULL;
	profiler->file_name_suffix = NULL;
	profiler->per_thread_buffer_size = 10000;
	profiler->statistical_buffer_size = 10000;
	profiler->statistical_call_chain_depth = 0;
	profiler->write_buffer_size = 1024;
	profiler->dump_next_heap_snapshots = 0;
	profiler->heap_shot_was_requested = FALSE;

	//初始化指定此profiler接受哪些事件
	profiler->flags = 
			MONO_PROFILE_APPDOMAIN_EVENTS|
			MONO_PROFILE_ASSEMBLY_EVENTS|
			MONO_PROFILE_MODULE_EVENTS|
			MONO_PROFILE_CLASS_EVENTS|
			MONO_PROFILE_METHOD_EVENTS|
			MONO_PROFILE_JIT_COMPILATION;

	//开启profiler
	profiler->profiler_enabled = TRUE;
	
	//若参数为空，则设定为默认参数
	if (arguments == NULL) {
		arguments = DEFAULT_ARGUMENTS;
	} else if (strstr (arguments, ":")) {
		arguments = strstr (arguments, ":") + 1;
		if (arguments [0] == 0) {
			arguments = DEFAULT_ARGUMENTS;
		}
	}
	
	arguments_array = g_strsplit (arguments, ",", -1);
	
	//遍历每一个参数
	for (current_argument = arguments_array; ((current_argument != NULL) && (current_argument [0] != 0)); current_argument ++) {
		char *argument = *current_argument;
		char *equals = strstr (argument, "=");
		const char *failure_message = NULL;
		gboolean has_plus;
		gboolean has_minus;
		
		if (*argument == '+') {
			has_plus = TRUE;
			has_minus = FALSE;
			argument ++;
		} else if (*argument == '-') {
			has_plus = FALSE;
			has_minus = TRUE;
			argument ++;
		} else {
			has_plus = FALSE;
			has_minus = FALSE;
		}
		
		//若参数中存在等式
		if (equals != NULL) {
			int equals_position = equals - argument;
			
			if (! (strncmp (argument, "per-thread-buffer-size", equals_position) && strncmp (argument, "tbs", equals_position))) {
				int value = atoi (equals + 1);
				FAIL_IF_HAS_MINUS;
				if (value > 0) {
					profiler->per_thread_buffer_size = value;
				}
			} else if (! (strncmp (argument, "statistical", equals_position) && strncmp (argument, "stat", equals_position) && strncmp (argument, "s", equals_position))) {
				int value = atoi (equals + 1);
				FAIL_IF_HAS_MINUS;
				if (value > 0) {
					if (value > MAX_STATISTICAL_CALL_CHAIN_DEPTH) {
						value = MAX_STATISTICAL_CALL_CHAIN_DEPTH;
					}
					profiler->statistical_call_chain_depth = value;
					profiler->flags |= MONO_PROFILE_STATISTICAL;
				}
			} else if (! (strncmp (argument, "statistical-thread-buffer-size", equals_position) && strncmp (argument, "sbs", equals_position))) {
				int value = atoi (equals + 1);
				FAIL_IF_HAS_MINUS;
				if (value > 0) {
					profiler->statistical_buffer_size = value;
				}
			} else if (! (strncmp (argument, "write-buffer-size", equals_position) && strncmp (argument, "wbs", equals_position))) {
				int value = atoi (equals + 1);
				FAIL_IF_HAS_MINUS;
				if (value > 0) {
					profiler->write_buffer_size = value;
				}
			} else if (! (strncmp (argument, "output", equals_position) && strncmp (argument, "out", equals_position) && strncmp (argument, "o", equals_position) && strncmp (argument, "O", equals_position))) {
				FAIL_IF_HAS_MINUS;
				if (strlen (equals + 1) > 0) {
					profiler->file_name = g_strdup (equals + 1);
				}
			} else if (! (strncmp (argument, "output-suffix", equals_position) && strncmp (argument, "suffix", equals_position) && strncmp (argument, "os", equals_position) && strncmp (argument, "OS", equals_position))) {
				FAIL_IF_HAS_MINUS;
				if (strlen (equals + 1) > 0) {
					profiler->file_name_suffix = g_strdup (equals + 1);
				}
			} else if (! (strncmp (argument, "heap-shot", equals_position) && strncmp (argument, "heap", equals_position) && strncmp (argument, "h", equals_position))) {
				char *parameter = equals + 1;
				if (! strcmp (parameter, "all")) {
					profiler->dump_next_heap_snapshots = -1;
				} else {
					profiler->dump_next_heap_snapshots = atoi (parameter);
				}
				FAIL_IF_HAS_MINUS;
				if (! has_plus) {
					profiler->action_flags.save_allocation_caller = TRUE;
					profiler->action_flags.save_allocation_stack = TRUE;
					profiler->action_flags.allocations_carry_id = TRUE_IF_NOT_MINUS;
				}
				profiler->action_flags.heap_shot = TRUE_IF_NOT_MINUS;
			} else if (! (strncmp (argument, "gc-dumps", equals_position) && strncmp (argument, "gc-d", equals_position) && strncmp (argument, "gcd", equals_position))) {
				FAIL_IF_HAS_MINUS;
				if (strlen (equals + 1) > 0) {
					profiler->dump_next_heap_snapshots = atoi (equals + 1);
				}
			} else if (! (strncmp (argument, "command-port", equals_position) && strncmp (argument, "cp", equals_position))) {
				FAIL_IF_HAS_MINUS;
				if (strlen (equals + 1) > 0) {
					profiler->command_port = atoi (equals + 1);
				}
			} else {
				FAIL_PARSING_VALUED_ARGUMENT;
			}
		} else {//若当前解析参数不存在等式

			if (! (strcmp (argument, "jit") && strcmp (argument, "j"))) {
				profiler->action_flags.jit_time = TRUE_IF_NOT_MINUS;
			} else if (! (strcmp (argument, "allocations") && strcmp (argument, "alloc") && strcmp (argument, "a"))) {
				FAIL_IF_HAS_MINUS;
				if (! has_plus) {
					profiler->action_flags.save_allocation_caller = TRUE;
					profiler->action_flags.save_allocation_stack = TRUE;
				}
				if (! has_minus) {
					profiler->flags |= MONO_PROFILE_ALLOCATIONS;
				} else {
					profiler->flags &= ~MONO_PROFILE_ALLOCATIONS;
				}
			} else if (! (strcmp (argument, "monitor") && strcmp (argument, "locks") && strcmp (argument, "lock"))) {
				FAIL_IF_HAS_MINUS;
				profiler->action_flags.track_stack = TRUE;
				profiler->flags |= MONO_PROFILE_MONITOR_EVENTS;
				profiler->flags |= MONO_PROFILE_GC;
			} else if (! (strcmp (argument, "gc") && strcmp (argument, "g"))) {
				FAIL_IF_HAS_MINUS;
				profiler->action_flags.report_gc_events = TRUE;
				profiler->flags |= MONO_PROFILE_GC;
			} else if (! (strcmp (argument, "allocations-summary") && strcmp (argument, "as"))) {
				profiler->action_flags.collection_summary = TRUE_IF_NOT_MINUS;
			} else if (! (strcmp (argument, "heap-shot") && strcmp (argument, "heap") && strcmp (argument, "h"))) {
				FAIL_IF_HAS_MINUS;
				if (! has_plus) {
					profiler->action_flags.save_allocation_caller = TRUE;
					profiler->action_flags.save_allocation_stack = TRUE;
					profiler->action_flags.allocations_carry_id = TRUE_IF_NOT_MINUS;
				}
				profiler->action_flags.heap_shot = TRUE_IF_NOT_MINUS;
			} else if (! (strcmp (argument, "unreachable") && strcmp (argument, "free") && strcmp (argument, "f"))) {
				profiler->action_flags.unreachable_objects = TRUE_IF_NOT_MINUS;
			} else if (! (strcmp (argument, "threads") && strcmp (argument, "t"))) {
				if (! has_minus) {
					profiler->flags |= MONO_PROFILE_THREADS;
				} else {
					profiler->flags &= ~MONO_PROFILE_THREADS;
				}
			} else if (! (strcmp (argument, "enter-leave") && strcmp (argument, "calls") && strcmp (argument, "c"))) {
				profiler->action_flags.track_calls = TRUE_IF_NOT_MINUS;
			} else if (! (strcmp (argument, "statistical") && strcmp (argument, "stat") && strcmp (argument, "s"))) {
				if (! has_minus) {
					profiler->flags |= MONO_PROFILE_STATISTICAL;
				} else {
					profiler->flags &= ~MONO_PROFILE_STATISTICAL;
				}
			} else if (! (strcmp (argument, "save-allocation-caller") && strcmp (argument, "sac"))) {
				profiler->action_flags.save_allocation_caller = TRUE_IF_NOT_MINUS;
			} else if (! (strcmp (argument, "save-allocation-stack") && strcmp (argument, "sas"))) {
				profiler->action_flags.save_allocation_stack = TRUE_IF_NOT_MINUS;
			} else if (! (strcmp (argument, "allocations-carry-id") && strcmp (argument, "aci"))) {
				profiler->action_flags.allocations_carry_id = TRUE_IF_NOT_MINUS;
			} else if (! (strcmp (argument, "start-enabled") && strcmp (argument, "se"))) {
				profiler->profiler_enabled = TRUE_IF_NOT_MINUS;
			} else if (! (strcmp (argument, "start-disabled") && strcmp (argument, "sd"))) {
				profiler->profiler_enabled = ! TRUE_IF_NOT_MINUS;
			} else if (! (strcmp (argument, "force-accurate-timer") && strcmp (argument, "fac"))) {
				use_fast_timer = TRUE_IF_NOT_MINUS;
#if (HAS_OPROFILE)
			} else if (! (strcmp (argument, "oprofile") && strcmp (argument, "oprof"))) {
				profiler->flags |= MONO_PROFILE_JIT_COMPILATION;
				profiler->action_flags.oprofile = TRUE;
				if (op_open_agent ()) {
					FAIL_ARGUMENT_CHECK ("problem calling op_open_agent");
				}
#endif
			} else if (strcmp (argument, "logging")) {
				FAIL_PARSING_FLAG_ARGUMENT;
			}
		}//end of if (equals != NULL) 
		
failure_handling:
		if (failure_message != NULL) {
			g_warning (failure_message, argument);
			failure_message = NULL;
		}


	}//for (current_argument = arguments_array; ((current_argument != NULL) && (current_argument [0] != 0)); current_argument ++)
	
	g_free (arguments_array);
	

	//根据用户的参数选择设置指定的接收标志位

	/* Ensure that the profiler flags needed to support required action flags are active */
	if (profiler->action_flags.jit_time) {
		profiler->flags |= MONO_PROFILE_JIT_COMPILATION;
	}
	if (profiler->action_flags.save_allocation_caller || profiler->action_flags.save_allocation_stack || profiler->action_flags.allocations_carry_id) {
		profiler->flags |= MONO_PROFILE_ALLOCATIONS;
	}
	if (profiler->action_flags.collection_summary || profiler->action_flags.heap_shot || profiler->action_flags.unreachable_objects) {
		profiler->flags |= MONO_PROFILE_ALLOCATIONS;
		profiler->action_flags.report_gc_events = TRUE;
	}
	if (profiler->action_flags.track_calls) {
		profiler->flags |= MONO_PROFILE_ENTER_LEAVE;
		profiler->action_flags.jit_time = TRUE;
	}
	if (profiler->action_flags.save_allocation_caller || profiler->action_flags.save_allocation_stack) {
		profiler->action_flags.track_stack = TRUE;
		profiler->flags |= MONO_PROFILE_ENTER_LEAVE;
	}
	if (profiler->action_flags.track_stack) {
		profiler->flags |= MONO_PROFILE_ENTER_LEAVE;
	}
	
	/* Tracking call stacks is useless if we already emit all enter-exit events... */
	if (profiler->action_flags.track_calls) {
		profiler->action_flags.track_stack = FALSE;
		profiler->action_flags.save_allocation_caller = FALSE;
		profiler->action_flags.save_allocation_stack = FALSE;
	}
	
	/* Without JIT events the stat profiler will not find method IDs... */
	if (profiler->flags | MONO_PROFILE_STATISTICAL) {
		profiler->flags |= MONO_PROFILE_JIT_COMPILATION;
	}
	/* Profiling allocations without knowing which gc we are doing is not nice... */
	if (profiler->flags | MONO_PROFILE_ALLOCATIONS) {
		profiler->flags |= MONO_PROFILE_GC;
		profiler->action_flags.report_gc_events = TRUE;
	}

	{//以当前系统时间为文件名
		SYSTEMTIME systime;
		char heapshot_file_dir[2048] = {0};
		GetLocalTime( &systime );
		GetCurrentDirectoryA( sizeof(heapshot_file_dir) , heapshot_file_dir );
		profiler->file_name = g_strdup_printf ("%s\\HeapShot_%d-%d-%d_%d-%d-%d-%d.mprof", heapshot_file_dir ,
			systime.wYear , systime.wMonth , systime.wDay , systime.wHour , systime.wMinute , systime.wSecond , systime.wMilliseconds);
	}
	
	//if (profiler->file_name == NULL) {
	//	char *program_name = g_get_prgname ();
	//	
	//	if (program_name != NULL) {
	//		char *name_buffer = g_strdup (program_name);
	//		char *name_start = name_buffer;
	//		char *cursor;
	//		
	//		/* Jump over the last '/' */
	//		cursor = strrchr (name_buffer, '/');
	//		if (cursor == NULL) {
	//			cursor = name_buffer;
	//		} else {
	//			cursor ++;
	//		}
	//		name_start = cursor;
	//		
	//		/* Then jump over the last '\\' */
	//		cursor = strrchr (name_start, '\\');
	//		if (cursor == NULL) {
	//			cursor = name_start;
	//		} else {
	//			cursor ++;
	//		}
	//		name_start = cursor;
	//		
	//		/* Finally, find the last '.' */
	//		cursor = strrchr (name_start, '.');
	//		if (cursor != NULL) {
	//			*cursor = 0;
	//		} 
	//		if (profiler->file_name_suffix == NULL) {
	//			profiler->file_name = g_strdup_printf ("%s.mprof", name_start);
	//		} else {
	//			profiler->file_name = g_strdup_printf ("%s-%s.mprof", name_start, profiler->file_name_suffix);
	//		}
	//		g_free (name_buffer);
	//	} else {
	//		profiler->file_name = g_strdup_printf ("%s.mprof", "profiler-log");
	//	}
	//}
}
//end of setup_user_options

static  DWORD WINAPI
data_writer_thread (LPVOID nothing) {
	for (;;) {
		ProfilerStatisticalData *statistical_data;
		gboolean done; 

		//等待 profiler->wake_data_writer_event  事件
		WRITER_EVENT_WAIT (); 
		
		//请求heapshot? 
		if (profiler->heap_shot_was_requested)
		{ 
			//获得根domain，什么是domain?为何与线程有关？
			MonoDomain * root_domain = mono_get_root_domain ();
			
			//若根domain可用
			if (root_domain != NULL) 
			{
				MonoThread *this_thread; 
				//将根domain附加到当前线程?
				this_thread = mono_thread_attach (root_domain); 
				//全局GC
				mono_gc_collect (mono_gc_max_generation ()); 
				//将domain抽离当前线程
				mono_thread_detach (this_thread);
				this_thread = NULL; 
			}  
		}
		
		statistical_data = profiler->statistical_data_ready;
		done = (statistical_data == NULL) && (profiler->heap_shot_write_jobs == NULL);
		
		if (!done) { 
			//锁住全局profiler数据结构
			LOCK_PROFILER ();
			
			// This makes sure that all method ids are in place 
			flush_all_mappings (); 
			
			if (statistical_data != NULL) 
			{
				LOG_WRITER_THREAD ("data_writer_thread: writing statistical data...");
				profiler->statistical_data_ready = NULL;
				write_statistical_data_block (statistical_data);
				statistical_data->next_free_index = 0;
				statistical_data->first_unwritten_index = 0;
				profiler->statistical_data_second_buffer = statistical_data;
				LOG_WRITER_THREAD ("data_writer_thread: wrote statistical data");
			}
			
			profiler_process_heap_shot_write_jobs ();
			
			UNLOCK_PROFILER (); 
		} else { 
			LOCK_PROFILER (); 
			flush_everything ();
			UNLOCK_PROFILER (); 
		}
		
		if (profiler->terminate_writer_thread) 
		{ 
			CLEANUP_WRITER_THREAD ();
			EXIT_THREAD ();
		}

	}// end of for(;;)
	return 0;
}

void
mono_profiler_startup (const char *desc);

/* the entry point (mono_profiler_load?) */
void
mono_profiler_startup (const char *desc)
{
	//初始化全局Profiler数据，此结构是此文件中的
	//_MonoProfiler结构体。
	profiler = g_new0 (MonoProfiler, 1);
	
	//根据用户输入参数，配置profiler结构
	setup_user_options ((desc != NULL) ? desc : DEFAULT_ARGUMENTS);
	
	//初始化 profiler的数据锁
	INITIALIZE_PROFILER_MUTEX ();

	//初始化 profiler的启动时间点
	MONO_PROFILER_GET_CURRENT_TIME (profiler->start_time);
	MONO_PROFILER_GET_CURRENT_COUNTER (profiler->start_counter);
	profiler->last_header_counter = 0;
	
	//初始化方法映射查找表
	profiler->methods = method_id_mapping_new ();
	//初始化类映射查找表
	profiler->classes = class_id_mapping_new ();
	profiler->loaded_element_next_free_id = 1;
	profiler->loaded_assemblies = g_hash_table_new_full (g_direct_hash, NULL, NULL, loaded_element_destroy);
	profiler->loaded_modules = g_hash_table_new_full (g_direct_hash, NULL, NULL, loaded_element_destroy);
	profiler->loaded_appdomains = g_hash_table_new_full (g_direct_hash, NULL, NULL, loaded_element_destroy);
	
	//初始化类信息查找表
	profiler->classinfos = g_hash_table_new_full(g_direct_hash,NULL,NULL,class_info_destroy);
	
	profiler->statistical_data = profiler_statistical_data_new (profiler);
	profiler->statistical_data_second_buffer = profiler_statistical_data_new (profiler);
	
	//分配日志缓存
	profiler->write_buffers = g_malloc (sizeof (ProfilerFileWriteBuffer) + PROFILER_FILE_WRITE_BUFFER_SIZE);
	profiler->write_buffers->next = NULL;
	profiler->current_write_buffer = profiler->write_buffers;
	profiler->current_write_position = 0;
	profiler->full_write_buffers = 0;
	profiler_code_chunks_initialize (& (profiler->code_chunks));
	
	profiler->executable_regions = profiler_executable_memory_regions_new (1, 1);
	
	profiler->executable_files.table = g_hash_table_new (g_str_hash, g_str_equal); 
	profiler->executable_files.new_files = NULL; 
	
	profiler->heap_shot_write_jobs = NULL;
	if (profiler->action_flags.unreachable_objects || profiler->action_flags.heap_shot || profiler->action_flags.collection_summary) {
		profiler_heap_buffers_setup (&(profiler->heap));
		profiler->heap_objs_tab = g_hash_table_new_full (g_direct_hash, NULL, NULL, heap_object_info_element_destroy);
	} else {
		profiler_heap_buffers_clear (&(profiler->heap));
		profiler->heap_objs_tab = NULL;
	}
	profiler->garbage_collection_counter = 0;
	//初始化引用缓存
	ensure_references_buf_capacity(PROFILER_REFERENCES_BUF_INIT_SIZE);
	
	WRITER_EVENT_INIT();

	//创建数据写线程？详细看data_writer_thread。
	//CREATE_WRITER_THREAD (data_writer_thread); 
	if ((profiler->command_port >= 1024) && (profiler->command_port <= 65535)) {
		//创建用户线程,详细看user_thread。
		CREATE_USER_THREAD (user_thread);
	} else {

	}

	//分配tls槽位
	ALLOCATE_PROFILER_THREAD_DATA ();

	OPEN_FILE ();
	CLOSE_FILE();

	write_intro_block ();
	write_directives_block (TRUE);
	
	mono_profiler_install (profiler, profiler_shutdown);
	
	mono_profiler_install_appdomain (appdomain_start_load, appdomain_end_load,
		appdomain_start_unload, appdomain_end_unload);
	mono_profiler_install_assembly (assembly_start_load, assembly_end_load,
		assembly_start_unload, assembly_end_unload);
	//mono_profiler_install_module (module_start_load, module_end_load,
	//		module_start_unload, module_end_unload);
	mono_profiler_install_class (class_start_load, class_end_load,
			class_start_unload, class_end_unload);
	//mono_profiler_install_jit_compile (method_start_jit, method_end_jit);
	//mono_profiler_install_enter_leave (method_enter, method_leave);
	//mono_profiler_install_method_free (method_free);
	//mono_profiler_install_thread (thread_start, thread_end);
	mono_profiler_install_allocation (object_allocated);
	//mono_profiler_install_monitor (monitor_event);
	//mono_profiler_install_statistical (statistical_hit);
	//mono_profiler_install_statistical_call_chain (statistical_call_chain, profiler->statistical_call_chain_depth, MONO_PROFILER_CALL_CHAIN_MANAGED);
	mono_profiler_install_gc (gc_event, gc_resize);
	mono_profiler_install_runtime_initialized (runtime_initialized);
#if (HAS_OPROFILE)
	mono_profiler_install_jit_end (method_jit_result);
#endif
	//if (profiler->flags | MONO_PROFILE_STATISTICAL) {
	//	mono_profiler_install_code_chunk_new (profiler_code_chunk_new_callback);
	//	mono_profiler_install_code_chunk_destroy (profiler_code_chunk_destroy_callback);
	//	mono_profiler_install_code_buffer_new (profiler_code_buffer_new_callback);
	//}
	
	mono_profiler_set_events (profiler->flags);
}


void mono_profiler_logging_load( const char* desc )
{
	mono_gc_base_init (); 

	if( !desc || (strcmp("logging",desc) == 0) || (strncmp(desc,"logging:" , 8) == 0))
	{
		 mono_profiler_startup( desc );
	}
}
