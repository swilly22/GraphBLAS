//------------------------------------------------------------------------------
// GB_convert_to_full: convert a matrix to full; deleting prior values
//------------------------------------------------------------------------------

// SuiteSparse:GraphBLAS, Timothy A. Davis, (c) 2017-2020, All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

//------------------------------------------------------------------------------

#include "GB.h"

GrB_Info GB_convert_to_full     // convert matrix to full; delete prior values
(
    GrB_Matrix A                // matrix to convert to full
)
{

    //--------------------------------------------------------------------------
    // check inputs
    //--------------------------------------------------------------------------

    GB_void *Ax_new = NULL ;
    ASSERT_MATRIX_OK (A, "A converting to full", GB0) ;
    GBURBLE ("(to full) ") ;
    ASSERT (GB_ZOMBIES_OK (A)) ;
    ASSERT (GB_JUMBLED_OK (A)) ;
    ASSERT (GB_PENDING_OK (A)) ;
    ASSERT (GB_IS_FULL (A) || GB_IS_BITMAP (A) || GB_IS_SPARSE (A) ||
        GB_IS_HYPERSPARSE (A)) ;

    int64_t avdim = A->vdim ;
    int64_t avlen = A->vlen ;
    GrB_Index anzmax ;
    bool ok = GB_Index_multiply (&anzmax, avlen, avdim) ;
    if (!ok)
    { 
        // problem too large
        return (GrB_OUT_OF_MEMORY) ;
    }

    //--------------------------------------------------------------------------
    // allocate new space for A->x
    //--------------------------------------------------------------------------

    #ifdef GB_DEBUG
    // in debug mode, calloc the matrix so it can be safely printed below
    Ax_new = GB_CALLOC (anzmax * A->type->size, GB_void) ;    // BIG (debug)
    #else
    // in production mode, A->x is uninitialized
    Ax_new = GB_MALLOC (anzmax * A->type->size, GB_void) ;
    #endif

    if (Ax_new == NULL)
    { 
        // out of memory
        return (GrB_OUT_OF_MEMORY) ;
    }

    //--------------------------------------------------------------------------
    // free all prior content and transplant the new content into A
    //--------------------------------------------------------------------------

    GB_phbix_free (A) ;
    A->x = Ax_new ;
    A->plen = -1 ;
    A->nvec = avdim ;
    A->nvec_nonempty = (avlen == 0) ? 0 : avdim ;
    A->nzmax = GB_IMAX (anzmax, 1) ;
    A->magic = GB_MAGIC ;

    //--------------------------------------------------------------------------
    // return result
    //--------------------------------------------------------------------------

    ASSERT_MATRIX_OK (A, "A converted to full (values all zero)", GB0) ;
    ASSERT (GB_IS_FULL (A)) ;
    return (GrB_SUCCESS) ;
}

