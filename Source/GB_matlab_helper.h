//------------------------------------------------------------------------------
// GB_matlab_helper.h: helper functions for MATLAB interface
//------------------------------------------------------------------------------

// SuiteSparse:GraphBLAS, Timothy A. Davis, (c) 2017-2019, All Rights Reserved.
// http://suitesparse.com   See GraphBLAS/Doc/License.txt for license.

//------------------------------------------------------------------------------

// These functions are only used by the MATLAB interface for
// SuiteSparse:GraphBLAS.

#ifndef GB_MATLAB_HELPER_H
#define GB_MATLAB_HELPER_H

#include "GB.h"

void GB_matlab_helper1              // convert zero-based indices to one-based
(
    double *GB_RESTRICT I_double,   // output array
    const GrB_Index *GB_RESTRICT I, // input array
    int64_t nvals                   // size of input and output arrays
) ;

void GB_matlab_helper1i             // convert zero-based indices to one-based
(
    int64_t *GB_RESTRICT I,         // input/output array
    int64_t nvals                   // size of input/output array
) ;

void GB_matlab_helper2              // fill Xp and Xi for a dense matrix
(
    GrB_Index *GB_RESTRICT Xp,      // size ncols+1
    GrB_Index *GB_RESTRICT Xi,      // size nrows*ncols
    int64_t ncols,
    int64_t nrows
) ;

bool GB_matlab_helper3              // return true if OK, false on error
(
    int64_t *GB_RESTRICT List,      // size len, output array
    const double *GB_RESTRICT List_double, // size len, input array
    int64_t len,
    int64_t *List_max               // also compute the max entry in the list
) ;

bool GB_matlab_helper3i             // return true if OK, false on error
(
    int64_t *GB_RESTRICT List,      // size len, output array
    const int64_t *GB_RESTRICT List_int64, // size len, input array
    int64_t len,
    int64_t *List_max               // also compute the max entry in the list
) ;

bool GB_matlab_helper4              // return true if OK, false on error
(
    const GrB_Index *GB_RESTRICT I, // array of size len
    const int64_t len,
    GrB_Index *List_max             // find max (I) + 1
) ;

void GB_matlab_helper5              // construct pattern of S
(
    GrB_Index *GB_RESTRICT Si,         // array of size anz
    GrB_Index *GB_RESTRICT Sj,         // array of size anz
    const GrB_Index *GB_RESTRICT Mi,   // array of size mnz
    const GrB_Index *GB_RESTRICT Mj,   // array of size mnz
    GrB_Index *GB_RESTRICT Ai,         // array of size anz
    const GrB_Index anz
) ;

void GB_matlab_helper6              // set Gbool to all true
(
    bool *GB_RESTRICT Gbool,        // array of size gnvals
    const GrB_Index gnvals
) ;

void GB_matlab_helper7              // Kx = uint64 (0:mnz-1)
(
    uint64_t *GB_RESTRICT Kx,       // array of size mnz
    const GrB_Index mnz
) ;

void GB_matlab_helper8
(
    GB_void *C,         // output array of size nvals * s
    GB_void *A,         // input scalar of size s
    GrB_Index nvals,    // size of C
    size_t s            // size of each scalar
) ;

#endif

