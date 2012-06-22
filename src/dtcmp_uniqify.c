/* Copyright (c) 2012, Lawrence Livermore National Security, LLC.
 * Produced at the Lawrence Livermore National Laboratory.
 * Written by Adam Moody <moody20@llnl.gov>.
 * LLNL-CODE-557516.
 * All rights reserved.
 * This file is part of the DTCMP library.
 * For details, see https://github.com/hpc/dtcmp
 * Please also read this file: LICENSE.TXT. */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "mpi.h"
#include "dtcmp_internal.h"

/* allocate a handle for uniqify function */
int dtcmp_handle_alloc_uniqify(size_t size, dtcmp_handle_uniqify_t** vals, DTCMP_Handle* handle)
{
  /* setup handle and buffer to merge data to return to user */
  void* ret_buf = NULL;
  size_t ret_buf_size = sizeof(dtcmp_handle_free_fn) + sizeof(dtcmp_handle_uniqify_t) + size;
  if (ret_buf_size > 0) {
    ret_buf = (void*) dtcmp_malloc(ret_buf_size, 0, __FILE__, __LINE__);
    if (ret_buf == NULL) {
      /* TODO: error */
    }
  }

  /* compute and allocate space to merge received data */
  char* ret_buf_tmp = (char*)ret_buf;
  if (ret_buf != NULL) {
    /* allocate and initialize function pointer as first item in handle struct */
    dtcmp_handle_free_fn* fn = (dtcmp_handle_free_fn*) ret_buf;
    *fn = dtcmp_handle_free_uniqify;
    ret_buf_tmp += sizeof(dtcmp_handle_free_fn);

    /* create space to hold the types, op, and pointer to the buffer */
    dtcmp_handle_uniqify_t* values = (dtcmp_handle_uniqify_t*) ret_buf_tmp;
    values->key    = MPI_DATATYPE_NULL;
    values->keysat = MPI_DATATYPE_NULL;
    values->cmp    = DTCMP_OP_NULL;
    ret_buf_tmp += sizeof(dtcmp_handle_uniqify_t);

    /* record the start of the buffer */
    if (size > 0) {
      values->buf = (void*) ret_buf_tmp;
    } else {
      values->buf    = NULL;
    }

    *vals = values;
  }

  /* set the handle value */
  *handle = ret_buf;

  return DTCMP_SUCCESS;
}

/* assumes that handle just points to one big block of memory that must be freed */
int dtcmp_handle_free_uniqify(DTCMP_Handle* handle)
{
  if (handle != NULL && *handle != DTCMP_HANDLE_NULL) {
    /* get pointer to handle memory block and skip over fn pointer (first item) */
    char* ptr = (char*) *handle;
    ptr += sizeof(dtcmp_handle_free_fn);

    /* free the types we have cached here */
    dtcmp_handle_uniqify_t* vals = (dtcmp_handle_uniqify_t*) ptr;
    if (vals->key != MPI_DATATYPE_NULL) {
      MPI_Type_free(&vals->key);
    }
    if (vals->keysat != MPI_DATATYPE_NULL) {
      MPI_Type_free(&vals->keysat);
    }
    if (vals->cmp != DTCMP_OP_NULL) {
      DTCMP_Op_free(&vals->cmp);
    }

    /* now free off the memory */
    free(*handle);
    *handle = DTCMP_HANDLE_NULL;
  }
  return DTCMP_SUCCESS;
}

/* the dtcmp_uniqify function takes an input buffer with count, key,
 * keysat, and comparison operation and allocates a new buffer ensuring
 * each element is unique by copying original elements with rank and
 * original index, returns new buffer, new key and ketsat types, and
 * new comparison operation.  When done, the associated handle must be
 * passed to DTCMP_Handle_free to clean up. */
