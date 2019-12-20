// Copyright (c) 2016-2019 The Regents of the University of Michigan
// This file is part of the General Simulation Data (GSD) project, released under the BSD 2-Clause
// License.

#include <sys/stat.h>
#ifdef _WIN32

#define GSD_USE_MMAP 0
#include <io.h>

#else // linux / mac

#define _XOPEN_SOURCE 500
#include <sys/mman.h>
#include <unistd.h>
#define GSD_USE_MMAP 1

#endif

#ifdef __APPLE__
#include <limits.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "gsd.h"

/** @file gsd.c
    @brief Implements the GSD C API
*/

/// Magic value identifying a GSD file
const uint64_t GSD_MAGIC_ID = 0x65DF65DF65DF65DF;

/// Initial index size
enum
{
    GSD_INITIAL_INDEX_SIZE = 128
};

/// Initial namelist size
enum
{
    GSD_INITIAL_NAMELIST_SIZE = 65535
};

/// Size of the temporary copy buffer
enum
{
    GSD_COPY_BUFFER_SIZE = 1024 * 16
};

// define windows wrapper functions
#ifdef _WIN32
#define lseek _lseeki64
#define open _open
#define ftruncate _chsize
#define fsync _commit
typedef int64_t ssize_t;

int S_IRUSR = _S_IREAD;
int S_IWUSR = _S_IWRITE;
int S_IRGRP = _S_IREAD;
int S_IWGRP = _S_IWRITE;

ssize_t pread(int fd, void* buf, size_t count, int64_t offset)
{
    // Note: _read only accepts unsigned int values
    if (count > UINT_MAX)
        return GSD_ERROR_IO;

    int64_t oldpos = _telli64(fd);
    _lseeki64(fd, offset, SEEK_SET);
    ssize_t result = _read(fd, buf, (unsigned int)count);
    _lseeki64(fd, oldpos, SEEK_SET);
    return result;
}

ssize_t pwrite(int fd, const void* buf, size_t count, int64_t offset)
{
    // Note: _write only accepts unsigned int values
    if (count > UINT_MAX)
        return GSD_ERROR_IO;

    int64_t oldpos = _telli64(fd);
    _lseeki64(fd, offset, SEEK_SET);
    ssize_t result = _write(fd, buf, (unsigned int)count);
    _lseeki64(fd, oldpos, SEEK_SET);
    return result;
}

#endif

/** Zero memory

    @param d pointer to memory region
    @param size_to_zero size of the area to zero in bytes
*/
inline static void gsd_zero_memory(void* d, size_t size_to_zero)
{
    memset(d, 0, size_to_zero);
}

/** @internal
    @brief Write large data buffer to file

    The system call pwrite() fails to write very large data buffers. This method calls pwrite() as
    many times as necessary to completely write a large buffer.

    @param fd File descriptor.
    @param buf Data buffer.
    @param count Number of bytes to write.
    @param offset Location in the file to start writing.

    @returns The total number of bytes written or a negative value on error.
*/
inline static ssize_t gsd_pwrite_retry(int fd, const void* buf, size_t count, int64_t offset)
{
    size_t total_bytes_written = 0;
    const char* ptr = (char*)buf;

    // perform multiple pwrite calls to complete a large write successfully
    while (total_bytes_written < count)
    {
        size_t to_write = count - total_bytes_written;
#if defined(_WIN32) || defined(__APPLE__)
        // win32 and apple raise an error for writes greater than INT_MAX
        if (to_write > INT_MAX / 2)
            to_write = INT_MAX / 2;
#endif

        errno = 0;
        ssize_t bytes_written
            = pwrite(fd, ptr + total_bytes_written, to_write, offset + total_bytes_written);
        if (bytes_written == -1 || (bytes_written == 0 && errno != 0))
        {
            return GSD_ERROR_IO;
        }

        total_bytes_written += bytes_written;
    }

    return total_bytes_written;
}

/** @internal
    @brief Read large data buffer to file

    The system call pread() fails to read very large data buffers. This method calls pread() as many
    times as necessary to completely read a large buffer.

    @param fd File descriptor.
    @param buf Data buffer.
    @param count Number of bytes to read.
    @param offset Location in the file to start reading.

    @returns The total number of bytes read or a negative value on error.
*/
inline static ssize_t gsd_pread_retry(int fd, void* buf, size_t count, int64_t offset)
{
    size_t total_bytes_read = 0;
    char* ptr = (char*)buf;

    // perform multiple pread calls to complete a large write successfully
    while (total_bytes_read < count)
    {
        size_t to_read = count - total_bytes_read;
#if defined(_WIN32) || defined(__APPLE__)
        // win32 and apple raise errors for reads greater than INT_MAX
        if (to_read > INT_MAX / 2)
            to_read = INT_MAX / 2;
#endif

        errno = 0;
        ssize_t bytes_read = pread(fd, ptr + total_bytes_read, to_read, offset + total_bytes_read);
        if (bytes_read == -1 || (bytes_read == 0 && errno != 0))
        {
            return GSD_ERROR_IO;
        }

        total_bytes_read += bytes_read;

        // handle end of file
        if (bytes_read == 0)
        {
            return total_bytes_read;
        }
    }

    return total_bytes_read;
}

