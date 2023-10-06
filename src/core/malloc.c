/*
 * Copyright (C) 2006 Michael Brown <mbrown@fensystems.co.uk>.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 * You can also choose to distribute this program under the terms of
 * the Unmodified Binary Distribution Licence (as given in the file
 * COPYING.UBDL), provided that you have satisfied its requirements.
 */

FILE_LICENCE ( GPL2_OR_LATER_OR_UBDL );

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <ipxe/io.h>
#include <ipxe/list.h>
#include <ipxe/init.h>
#include <ipxe/refcnt.h>
#include <ipxe/malloc.h>
#include <valgrind/memcheck.h>

#include <ipxe/efi/efi.h>
#include <ipxe/efi/Library/BaseLib.h>

/** @file
 *
 * Dynamic memory allocation
 *
 */

/** A free block of memory */
struct memory_block {
	/** Size of this block */
	size_t size;
	/** Padding
	 *
	 * This padding exists to cover the "count" field of a
	 * reference counter, in the common case where a reference
	 * counter is the first element of a dynamically-allocated
	 * object.  It avoids clobbering the "count" field as soon as
	 * the memory is freed, and so allows for the possibility of
	 * detecting reference counting errors.
	 */
	char pad[ offsetof ( struct refcnt, count ) +
		  sizeof ( ( ( struct refcnt * ) NULL )->count ) ];
	/** List of free blocks */
	struct list_head list;
};

#define MIN_MEMBLOCK_SIZE \
	( ( size_t ) ( 1 << ( fls ( sizeof ( struct memory_block ) - 1 ) ) ) )

/** A block of allocated memory complete with size information */
struct autosized_block {
	/** Size of this block */
	size_t size;
	/** Remaining data */
	char data[0];
};

/**
 * Address for zero-length memory blocks
 *
 * @c malloc(0) or @c realloc(ptr,0) will return the special value @c
 * NOWHERE.  Calling @c free(NOWHERE) will have no effect.
 *
 * This is consistent with the ANSI C standards, which state that
 * "either NULL or a pointer suitable to be passed to free()" must be
 * returned in these cases.  Using a special non-NULL value means that
 * the caller can take a NULL return value to indicate failure,
 * without first having to check for a requested size of zero.
 *
 * Code outside of malloc.c do not ever need to refer to the actual
 * value of @c NOWHERE; this is an internal definition.
 */
#define NOWHERE ( ( void * ) ~( ( intptr_t ) 0 ) )

/** List of free memory blocks */
static LIST_HEAD ( free_blocks );

/** Total amount of free memory */
size_t freemem;

/** Total amount of used memory */
size_t usedmem;

/** Maximum amount of used memory */
size_t maxusedmem;

/**
 * Heap size
 *
 * Currently fixed at 512kB.
 */
#define HEAP_SIZE ( 512 * 1024 )

/** The heap itself */
static char heap[HEAP_SIZE] __attribute__ (( aligned ( __alignof__(void *) )));

/**
 * Mark all blocks in free list as defined
 *
 */
static inline void valgrind_make_blocks_defined ( void ) {
	struct memory_block *block;

	/* Do nothing unless running under Valgrind */
	if ( RUNNING_ON_VALGRIND <= 0 )
		return;

	/* Traverse free block list, marking each block structure as
	 * defined.  Some contortions are necessary to avoid errors
	 * from list_check().
	 */

	/* Mark block list itself as defined */
	VALGRIND_MAKE_MEM_DEFINED ( &free_blocks, sizeof ( free_blocks ) );

	/* Mark areas accessed by list_check() as defined */
	VALGRIND_MAKE_MEM_DEFINED ( &free_blocks.prev->next,
				    sizeof ( free_blocks.prev->next ) );
	VALGRIND_MAKE_MEM_DEFINED ( free_blocks.next,
				    sizeof ( *free_blocks.next ) );
	VALGRIND_MAKE_MEM_DEFINED ( &free_blocks.next->next->prev,
				    sizeof ( free_blocks.next->next->prev ) );

	/* Mark each block in list as defined */
	list_for_each_entry ( block, &free_blocks, list ) {

		/* Mark block as defined */
		VALGRIND_MAKE_MEM_DEFINED ( block, sizeof ( *block ) );

		/* Mark areas accessed by list_check() as defined */
		VALGRIND_MAKE_MEM_DEFINED ( block->list.next,
					    sizeof ( *block->list.next ) );
		VALGRIND_MAKE_MEM_DEFINED ( &block->list.next->next->prev,
				      sizeof ( block->list.next->next->prev ) );
	}
}