int dtcmp_uniqify(
  const void* inbuf, int count, MPI_Datatype inkey, MPI_Datatype inkeysat, DTCMP_Op incmp, DTCMP_Flags inhints,
  void** outbuf, MPI_Datatype* outkey, MPI_Datatype* outkeysat, DTCMP_Op* outcmp, DTCMP_Flags* outhints,
  MPI_Comm comm, DTCMP_Handle* handle)
{
  /* TODO: with an allreduce and scan, we get total number of items,
   * and the offset of each of ours, then use this info to use one
   * original index tag globally across procs and select the smallest
   * datatype to use as this tag */

  /* TODO: just copy input params to output and return if inhints have unique bit set */
  *outhints = inhints | DTCMP_FLAG_UNIQUE;

  /* DTCMP_OP_create_hseries(first, cmp_offset, series_offset, second, cmp) 
   * or provide all functions with separate key and sat types so we can pull them apart */

  /* get key true extent */
  MPI_Aint key_true_lb, key_true_extent;
  MPI_Type_get_true_extent(inkey, &key_true_lb, &key_true_extent);

  /* get keysat true extent */
  MPI_Aint keysat_true_lb, keysat_true_extent;
  MPI_Type_get_true_extent(inkeysat, &keysat_true_lb, &keysat_true_extent);

  /* allocate buffer to hold new elements (key, rank, index, keysat) */
  dtcmp_handle_uniqify_t* values;
  size_t elem_size = key_true_extent + 2 * sizeof(int) + keysat_true_extent;
  size_t new_buf_size = count * elem_size;
  dtcmp_handle_alloc_uniqify(new_buf_size, &values, handle);

  /* get our rank */
  int rank;
  MPI_Comm_rank(comm, &rank);

  /* copy (rank,index,keysat) into new buffer */
  int i;
  char* new_buf = values->buf;
  for (i = 0; i < count; i++) {
    const char* src = (char*)inbuf + i * keysat_true_extent;
    char* dst = new_buf + i * elem_size;

    /* copy the key */
    DTCMP_Memcpy(dst, 1, inkey, src, 1, inkey);
    dst += key_true_extent;

    /* copy the rank */
    memcpy(dst, &rank, sizeof(int));
    dst += sizeof(int);

    /* copy the index */
    memcpy(dst, &i, sizeof(int));
    dst += sizeof(int);

    /* copy the keysat */
    DTCMP_Memcpy(dst, 1, inkeysat, src, 1, inkeysat);
  }

  MPI_Datatype types[4];

  /* build new key type */
  types[0] = inkey;
  types[1] = MPI_INT;
  types[2] = MPI_INT;
  dtcmp_type_concat(3, types, &values->key);

  /* build new keysat type */
  types[0] = inkey;
  types[1] = MPI_INT;
  types[2] = MPI_INT;
  types[3] = inkeysat;
  dtcmp_type_concat(4, types, &values->keysat);

  /* build new comparison op */
  DTCMP_Op cmp_2int;
  DTCMP_Op_create_series(DTCMP_OP_INT_ASCEND, DTCMP_OP_INT_ASCEND, &cmp_2int);
  DTCMP_Op_create_series(incmp, cmp_2int, &values->cmp);
  DTCMP_Op_free(&cmp_2int);

  /* set output parameters */
  *outbuf    = values->buf;
  *outkey    = values->key;
  *outkeysat = values->keysat;
  *outcmp    = values->cmp;

  return DTCMP_SUCCESS;
}

int dtcmp_deuniqify(
  const void* buf, int count, MPI_Datatype key, MPI_Datatype keysat,
  void* outbuf, MPI_Datatype outkey, MPI_Datatype outkeysat,
  DTCMP_Handle* handle)
{
  /* get unique key true extent */
  MPI_Aint key_true_lb, key_true_extent;
  MPI_Type_get_true_extent(key, &key_true_lb, &key_true_extent);

  /* get unique keysat true extent */
  MPI_Aint keysat_true_lb, keysat_true_extent;
  MPI_Type_get_true_extent(keysat, &keysat_true_lb, &keysat_true_extent);

  /* get output keysat extent */
  MPI_Aint outkeysat_lb, outkeysat_extent;
  MPI_Type_get_extent(outkeysat, &outkeysat_lb, &outkeysat_extent);

  /* copy original keysat into buffer */
  int i;
  const char* src = (char*)buf + key_true_extent;
  char* dst = (char*)outbuf;
  for (i = 0; i < count; i++) {
    /* copy the key */
    DTCMP_Memcpy(dst, 1, outkeysat, src, 1, outkeysat);
    src += keysat_true_extent;
    dst += outkeysat_extent;
  }

  /* free handle allocated during uniqify call */
  DTCMP_Free(handle);

  return DTCMP_SUCCESS;
}

int dtcmp_deuniqifyz(
  const void* buf, int count, MPI_Datatype key, MPI_Datatype keysat,
  void** outbuf, MPI_Datatype outkey, MPI_Datatype outkeysat,
  DTCMP_Handle* uniqhandle, DTCMP_Handle* handle)
{
  /* get output keysat true extent */
  MPI_Aint outkeysat_true_lb, outkeysat_true_extent;
  MPI_Type_get_true_extent(outkeysat, &outkeysat_true_lb, &outkeysat_true_extent);

  /* allocate buffer to hold output elements */
  size_t new_buf_size = count * outkeysat_true_extent;
  dtcmp_handle_alloc_single(new_buf_size, outbuf, handle);

  /* copy original keysat into buffer */
  dtcmp_deuniqify(buf, count, key, keysat, *outbuf, outkey, outkeysat, uniqhandle);

  return DTCMP_SUCCESS;
}