/** @internal
    @brief Utility function to expand the memory space for the index block
    @param handle Handle to the open gsd file.
*/
static int gsd_expand_index(struct gsd_handle* handle)
{
    // multiply the index size each time it grows
    // this allows the index to grow rapidly to accommodate new frames
    const int multiplication_factor = 2;

    // save the old size and update the new size
    size_t old_size = handle->header.index_allocated_entries;
    size_t new_size = old_size * multiplication_factor;
    handle->header.index_allocated_entries = new_size;

    if (handle->open_flags == GSD_OPEN_READWRITE)
    {
        // allocate the new larger index block
        handle->index = (struct gsd_index_entry*)realloc(handle->index,
                                                         sizeof(struct gsd_index_entry) * new_size);
        if (handle->index == NULL)
        {
            return GSD_ERROR_MEMORY_ALLOCATION_FAILED;
        }

        // zero the new memory
        gsd_zero_memory(handle->index + old_size,
                        sizeof(struct gsd_index_entry) * (new_size - old_size));

        // now, put the new larger index at the end of the file
        handle->header.index_location = lseek(handle->fd, 0, SEEK_END);
        ssize_t bytes_written = gsd_pwrite_retry(handle->fd,
                                                 handle->index,
                                                 sizeof(struct gsd_index_entry) * new_size,
                                                 handle->header.index_location);

        if (bytes_written == -1 || bytes_written != sizeof(struct gsd_index_entry) * new_size)
        {
            return GSD_ERROR_IO;
        }

        // set the new file size
        handle->file_size = handle->header.index_location + bytes_written;
    }
    else if (handle->open_flags == GSD_OPEN_APPEND)
    {
        // in append mode, we don't have the whole index stored in memory. Instead, we need to copy
        // it in chunks from the file's old position to the new position
        char buf[GSD_COPY_BUFFER_SIZE];

        int64_t new_index_location = lseek(handle->fd, 0, SEEK_END);
        int64_t old_index_location = handle->header.index_location;
        size_t total_bytes_written = 0;
        size_t old_index_bytes = old_size * sizeof(struct gsd_index_entry);
        while (total_bytes_written < old_index_bytes)
        {
            size_t bytes_to_copy = GSD_COPY_BUFFER_SIZE;
            if (old_index_bytes - total_bytes_written < GSD_COPY_BUFFER_SIZE)
            {
                bytes_to_copy = old_index_bytes - total_bytes_written;
            }

            ssize_t bytes_read = gsd_pread_retry(handle->fd,
                                                 buf,
                                                 bytes_to_copy,
                                                 old_index_location + total_bytes_written);

            if (bytes_read == -1 || bytes_read != bytes_to_copy)
            {
                return GSD_ERROR_IO;
            }

            ssize_t bytes_written = gsd_pwrite_retry(handle->fd,
                                                     buf,
                                                     bytes_to_copy,
                                                     new_index_location + total_bytes_written);

            if (bytes_written == -1 || bytes_written != bytes_to_copy)
            {
                return GSD_ERROR_IO;
            }

            total_bytes_written += bytes_written;
        }

        // fill the new index space with 0s
        gsd_zero_memory(buf, GSD_COPY_BUFFER_SIZE);

        size_t new_index_bytes = new_size * sizeof(struct gsd_index_entry);
        while (total_bytes_written < new_index_bytes)
        {
            size_t bytes_to_copy = GSD_COPY_BUFFER_SIZE;
            if (new_index_bytes - total_bytes_written < GSD_COPY_BUFFER_SIZE)
            {
                bytes_to_copy = new_index_bytes - total_bytes_written;
            }

            ssize_t bytes_written = gsd_pwrite_retry(handle->fd,
                                                     buf,
                                                     bytes_to_copy,
                                                     new_index_location + total_bytes_written);

            if (bytes_written == -1 || bytes_written != bytes_to_copy)
            {
                return GSD_ERROR_IO;
            }

            total_bytes_written += bytes_written;
        }

        // update to the new index location in the header
        handle->header.index_location = new_index_location;
        handle->file_size = handle->header.index_location + total_bytes_written;
    }

    // sync the expanded index
    int retval = fsync(handle->fd);
    if (retval != 0)
    {
        return GSD_ERROR_IO;
    }

    // write the new header out
    ssize_t bytes_written
        = gsd_pwrite_retry(handle->fd, &(handle->header), sizeof(struct gsd_header), 0);
    if (bytes_written != sizeof(struct gsd_header))
    {
        return GSD_ERROR_IO;
    }

    // sync the updated header
    retval = fsync(handle->fd);
    if (retval != 0)
    {
        return GSD_ERROR_IO;
    }

    return GSD_SUCCESS;
}

inline static int gsd_cmp_name_id_pair(const void *p1, const void *p2)
    {
    return strcmp(((struct gsd_name_id_pair*)p1)->name,
                  ((struct gsd_name_id_pair*)p2)->name);
    }

/** @internal
    @brief Sort the name/id mapping

    @param handle Open handle to sort.
*/
inline static void gsd_sort_name_id_pairs(struct gsd_handle* handle)
{
    qsort(handle->names,
          handle->namelist_num_entries,
          sizeof(struct gsd_name_id_pair),
          gsd_cmp_name_id_pair);
}

