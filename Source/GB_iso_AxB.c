//------------------------------------------------------------------------------
// GB_iso_AxB: check for iso result for C=A*B and compute the iso scalar for C
//------------------------------------------------------------------------------

// SuiteSparse:GraphBLAS, Timothy A. Davis, (c) 2017-2021, All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

//------------------------------------------------------------------------------

// Return true if C=A*B results in an iso matrix C, and return the iso value of
// C.  The type of the matrix C and scalar c is semiring->add->ztype.

// If both A and B are full and iso, then C is also full and iso, for nearly
// all semirings.  The inner dimension of the matrix multiply is required to
// compute the iso value of C.  Assuming all matrices are CSC:

//  C = A*B     n = A->vdim == B->vlen
//  C = A'*B    n = A->vlen == B->vlen
//  C = A*B'    n = A->vdim == B->vdim
//  C = A'*B'   n = A->vlen == B->vdim

#include "GB_mxm.h"
#include "GB_reduce.h"
#include "GB_binop.h"

bool GB_iso_AxB             // C = A*B, return true if C is iso
(
    // output
    GB_void *restrict c,    // output scalar of iso array
    // input
    GrB_Matrix A,           // input matrix
    GrB_Matrix B,           // input matrix
    uint64_t n,             // inner dimension of the matrix multiply
    GrB_Semiring semiring   // semiring
)
{

    //--------------------------------------------------------------------------
    // get inputs
    //--------------------------------------------------------------------------

    ASSERT_MATRIX_OK (A, "A for GB_iso_AxB", GB0) ;
    ASSERT_MATRIX_OK (B, "B for GB_iso_AxB", GB0) ;
    ASSERT_SEMIRING_OK (semiring, "semiring for GB_iso_AxB", GB0) ;
    ASSERT (c != NULL) ;

    //--------------------------------------------------------------------------
    // quick return if multop is positional
    //--------------------------------------------------------------------------

    GB_Opcode add_opcode = semiring->add->op->opcode ;
    const GrB_BinaryOp multiply = semiring->multiply ;

    if (GB_OP_IS_POSITIONAL (multiply) || add_opcode >= GB_USER_opcode)
    { 
        // C is not iso if the multiply op is positional, or if the monoid
        // is user-defined
        return (false) ;
    }

    //--------------------------------------------------------------------------
    // get the binary operator and the types of C, A, and B
    //--------------------------------------------------------------------------

    const GxB_binary_function fmult = multiply->function ;
    GB_Opcode mult_opcode = multiply->opcode ;

    const GrB_Type xtype = multiply->xtype ;
    const GrB_Type ytype = multiply->ytype ;
    const GrB_Type ztype = multiply->ztype ;

    const GB_Type_code xcode = xtype->code ;
    const GB_Type_code ycode = ytype->code ;
    const GB_Type_code zcode = ztype->code ;
    const GB_Type_code acode = A->type->code ;
    const GB_Type_code bcode = B->type->code ;

    const size_t xsize = xtype->size ;
    const size_t ysize = ytype->size ;
    const size_t zsize = ztype->size ;
    const size_t asize = A->type->size ;
    const size_t bsize = B->type->size ;

    if (zcode == GB_BOOL_code)
    { 
        // rename a boolean monoid:
        // MIN_BOOL and TIMES_BOOL monoids become LAND
        // MAX_BOOL and PLUS_BOOL monoids become LOR
        add_opcode = GB_boolean_rename (add_opcode) ;
    }

    if (xcode == GB_BOOL_code)
    { 
        // rename a boolean multiply op
        mult_opcode = GB_boolean_rename (mult_opcode) ;
    }

    // "nice" monoids have the property that reducing a set of iso values to a
    // single result doesn't change the result: ANY, LAND, LOR, BAND, BOR, MIN
    // and MAX.  That is, x == reduce ([x x x x x ... x x x x]) holds for all
    // these monoids.  The monoids that do not fall into this "nice" category
    // are PLUS, TIMES, EQ (LXNOR), LXOR, BXOR, and BXNOR.
    const bool nice_monoid =
        add_opcode == GB_ANY_opcode  ||
        add_opcode == GB_LAND_opcode || add_opcode == GB_LOR_opcode ||
        add_opcode == GB_BAND_opcode || add_opcode == GB_BOR_opcode ||
        add_opcode == GB_MAX_opcode  || add_opcode == GB_MIN_opcode ;

    // Nearly all cases where C is iso require a "nice" monoid, with the
    // exception of the EQ_PAIR and TIMES_PAIR semirings, which are the same
    // as ANY_PAIR semirings.
    const bool nice_with_pair = nice_monoid ||
        add_opcode == GB_EQ_opcode || add_opcode == GB_TIMES_opcode ;

    // the FIRST or ANY multiply ops can both produce a FIRST result
    const bool first =
        mult_opcode == GB_FIRST_opcode || mult_opcode == GB_ANY_opcode ;

    // the SECOND or ANY multiply ops can both produce a SECOND result
    const bool second =
        mult_opcode == GB_SECOND_opcode || mult_opcode == GB_ANY_opcode ;

    //--------------------------------------------------------------------------
    // determine if C is iso
    //--------------------------------------------------------------------------

    // A and B are treated as if iso if they have 1 entry and are not bitmap
    const bool A_iso = A->iso || (GB_nnz (A) == 1 && !GB_IS_BITMAP (A)) ;
    const bool B_iso = B->iso || (GB_nnz (B) == 1 && !GB_IS_BITMAP (B)) ;

    if (nice_with_pair && mult_opcode == GB_PAIR_opcode)
    { 

        //----------------------------------------------------------------------
        // C is iso, with c = 1
        //----------------------------------------------------------------------

        GB_cast_one (c, zcode) ;
        return (true) ;

    }
    else if (B_iso && nice_monoid && second)
    {

        //----------------------------------------------------------------------
        // C is iso, with c = b
        //----------------------------------------------------------------------

        if (zcode == ycode && bcode == ycode)
        { 
            // c = Bx [0]
            memcpy (c, B->x, zsize) ;
        }
        else
        { 
            // c = (ztype) ((ytype) Bx [0])
            GB_void y [GB_VLA(ysize)] ;
            GB_cast_scalar (y, ycode, B->x, bcode, bsize) ;
            GB_cast_scalar (c, zcode, y, ycode, ysize) ;
        }
        return (true) ;

    }
    else if (A_iso && nice_monoid && first)
    {

        //----------------------------------------------------------------------
        // C is iso, with c = a
        //----------------------------------------------------------------------

        if (zcode == xcode && acode == xcode)
        {
            // c = Ax [0]
            memcpy (c, A->x, zsize) ;
        }
        else
        {
            // c = (ztype) ((xtype) Ax [0])
            GB_void x [GB_VLA(xsize)] ;
            GB_cast_scalar (x, xcode, A->x, acode, asize) ;
            GB_cast_scalar (c, zcode, x, xcode, xsize) ;
        }
        return (true) ;

    }
    else if (A_iso && B_iso)
    {

        //----------------------------------------------------------------------
        // both A and B are iso
        //----------------------------------------------------------------------

        if (nice_monoid)
        {

            //------------------------------------------------------------------
            // C is iso, with c = fmult(a,b), for any fmult, incl. user-defined
            //------------------------------------------------------------------

            if (acode == xcode && bcode == ycode)
            {
                // c = fmult (Ax [0], Bx [0])
                fmult (c, A->x, B->x) ;
            }
            else
            {
                // x = (xtype) Ax [0]
                GB_void x [GB_VLA(xsize)] ;
                GB_cast_scalar (x, xcode, A->x, acode, asize) ;
                // y = (ytype) Bx [0]
                GB_void y [GB_VLA(ysize)] ;
                GB_cast_scalar (y, ycode, B->x, bcode, bsize) ;
                // c = fmult (x,y)
                fmult (c, x, y) ;
            }
            return (true) ;

        }
        else if (GB_as_if_full (A) && GB_as_if_full (B))
        {

            //------------------------------------------------------------------
            // C = A*B where A and B are both full and iso
            //------------------------------------------------------------------

            // If A and B are both full and iso, then C is also full and iso,
            // for any semiring (including user-defined) except those with a
            // positional multiplicative operator.  Each entry C(i,j) is the
            // reduction of n copies of the single iso scalar t, where t =
            // A(i,k)*B(k,j) is iso-valued for any i, j, or k, assuming n is
            // the inner dimension of the C=A*B matrix multiply.

            // first, compute t = A(i,k)*B(k,j)
            GB_void t [GB_VLA(zsize)] ;
            if (acode == xcode && bcode == ycode)
            {
                // t = fmult (Ax [0], Bx [0])
                fmult (t, A->x, B->x) ;
            }
            else
            {
                // x = (xtype) Ax [0]
                GB_void x [GB_VLA(xsize)] ;
                GB_cast_scalar (x, xcode, A->x, acode, asize) ;
                // y = (ytype) Bx [0]
                GB_void y [GB_VLA(ysize)] ;
                GB_cast_scalar (y, ycode, B->x, bcode, bsize) ;
                // c = fmult (x,y)
                fmult (t, x, y) ;
            }

            // reduce n copies of t to the single scalar c, in O(log(n))
            GxB_binary_function freduce = semiring->add->op->function ;
            GB_iso_reduce_worker (c, freduce, t, n, zsize) ;

            // the total time to compute C=A*B where all matrices are n-by-n
            // and full is thus O(log(n)), much smaller than O(n^3) for the
            // conventional matrix-multiply algorithm.  It would be possible to
            // reduce the time still further, since most reductions of n copies
            // of t can be done in O(1) time, but the O(log(n)) method works
            // for any monoid, including user-defined ones.
            return (true) ;
        }
    }

    //--------------------------------------------------------------------------
    // otherwise, C is not iso
    //--------------------------------------------------------------------------

    return (false) ;
}