/**
 * Mark all blocks in free list as inaccessible
 *
 */
static inline void valgrind_make_blocks_noaccess ( void ) {
	struct memory_block *block;
	struct memory_block *prev = NULL;

	/* Do nothing unless running under Valgrind */
	if ( RUNNING_ON_VALGRIND <= 0 )
		return;

	/* Traverse free block list, marking each block structure as
	 * inaccessible.  Some contortions are necessary to avoid
	 * errors from list_check().
	 */

	/* Mark each block in list as inaccessible */
	list_for_each_entry ( block, &free_blocks, list ) {

		/* Mark previous block (if any) as inaccessible. (Current
		 * block will be accessed by list_check().)
		 */
		if ( prev )
			VALGRIND_MAKE_MEM_NOACCESS ( prev, sizeof ( *prev ) );
		prev = block;

		/* At the end of the list, list_check() will end up
		 * accessing the first list item.  Temporarily mark
		 * this area as defined.
		 */
		VALGRIND_MAKE_MEM_DEFINED ( &free_blocks.next->prev,
					    sizeof ( free_blocks.next->prev ) );
	}
	/* Mark last block (if any) as inaccessible */
	if ( prev )
		VALGRIND_MAKE_MEM_NOACCESS ( prev, sizeof ( *prev ) );

	/* Mark as inaccessible the area that was temporarily marked
	 * as defined to avoid errors from list_check().
	 */
	VALGRIND_MAKE_MEM_NOACCESS ( &free_blocks.next->prev,
				     sizeof ( free_blocks.next->prev ) );

	/* Mark block list itself as inaccessible */
	VALGRIND_MAKE_MEM_NOACCESS ( &free_blocks, sizeof ( free_blocks ) );
}

/**
 * Check integrity of the blocks in the free list
 *
 */
static inline void check_blocks ( void ) {
	struct memory_block *block;
	struct memory_block *prev = NULL;

	if ( ! ASSERTING )
		return;

	list_for_each_entry ( block, &free_blocks, list ) {

		/* Check that list structure is intact */
		list_check ( &block->list );

		/* Check that block size is not too small */
		assert ( block->size >= sizeof ( *block ) );
		assert ( block->size >= MIN_MEMBLOCK_SIZE );

		/* Check that block does not wrap beyond end of address space */
		assert ( ( ( void * ) block + block->size ) >
			 ( ( void * ) block ) );

		/* Check that blocks remain in ascending order, and
		 * that adjacent blocks have been merged.
		 */
		if ( prev ) {
			assert ( ( ( void * ) block ) > ( ( void * ) prev ) );
			assert ( ( ( void * ) block ) >
				 ( ( ( void * ) prev ) + prev->size ) );
		}
		prev = block;
	}
}

/**
 * Discard some cached data
 *
 * @ret discarded	Number of cached items discarded
 */
static unsigned int discard_cache ( void ) {
	struct cache_discarder *discarder;
	unsigned int discarded;

	for_each_table_entry ( discarder, CACHE_DISCARDERS ) {
		discarded = discarder->discard();
		if ( discarded )
			return discarded;
	}
	return 0;
}

/**
 * Discard all cached data
 *
 */
static void discard_all_cache ( void ) {
	unsigned int discarded;

	do {
		discarded = discard_cache();
	} while ( discarded );
}

volatile unsigned char loop = 1;

/**
 * Allocate a memory block
 *
 * @v size		Requested size
 * @v align		Physical alignment
 * @v offset		Offset from physical alignment
 * @ret ptr		Memory block, or NULL
 *
 * Allocates a memory block @b physically aligned as requested.  No
 * guarantees are provided for the alignment of the virtual address.
 *
 * @c align must be a power of two.  @c size may not be zero.
 */