/** @internal
    @brief utility function to search the namelist and return the index of the name

    @param handle handle to the open gsd file
    @param name string name

    @return the index of the name in handle->names[] or UINT16_MAX if not found
    @warning gsd_find_name() only searches entries that have already been commited by
             gsd_end_frame(). This is fine because a name may only appear once per frame.
*/
inline static uint16_t gsd_find_name(struct gsd_handle* handle, const char* name)
{
    size_t len = strlen(name);

    // return not found if the list is empty
    if (handle->namelist_written_entries == 0)
    {
        return UINT16_MAX;
    }

    // binary search for the first index entry at the requested frame
    size_t L = 0;
    size_t R = handle->namelist_written_entries;

    // base case:
    int cmp = strncmp(name, handle->names[L].name, len);
    if (cmp < 0)
    {
        return UINT16_MAX;
    }
    if (cmp == 0)
    {
        return L;
    }

    // progressively narrow the search window by halves
    do
    {
        size_t m = (L + R) / 2;
        cmp = strncmp(name, handle->names[m].name, len);

        if (cmp < 0)
        {
            R = m;
        }
        else if (cmp == 0)
        {
            return (uint16_t)m;
        }
        else
        {
            L = m;
        }
    } while ((R - L) > 1);

    return UINT16_MAX;
}

/** @internal
    @brief utility function to append a name to the namelist

    @param id[out] ID of the new name
    @param handle handle to the open gsd file
    @param name string name

    @return
      - GSD_SUCCESS (0) on success. Negative value on failure:
      - GSD_ERROR_IO: IO error (check errno).
      - GSD_ERROR_MEMORY_ALLOCATION_FAILED: Unable to allocate memory.
      - GSD_ERROR_FILE_MUST_BE_WRITABLE: File must not be read only.
*/
inline static int gsd_append_name(uint16_t* id, struct gsd_handle* handle, const char* name)
{
    if (handle->open_flags == GSD_OPEN_READONLY)
    {
        return GSD_ERROR_FILE_MUST_BE_WRITABLE;
    }

    if (handle->namelist_num_entries == handle->header.namelist_allocated_entries)
    {
        // TODO: expand index
        return GSD_ERROR_NAMELIST_FULL;
    }

    // add the name to the end of the index
    strncpy(handle->namelist[handle->namelist_num_entries].name,
            name,
            sizeof(struct gsd_namelist_entry) - 1);
    handle->namelist[handle->namelist_num_entries].name[sizeof(struct gsd_namelist_entry) - 1] = 0;

    // expand the names[] list if needed
    if (handle->namelist_num_entries + 1 > handle->names_allocated_size)
    {
        handle->names_allocated_size *= 2;
        handle->names
            = realloc(handle->names, sizeof(struct gsd_name_id_pair) * handle->names_allocated_size);
        if (handle->names == NULL)
        {
            return GSD_ERROR_MEMORY_ALLOCATION_FAILED;
        }
    }

    // update the names[] mapping
    handle->names[handle->namelist_num_entries].name
        = handle->namelist[handle->namelist_num_entries].name;
    handle->names[handle->namelist_num_entries].id = handle->namelist_num_entries;

    // increment the number of names in the list
    *id = (uint16_t)handle->namelist_num_entries;
    handle->namelist_num_entries++;

    return GSD_SUCCESS;
}

/** @internal
    @brief utility function to search the namelist and return the id assigned to the name

    @param handle handle to the open gsd file
    @param name string name

    @return the id assigned to the name, or UINT16_MAX if not found and append is false
*/
inline static uint16_t gsd_get_id(struct gsd_handle* handle, const char* name)
{
    uint16_t i = gsd_find_name(handle, name);
    if (i != UINT16_MAX)
        {
        return handle->names[i].id;
        }

    // otherwise, return not found
    return UINT16_MAX;
}

/** @internal
    @brief Truncate the file and write a new gsd header.

    @param fd file descriptor to initialize
    @param application Generating application name (truncated to 63 chars)
    @param schema Schema name for data to be written in this GSD file (truncated to 63 chars)
    @param schema_version Version of the scheme data to be written (make with gsd_make_version())
*/
static int gsd_initialize_file(int fd,
                        const char* application,
                        const char* schema,
                        uint32_t schema_version)
{
    // check if the file was created
    if (fd == -1)
    {
        return GSD_ERROR_IO;
    }

    int retval = ftruncate(fd, 0);
    if (retval != 0)
    {
        return GSD_ERROR_IO;
    }

    // populate header fields
    struct gsd_header header;
    gsd_zero_memory(&header, sizeof(header));

    header.magic = GSD_MAGIC_ID;
    header.gsd_version = gsd_make_version(1, 0);
    strncpy(header.application, application, sizeof(header.application) - 1);
    header.application[sizeof(header.application) - 1] = 0;
    strncpy(header.schema, schema, sizeof(header.schema) - 1);
    header.schema[sizeof(header.schema) - 1] = 0;
    header.schema_version = schema_version;
    header.index_location = sizeof(header);
    header.index_allocated_entries = GSD_INITIAL_INDEX_SIZE;
    header.namelist_location
        = header.index_location + sizeof(struct gsd_index_entry) * header.index_allocated_entries;
    header.namelist_allocated_entries = GSD_INITIAL_NAMELIST_SIZE;
    gsd_zero_memory(header.reserved, sizeof(header.reserved));

    // write the header out
    ssize_t bytes_written = gsd_pwrite_retry(fd, &header, sizeof(header), 0);
    if (bytes_written != sizeof(header))
    {
        return GSD_ERROR_IO;
    }

    // allocate and zero default index memory
    struct gsd_index_entry index[GSD_INITIAL_INDEX_SIZE];
    gsd_zero_memory(index, sizeof(index));

    // write the empty index out
    bytes_written = gsd_pwrite_retry(fd, index, sizeof(index), sizeof(header));
    if (bytes_written != sizeof(index))
    {
        return GSD_ERROR_IO;
    }

    // allocate and zero the namelist memory
    struct gsd_namelist_entry namelist[GSD_INITIAL_NAMELIST_SIZE];
    gsd_zero_memory(namelist, sizeof(namelist));

    // write the namelist out
    bytes_written
        = gsd_pwrite_retry(fd, namelist, sizeof(namelist), sizeof(header) + sizeof(index));
    if (bytes_written != sizeof(namelist))
    {
        return GSD_ERROR_IO;
    }

    // sync file
    retval = fsync(fd);
    if (retval != 0)
    {
        return GSD_ERROR_IO;
    }

    return GSD_SUCCESS;
}

/** @internal
    @brief Utility function to validate index entry
    @param handle handle to the open gsd file
    @param idx index of entry to validate

    @returns 1 if the entry is valid, 0 if it is not
*/
static int gsd_is_entry_valid(struct gsd_handle* handle, size_t idx)
{
    const struct gsd_index_entry entry = handle->index[idx];

    // check for valid type
    if (gsd_sizeof_type((enum gsd_type)entry.type) == 0)
    {
        return 0;
    }

    // validate that we don't read past the end of the file
    size_t size = entry.N * entry.M * gsd_sizeof_type((enum gsd_type)entry.type);
    if ((entry.location + size) > handle->file_size)
    {
        return 0;
    }

    // check for valid frame (frame cannot be more than the number of index entries)
    if (entry.frame >= handle->header.index_allocated_entries)
    {
        return 0;
    }

    // check for valid id
    if (entry.id >= handle->namelist_num_entries)
    {
        return 0;
    }

    // check for valid flags
    if (entry.flags != 0)
    {
        return 0;
    }

    return 1;
}

/** @internal
    @brief Read in the file index.

    @param handle Handle to read the header

    @pre handle->fd is an open file.
    @pre handle->open_flags is set.
*/
static int gsd_read_header(struct gsd_handle* handle)
{
    // check if the file was created
    if (handle->fd == -1)
    {
        return GSD_ERROR_IO;
    }

    // read the header
    ssize_t bytes_read = gsd_pread_retry(handle->fd, &handle->header, sizeof(struct gsd_header), 0);
    if (bytes_read == -1)
    {
        return GSD_ERROR_IO;
    }
    if (bytes_read != sizeof(struct gsd_header))
    {
        return GSD_ERROR_NOT_A_GSD_FILE;
    }

    // validate the header
    if (handle->header.magic != GSD_MAGIC_ID)
    {
        return GSD_ERROR_NOT_A_GSD_FILE;
    }

    if (handle->header.gsd_version < gsd_make_version(1, 0)
        && handle->header.gsd_version != gsd_make_version(0, 3))
    {
        return GSD_ERROR_INVALID_GSD_FILE_VERSION;
    }

    if (handle->header.gsd_version >= gsd_make_version(2, 0))
    {
        return GSD_ERROR_INVALID_GSD_FILE_VERSION;
    }

    // determine the file size
    handle->file_size = lseek(handle->fd, 0, SEEK_END);

// map the file in read only mode
#if GSD_USE_MMAP
    if (handle->open_flags == GSD_OPEN_READONLY)
    {
        size_t page_size = getpagesize();
        size_t index_size = sizeof(struct gsd_index_entry) * handle->header.index_allocated_entries;
        size_t offset = (handle->header.index_location / page_size) * page_size;
        handle->mapped_data = mmap(NULL,
                                   index_size + (handle->header.index_location - offset),
                                   PROT_READ,
                                   MAP_SHARED,
                                   handle->fd,
                                   offset);
        handle->index = (struct gsd_index_entry*)(((char*)handle->mapped_data)
                                                  + (handle->header.index_location - offset));

        if (handle->mapped_data == MAP_FAILED)
        {
            return GSD_ERROR_IO;
        }
    }
    else if (handle->open_flags == GSD_OPEN_READWRITE)
#endif
    {
        // read the indices into our own memory
        handle->mapped_data = NULL;

        // validate that the index block exists inside the file
        if (handle->header.index_location
                + sizeof(struct gsd_index_entry) * handle->header.index_allocated_entries
            > handle->file_size)
        {
            return GSD_ERROR_FILE_CORRUPT;
        }

        // read the index block
        handle->index = (struct gsd_index_entry*)malloc(sizeof(struct gsd_index_entry)
                                                        * handle->header.index_allocated_entries);
        if (handle->index == NULL)
        {
            return GSD_ERROR_MEMORY_ALLOCATION_FAILED;
        }

        bytes_read = gsd_pread_retry(handle->fd,
                                     handle->index,
                                     sizeof(struct gsd_index_entry)
                                         * handle->header.index_allocated_entries,
                                     handle->header.index_location);

        if (bytes_read == -1
            || bytes_read
                   != sizeof(struct gsd_index_entry) * handle->header.index_allocated_entries)
        {
            return GSD_ERROR_IO;
        }
    }
#if GSD_USE_MMAP
    else if (handle->open_flags == GSD_OPEN_APPEND)
    {
        // in append mode, we want to avoid reading the entire index in memory, but we also don't
        // want to bother keeping the mapping up to date. Map the index for now to determine
        // index_num_entries, but then unmap it and use different logic to manage a cache of only
        // unwritten index entries

        // mmap may fail if offset is not a multiple of the page size, so we
        // always memory map from the beginning of the file and then access the
        // index pointer by its offset.
        size_t page_size = getpagesize();
        size_t index_size = sizeof(struct gsd_index_entry) * handle->header.index_allocated_entries;
        size_t offset = (handle->header.index_location / page_size) * page_size;
        handle->mapped_data = mmap(NULL,
                                   index_size + (handle->header.index_location - offset),
                                   PROT_READ,
                                   MAP_SHARED,
                                   handle->fd,
                                   offset);
        handle->index = (struct gsd_index_entry*)(((char*)handle->mapped_data)
                                                  + (handle->header.index_location - offset));

        if (handle->mapped_data == MAP_FAILED)
        {
            return GSD_ERROR_IO;
        }
    }
#endif

    // since the namelist is small, we always allocate memory for it rather than memory mapping
    // validate that the namelist block exists inside the file
    if (handle->header.namelist_location
            + sizeof(struct gsd_namelist_entry) * handle->header.namelist_allocated_entries
        > handle->file_size)
    {
        return GSD_ERROR_FILE_CORRUPT;
    }

    // read the namelist block
    handle->namelist = (struct gsd_namelist_entry*)malloc(
        sizeof(struct gsd_namelist_entry) * handle->header.namelist_allocated_entries);
    if (handle->namelist == NULL)
    {
        return GSD_ERROR_MEMORY_ALLOCATION_FAILED;
    }

    bytes_read = gsd_pread_retry(handle->fd,
                                 handle->namelist,
                                 sizeof(struct gsd_namelist_entry)
                                     * handle->header.namelist_allocated_entries,
                                 handle->header.namelist_location);

    if (bytes_read == -1
        || bytes_read
               != sizeof(struct gsd_namelist_entry) * handle->header.namelist_allocated_entries)
    {
        return GSD_ERROR_IO;
    }

    // determine the number of namelist entries (marked by an empty string)
    // base case: the namelist is full
    handle->namelist_num_entries = handle->header.namelist_allocated_entries;

    // general case, find the first namelist entry that is the empty string
    size_t i;
    for (i = 0; i < handle->header.namelist_allocated_entries; i++)
    {
        if (handle->namelist[i].name[0] == 0)
        {
            handle->namelist_num_entries = i;
            break;
        }
    }

    // At this point, all namelist entries are written to disk
    handle->namelist_written_entries = handle->namelist_num_entries;

    // allocate and assign pointers to names
    handle->names_allocated_size = handle->namelist_num_entries;
    if (handle->names_allocated_size == 0)
    {
        handle->names_allocated_size = GSD_INITIAL_NAMELIST_SIZE;
    }

    handle->names = malloc(sizeof(struct gsd_name_id_pair) * handle->names_allocated_size);
    if (handle->names == NULL)
    {
        return GSD_ERROR_MEMORY_ALLOCATION_FAILED;
    }

    for (i = 0; i < handle->namelist_num_entries; i++)
    {
        handle->names[i].name = handle->namelist[i].name;
        handle->names[i].id = (uint16_t)i;
    }

    // sort the names
    gsd_sort_name_id_pairs(handle);

    // file is corrupt if first index entry is invalid
    if (handle->index[0].location != 0 && !gsd_is_entry_valid(handle, 0))
    {
        return GSD_ERROR_FILE_CORRUPT;
    }

    if (handle->index[0].location == 0)
    {
        handle->index_num_entries = 0;
    }
    else
    {
        // determine the number of index entries (marked by location = 0)
        // binary search for the first index entry with location 0
        size_t L = 0;
        size_t R = handle->header.index_allocated_entries;

        // progressively narrow the search window by halves
        do
        {
            size_t m = (L + R) / 2;

            // file is corrupt if any index entry is invalid or frame does not increase
            // monotonically
            if (handle->index[m].location != 0
                && (!gsd_is_entry_valid(handle, m)
                    || handle->index[m].frame < handle->index[L].frame))
            {
                return GSD_ERROR_FILE_CORRUPT;
            }

            if (handle->index[m].location != 0)
            {
                L = m;
            }
            else
            {
                R = m;
            }
        } while ((R - L) > 1);

        // this finds R = the first index entry with location = 0
        handle->index_num_entries = R;
    }

    // determine the current frame counter
    if (handle->index_num_entries == 0)
    {
        handle->cur_frame = 0;
    }
    else
    {
        handle->cur_frame = handle->index[handle->index_num_entries - 1].frame + 1;
    }

    // at this point, all valid index entries have been written to disk
    handle->index_written_entries = handle->index_num_entries;

    if (handle->open_flags == GSD_OPEN_APPEND)
    {
#if GSD_USE_MMAP
        // in append mode, we need to tear down the temporary mapping and allocate a temporary
        // buffer to hold indices for a single frame
        size_t page_size = getpagesize();
        size_t index_size = sizeof(struct gsd_index_entry) * handle->header.index_allocated_entries;
        size_t offset = (handle->header.index_location / page_size) * page_size;
        int retval
            = munmap(handle->mapped_data, index_size + (handle->header.index_location - offset));
        handle->index = NULL;

        if (retval != 0)
        {
            return GSD_ERROR_IO;
        }
#else
        free(handle->index);
#endif

        handle->append_index_size = 1;
        handle->index = (struct gsd_index_entry*)malloc(sizeof(struct gsd_index_entry)
                                                        * handle->append_index_size);
        if (handle->index == NULL)
        {
            return GSD_ERROR_MEMORY_ALLOCATION_FAILED;
        }

        handle->mapped_data = NULL;
    }

    return GSD_SUCCESS;
}