void * alloc_memblock ( size_t size, size_t align, size_t offset ) {
	EFI_BOOT_SERVICES *bs = efi_systab->BootServices;
	EFI_STATUS efirc;
	void *ptr;
	void *newptr;
	uint32_t stub;

	efirc = bs->AllocatePool ( EfiBootServicesData, size + align + sizeof(stub),
					  &ptr );
	if (efirc != 0) {
		while (loop) {}
		return NULL;
	}
	if ((align != 0) && ((uint64_t)ptr & (align - 1)) != 0) {
		newptr = (void*)(((uint64_t)ptr + sizeof (stub) + align - 1) & (~(align - 1)));
		stub = (uint64_t)newptr - (uint64_t)ptr;
		memcpy ((uint8_t*)newptr-4, &stub, sizeof(stub));
		ptr = newptr;
	}
	if ((size + align) > 0x1000) {
		while (loop) {}
	}
	if (offset) {
		while (loop) {}
		ptr = (void*)((uint64_t)ptr + offset);
	}
	if (ptr == NULL) {
		while (loop) {}
	}
	return ptr;
}

/**
 * Free a memory block
 *
 * @v ptr		Memory allocated by alloc_memblock(), or NULL
 * @v size		Size of the memory
 *
 * If @c ptr is NULL, no action is taken.
 */
void free_memblock ( void *ptr, size_t size ) {
	EFI_BOOT_SERVICES *bs = efi_systab->BootServices;
	EFI_STATUS efirc;
	uint32_t stub;
	void *real_ptr;
	if (size == 0) {
		assert (FALSE);
while (loop) {}
	}
	memcpy (&stub, (uint8_t*)ptr-4, sizeof(stub));
	real_ptr = (uint8_t*)ptr - stub;
	efirc = bs->FreePool ( real_ptr );
	if (efirc != 0) {
		assert (FALSE);
while (loop) {}
	}
}

typedef struct {
  UINT32             Signature;
  UINT32             Reserved;
  EFI_MEMORY_TYPE    Type;
  UINTN              Size;
  CHAR8              Data[1];
} POOL_HEAD;
#define POOL_HEAD_SIGNATURE      SIGNATURE_32('p','h','d','0')
#define POOLPAGE_HEAD_SIGNATURE  SIGNATURE_32('p','h','d','1')
#define SIZE_OF_POOL_HEAD  OFFSET_OF(POOL_HEAD,Data)

#define POOL_TAIL_SIGNATURE  SIGNATURE_32('p','t','a','l')
typedef struct {
  UINT32    Signature;
  UINT32    Reserved;
  UINTN     Size;
} POOL_TAIL;

#define POOL_OVERHEAD  (SIZE_OF_POOL_HEAD + sizeof(POOL_TAIL))
/**
 * Reallocate memory
 *
 * @v old_ptr		Memory previously allocated by malloc(), or NULL
 * @v new_size		Requested size
 * @ret new_ptr		Allocated memory, or NULL
 *
 * Allocates memory with no particular alignment requirement.  @c
 * new_ptr will be aligned to at least a multiple of sizeof(void*).
 * If @c old_ptr is non-NULL, then the contents of the newly allocated
 * memory will be the same as the contents of the previously allocated
 * memory, up to the minimum of the old and new sizes.  The old memory
 * will be freed.
 *
 * If allocation fails the previously allocated block is left
 * untouched and NULL is returned.
 *
 * Calling realloc() with a new size of zero is a valid way to free a
 * memory block.
 */
void * realloc ( void *old_ptr, size_t new_size ) {
	EFI_BOOT_SERVICES *bs = efi_systab->BootServices;
	EFI_STATUS efirc;
	void *new_ptr;
  POOL_HEAD  *Head;

	if (new_size == 0 && old_ptr == NULL) {
		return NULL;
	}

	if (new_size == 0) {
		// This is a free request
		efirc = bs->FreePool ( old_ptr );
		return NULL;
	}

	// Some real deal?
	efirc = bs->AllocatePool ( EfiBootServicesData, new_size, &new_ptr );
	if (efirc != 0) {
		assert (FALSE);
while (loop) {}
		return NULL;
	}

	if (old_ptr == NULL) {
		// Just wanted some allocation, so we do that
		memset ( new_ptr, 0, new_size );
		return new_ptr;
	}

	// Well, really want to reallocate...
	Head = BASE_CR (old_ptr, POOL_HEAD, Data);
	if ((Head->Signature != POOL_HEAD_SIGNATURE) && (Head->Signature != POOLPAGE_HEAD_SIGNATURE)) {
		while (loop) {}
	}
	if ((Head->Size - POOL_OVERHEAD) < new_size) {
		memcpy ( new_ptr, old_ptr, (Head->Size - POOL_OVERHEAD) );
	} else {
		memcpy ( new_ptr, old_ptr, new_size );
	}

	efirc = bs->FreePool ( old_ptr );
	if (efirc != 0) {
		assert (FALSE);
while (loop) {}
		return new_ptr;
	}

	return new_ptr;
}

/**
 * Allocate memory
 *
 * @v size		Requested size
 * @ret ptr		Memory, or NULL
 *
 * Allocates memory with no particular alignment requirement.  @c ptr
 * will be aligned to at least a multiple of sizeof(void*).
 */
void * malloc ( size_t size ) {
	void *ptr;

	ptr = realloc ( NULL, size );
	return ptr;
}

/**
 * Free memory
 *
 * @v ptr		Memory allocated by malloc(), or NULL
 *
 * Memory allocated with malloc_dma() cannot be freed with free(); it
 * must be freed with free_dma() instead.
 *
 * If @c ptr is NULL, no action is taken.
 */
void free ( void *ptr ) {

	realloc ( ptr, 0 );
}

/**
 * Allocate cleared memory
 *
 * @v size		Requested size
 * @ret ptr		Allocated memory
 *
 * Allocate memory as per malloc(), and zero it.
 *
 * This function name is non-standard, but pretty intuitive.
 * zalloc(size) is always equivalent to calloc(1,size)
 */
void * zalloc ( size_t size ) {
	void *data;

	data = malloc ( size );
	if ( data )
		memset ( data, 0, size );

	return data;
}

/**
 * Add memory to allocation pool
 *
 * @v start		Start address
 * @v end		End address
 *
 * Adds a block of memory [start,end) to the allocation pool.  This is
 * a one-way operation; there is no way to reclaim this memory.
 *
 * @c start must be aligned to at least a multiple of sizeof(void*).
 */
void mpopulate ( void *start, size_t len ) {

	/* Prevent free_memblock() from rounding up len beyond the end
	 * of what we were actually given...
	 */
	len &= ~( MIN_MEMBLOCK_SIZE - 1 );

	/* Add to allocation pool */
	free_memblock ( start, len );

	/* Fix up memory usage statistics */
	usedmem += len;
}

/**
 * Initialise the heap
 *
 */
static void init_heap ( void ) {
	VALGRIND_MAKE_MEM_NOACCESS ( heap, sizeof ( heap ) );
	VALGRIND_MAKE_MEM_NOACCESS ( &free_blocks, sizeof ( free_blocks ) );
	// mpopulate ( heap, sizeof ( heap ) );
}

/** Memory allocator initialisation function */
struct init_fn heap_init_fn __init_fn ( INIT_EARLY ) = {
	.initialise = init_heap,
};

/**
 * Discard all cached data on shutdown
 *
 */
static void shutdown_cache ( int booting __unused ) {
	discard_all_cache();
	DBGC ( &heap, "Maximum heap usage %zdkB\n", ( maxusedmem >> 10 ) );
}

/** Memory allocator shutdown function */
struct startup_fn heap_startup_fn __startup_fn ( STARTUP_EARLY ) = {
	.name = "heap",
	.shutdown = shutdown_cache,
};

#if 0
#include <stdio.h>
/**
 * Dump free block list
 *
 */
void mdumpfree ( void ) {
	struct memory_block *block;

	printf ( "Free block list:\n" );
	list_for_each_entry ( block, &free_blocks, list ) {
		printf ( "[%p,%p] (size %#zx)\n", block,
			 ( ( ( void * ) block ) + block->size ), block->size );
	}
}
#endif