uint32_t gsd_make_version(unsigned int major, unsigned int minor)
{
    return major << (sizeof(uint32_t) * 4) | minor;
}

int gsd_create(const char* fname,
               const char* application,
               const char* schema,
               uint32_t schema_version)
{
    int extra_flags = 0;
#ifdef _WIN32
    extra_flags = _O_BINARY;
#endif

    // create the file
    int fd = open(fname,
                  O_RDWR | O_CREAT | O_TRUNC | extra_flags,
                  S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
    int retval = gsd_initialize_file(fd, application, schema, schema_version);
    close(fd);
    return retval;
}

int gsd_create_and_open(struct gsd_handle* handle,
                        const char* fname,
                        const char* application,
                        const char* schema,
                        uint32_t schema_version,
                        const enum gsd_open_flag flags,
                        int exclusive_create)
{
    int extra_flags = 0;
#ifdef _WIN32
    extra_flags = _O_BINARY;
#endif

    // set the open flags in the handle
    if (flags == GSD_OPEN_READWRITE)
    {
        handle->open_flags = GSD_OPEN_READWRITE;
    }
    else if (flags == GSD_OPEN_READONLY)
    {
        return GSD_ERROR_FILE_MUST_BE_WRITABLE;
    }
    else if (flags == GSD_OPEN_APPEND)
    {
        handle->open_flags = GSD_OPEN_APPEND;
    }

    // set the exclusive create bit
    if (exclusive_create)
    {
        extra_flags |= O_EXCL;
    }

    // create the file
    handle->fd = open(fname,
                      O_RDWR | O_CREAT | O_TRUNC | extra_flags,
                      S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
    int retval = gsd_initialize_file(handle->fd, application, schema, schema_version);
    if (retval != 0)
    {
        close(handle->fd);
        return retval;
    }

    retval = gsd_read_header(handle);
    if (retval != 0)
    {
        close(handle->fd);
    }
    return retval;
}

int gsd_open(struct gsd_handle* handle, const char* fname, const enum gsd_open_flag flags)
{
    // allocate the handle
    gsd_zero_memory(handle, sizeof(struct gsd_handle));
    handle->index = NULL;
    handle->namelist = NULL;
    handle->cur_frame = 0;

    int extra_flags = 0;
#ifdef _WIN32
    extra_flags = _O_BINARY;
#endif

    // open the file
    if (flags == GSD_OPEN_READWRITE)
    {
        handle->fd = open(fname, O_RDWR | extra_flags);
        handle->open_flags = GSD_OPEN_READWRITE;
    }
    else if (flags == GSD_OPEN_READONLY)
    {
        handle->fd = open(fname, O_RDONLY | extra_flags);
        handle->open_flags = GSD_OPEN_READONLY;
    }
    else if (flags == GSD_OPEN_APPEND)
    {
        handle->fd = open(fname, O_RDWR | extra_flags);
        handle->open_flags = GSD_OPEN_APPEND;
    }

    int retval = gsd_read_header(handle);
    if (retval != 0)
    {
        close(handle->fd);
    }
    return retval;
}

int gsd_truncate(struct gsd_handle* handle)
{
    if (handle == NULL)
    {
        return GSD_ERROR_INVALID_ARGUMENT;
    }
    if (handle->open_flags == GSD_OPEN_READONLY)
    {
        return GSD_ERROR_FILE_MUST_BE_WRITABLE;
    }

    // deallocate indices
    if (handle->namelist != NULL)
    {
        free(handle->namelist);
        handle->namelist = NULL;
    }

    if (handle->names != NULL)
    {
        free(handle->names);
        handle->names = NULL;
    }

    if (handle->index != NULL)
    {
        free(handle->index);
        handle->index = NULL;
    }

    // keep a copy of the old header
    struct gsd_header old_header = handle->header;
    int retval = gsd_initialize_file(handle->fd,
                                     old_header.application,
                                     old_header.schema,
                                     old_header.schema_version);

    if (retval != 0)
    {
        return retval;
    }

    return gsd_read_header(handle);
}

int gsd_close(struct gsd_handle* handle)
{
    if (handle == NULL)
    {
        return GSD_ERROR_INVALID_ARGUMENT;
    }

    // save the fd so we can use it after freeing the handle
    int fd = handle->fd;

// zero and free memory allocated in the handle
#if GSD_USE_MMAP
    if (handle->mapped_data != NULL)
    {
        size_t page_size = getpagesize();
        size_t index_size = sizeof(struct gsd_index_entry) * handle->header.index_allocated_entries;
        size_t offset = (handle->header.index_location / page_size) * page_size;
        int retval
            = munmap(handle->mapped_data, index_size + (handle->header.index_location - offset));
        handle->mapped_data = NULL;
        handle->index = NULL;

        if (retval != 0)
        {
            return GSD_ERROR_IO;
        }

        gsd_zero_memory(handle, sizeof(struct gsd_handle));
    }
    else
#endif
    {
        if (handle->index != NULL)
        {
            free(handle->index);
            handle->index = NULL;

            gsd_zero_memory(handle, sizeof(struct gsd_handle));
        }
    }

    if (handle->namelist != NULL)
    {
        free(handle->namelist);
        handle->namelist = NULL;
    }

    if (handle->names != NULL)
    {
        free(handle->names);
        handle->names = NULL;
    }

    // close the file
    int retval = close(fd);
    if (retval != 0)
    {
        return GSD_ERROR_IO;
    }

    return GSD_SUCCESS;
}

int gsd_end_frame(struct gsd_handle* handle)
{
    if (handle == NULL)
    {
        return GSD_ERROR_INVALID_ARGUMENT;
    }
    if (handle->open_flags == GSD_OPEN_READONLY)
    {
        return GSD_ERROR_FILE_MUST_BE_WRITABLE;
    }

    // all data chunks have already been written to the file and the index updated in memory. To end
    // a frame, all we need to do is increment the frame counter
    handle->cur_frame++;

    // and write unwritten index entries to the file (if there are any to write)
    uint64_t entries_to_write = handle->index_num_entries - handle->index_written_entries;
    if (entries_to_write > 0)
    {
        // write just those unwritten entries to the end of the index block
        int64_t write_pos = handle->header.index_location
                            + sizeof(struct gsd_index_entry) * handle->index_written_entries;

        // in append mode, the start of the write is at the start of the index in memory
        // in readwrite mode, the entire index is in memory, so start at index_written_entries
        struct gsd_index_entry* data = handle->index;
        if (handle->open_flags != GSD_OPEN_APPEND)
        {
            data += handle->index_written_entries;
        }

        ssize_t bytes_written = gsd_pwrite_retry(handle->fd,
                                                 data,
                                                 sizeof(struct gsd_index_entry) * entries_to_write,
                                                 write_pos);

        if (bytes_written == -1
            || bytes_written != sizeof(struct gsd_index_entry) * entries_to_write)
        {
            return GSD_ERROR_IO;
        }

        handle->index_written_entries += entries_to_write;
    }

    // update the namelist on disk
    uint64_t new_namelist_entries = handle->namelist_num_entries - handle->namelist_written_entries;
    if (new_namelist_entries)
    {
        ssize_t bytes_written
            = gsd_pwrite_retry(handle->fd,
                            &(handle->namelist[handle->namelist_written_entries]),
                            sizeof(struct gsd_namelist_entry) * new_namelist_entries,
                            handle->header.namelist_location
                                + sizeof(struct gsd_namelist_entry) * handle->namelist_written_entries);

        if (bytes_written != sizeof(struct gsd_namelist_entry) * new_namelist_entries)
        {
            return GSD_ERROR_IO;
        }


        handle->namelist_written_entries = handle->namelist_num_entries;

        // sort written names for search on the next frame
        gsd_sort_name_id_pairs(handle);

        // ensure that the new namelist is comitted to the disk
        int retval = fsync(handle->fd);
        if (retval != 0)
        {
            return GSD_ERROR_IO;
        }
    }

    return GSD_SUCCESS;
}

int gsd_write_chunk(struct gsd_handle* handle,
                    const char* name,
                    enum gsd_type type,
                    uint64_t N,
                    uint32_t M,
                    uint8_t flags,
                    const void* data)
{
    // validate input
    if (data == NULL)
    {
        return GSD_ERROR_INVALID_ARGUMENT;
    }
    if (N == 0 || M == 0 || gsd_sizeof_type(type) == 0)
    {
        return GSD_ERROR_INVALID_ARGUMENT;
    }
    if (handle->open_flags == GSD_OPEN_READONLY)
    {
        return GSD_ERROR_FILE_MUST_BE_WRITABLE;
    }
    if (flags != 0)
    {
        return GSD_ERROR_INVALID_ARGUMENT;
    }

    // populate fields in the index_entry data
    struct gsd_index_entry index_entry;
    gsd_zero_memory(&index_entry, sizeof(index_entry));
    index_entry.frame = handle->cur_frame;
    index_entry.id = gsd_get_id(handle, name);
    if (index_entry.id == UINT16_MAX)
    {
        // not found, append to the index
        int retval = gsd_append_name(&index_entry.id, handle, name);
        if (retval != GSD_SUCCESS)
        {
            return retval;
        }

        if (index_entry.id == UINT16_MAX)
        {
            // this should never happen
            return GSD_ERROR_NAMELIST_FULL;
        }
    }
    index_entry.type = (uint8_t)type;
    index_entry.N = N;
    index_entry.M = M;
    size_t size = N * M * gsd_sizeof_type(type);

    // find the location at the end of the file for the chunk
    index_entry.location = handle->file_size;

    // write the data
    ssize_t bytes_written = gsd_pwrite_retry(handle->fd, data, size, index_entry.location);
    if (bytes_written == -1 || bytes_written != size)
    {
        return GSD_ERROR_IO;
    }

    // update the file_size in the handle
    handle->file_size += bytes_written;

    // update the index entry in the index
    // need to expand the index if it is already full
    if (handle->index_num_entries >= handle->header.index_allocated_entries)
    {
        int retval = gsd_expand_index(handle);
        if (retval != 0)
        {
            return retval;
        }
    }

    // once we get here, there is a free slot to add this entry to the index
    size_t slot = handle->index_num_entries;

    // in append mode, only unwritten entries are stored in memory
    if (handle->open_flags == GSD_OPEN_APPEND)
    {
        slot -= handle->index_written_entries;
        if (slot >= handle->append_index_size)
        {
            handle->append_index_size *= 2;
            handle->index = (struct gsd_index_entry*)realloc(handle->index,
                                                             handle->append_index_size
                                                                 * sizeof(struct gsd_index_entry));
            if (handle->index == NULL)
            {
                return GSD_ERROR_MEMORY_ALLOCATION_FAILED;
            }
        }
    }
    handle->index[slot] = index_entry;
    handle->index_num_entries++;

    return GSD_SUCCESS;
}

uint64_t gsd_get_nframes(struct gsd_handle* handle)
{
    if (handle == NULL)
    {
        return 0;
    }
    return handle->cur_frame;
}

const struct gsd_index_entry*
gsd_find_chunk(struct gsd_handle* handle, uint64_t frame, const char* name)
{
    if (handle == NULL)
    {
        return NULL;
    }
    if (name == NULL)
    {
        return NULL;
    }
    if (frame >= gsd_get_nframes(handle))
    {
        return NULL;
    }
    if (handle->open_flags == GSD_OPEN_APPEND)
    {
        return NULL;
    }

    // find the id for the given name
    uint16_t match_id = gsd_get_id(handle, name);
    if (match_id == UINT16_MAX)
    {
        return NULL;
    }

    // binary search for the first index entry at the requested frame
    size_t L = 0;
    size_t R = handle->index_num_entries;

    // progressively narrow the search window by halves
    do
    {
        size_t m = (L + R) / 2;

        if (frame < handle->index[m].frame)
        {
            R = m;
        }
        else
        {
            L = m;
        }
    } while ((R - L) > 1);

    // this finds L = the rightmost index with the desired frame
    int64_t cur_index;

    // search all index entries with the matching frame
    for (cur_index = L; (cur_index >= 0) && (handle->index[cur_index].frame == frame); cur_index--)
    {
        // if the frame matches, check the id
        if (match_id == handle->index[cur_index].id)
        {
            return &(handle->index[cur_index]);
        }
    }

    // if we got here, we didn't find the specified chunk
    return NULL;
}

int gsd_read_chunk(struct gsd_handle* handle, void* data, const struct gsd_index_entry* chunk)
{
    if (handle == NULL)
    {
        return GSD_ERROR_INVALID_ARGUMENT;
    }
    if (data == NULL)
    {
        return GSD_ERROR_INVALID_ARGUMENT;
    }
    if (chunk == NULL)
    {
        return GSD_ERROR_INVALID_ARGUMENT;
    }
    if (handle->open_flags == GSD_OPEN_APPEND)
    {
        return GSD_ERROR_FILE_MUST_BE_READABLE;
    }

    size_t size = chunk->N * chunk->M * gsd_sizeof_type((enum gsd_type)chunk->type);
    if (size == 0)
    {
        return GSD_ERROR_FILE_CORRUPT;
    }
    if (chunk->location == 0)
    {
        return GSD_ERROR_FILE_CORRUPT;
    }

    // validate that we don't read past the end of the file
    if ((chunk->location + size) > handle->file_size)
    {
        return GSD_ERROR_FILE_CORRUPT;
    }

    ssize_t bytes_read = gsd_pread_retry(handle->fd, data, size, chunk->location);
    if (bytes_read == -1 || bytes_read != size)
    {
        return GSD_ERROR_IO;
    }

    return GSD_SUCCESS;
}

size_t gsd_sizeof_type(enum gsd_type type)
{
    size_t val = 0;
    if (type == GSD_TYPE_UINT8)
    {
        val = sizeof(uint8_t);
    }
    else if (type == GSD_TYPE_UINT16)
    {
        val = sizeof(uint16_t);
    }
    else if (type == GSD_TYPE_UINT32)
    {
        val = sizeof(uint32_t);
    }
    else if (type == GSD_TYPE_UINT64)
    {
        val = sizeof(uint64_t);
    }
    else if (type == GSD_TYPE_INT8)
    {
        val = sizeof(int8_t);
    }
    else if (type == GSD_TYPE_INT16)
    {
        val = sizeof(int16_t);
    }
    else if (type == GSD_TYPE_INT32)
    {
        val = sizeof(int32_t);
    }
    else if (type == GSD_TYPE_INT64)
    {
        val = sizeof(int64_t);
    }
    else if (type == GSD_TYPE_FLOAT)
    {
        val = sizeof(float);
    }
    else if (type == GSD_TYPE_DOUBLE)
    {
        val = sizeof(double);
    }
    else
    {
        return 0;
    }
    return val;
}

const char*
gsd_find_matching_chunk_name(struct gsd_handle* handle, const char* match, const char* prev)
{
    if (handle == NULL)
    {
        return NULL;
    }
    if (match == NULL)
    {
        return NULL;
    }
    if (handle->namelist_written_entries == 0)
    {
        return NULL;
    }

    // determine search start index
    size_t start;
    if (prev == NULL)
    {
        start = 0;
    }
    else
    {
        uint16_t found = gsd_find_name(handle, prev);
        if (found == UINT16_MAX)
        {
            // prev is not in the list
            return NULL;
        }
        start = (size_t)found + 1;
    }

    size_t match_len = strlen(match);
    size_t i;
    for (i = start; i < handle->namelist_written_entries; i++)
    {
        if (0 == strncmp(match, handle->names[i].name, match_len))
        {
            return handle->names[i].name;
        }
    }

    // searched past the end of the list, return NULL
    return NULL;
}

// undefine windows wrapper macros
#ifdef _WIN32
#undef lseek
#undef write
#undef read
#undef open
#undef ftruncate

#endif
