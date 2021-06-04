//------------------------------------------------------------------------------
// GB_AxB_semiring_builtin:  determine if semiring is built-in
//------------------------------------------------------------------------------

// SuiteSparse:GraphBLAS, Timothy A. Davis, (c) 2017-2021, All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

//------------------------------------------------------------------------------

// Determine if A*B uses a built-in semiring, and if so, determine the
// opcodes and type codes of the semiring.

// This function is not used by the CUDA jitified kernels, since they can
// typecast the entries in the matrices A and B to the types of x and y of the
// operator, as needed.

#include "GB_mxm.h"
#include "GB_binop.h"

bool GB_AxB_semiring_builtin        // true if semiring is builtin
(
    // inputs:
    const GrB_Matrix A,
    const bool A_is_pattern,        // true if only the pattern of A is used
    const GrB_Matrix B,
    const bool B_is_pattern,        // true if only the pattern of B is used
    const GrB_Semiring semiring,    // semiring that defines C=A*B
    const bool flipxy,              // true if z=fmult(y,x), flipping x and y
    // outputs:
    GB_Opcode *mult_opcode,         // multiply opcode
    GB_Opcode *add_opcode,          // add opcode
    GB_Type_code *xcode,            // type code for x input
    GB_Type_code *ycode,            // type code for y input
    GB_Type_code *zcode             // type code for z output
)
{

    //--------------------------------------------------------------------------
    // check inputs
    //--------------------------------------------------------------------------

    // A and B may be aliased

    GrB_BinaryOp add  = semiring->add->op ;     // add operator
    GrB_BinaryOp mult = semiring->multiply ;    // multiply operator

    // add is a monoid
    ASSERT (add->xtype == add->ztype && add->ytype == add->ztype) ;
    ASSERT (!GB_OP_IS_POSITIONAL (add)) ;

    // in a semiring, the ztypes of add and mult are always the same:
    ASSERT (add->ztype == mult->ztype) ;

    // The conditions above are true for any semiring and any A and B, whether
    // or not this function handles the semiring as hard-coded.  Now return for
    // cases this function does not handle.

    (*mult_opcode) = GB_NOP_opcode ;
    (*xcode) = GB_ignore_code ;
    (*ycode) = GB_ignore_code ;
    (*zcode) = GB_ignore_code ;

    //--------------------------------------------------------------------------
    // check the monoid
    //--------------------------------------------------------------------------

    (*add_opcode) = add->opcode ;
    if (*add_opcode >= GB_USER_opcode)
    { 
        // semiring has a user-defined add operator for its monoid
        return (false) ;
    }

    //--------------------------------------------------------------------------
    // rename redundant boolean monoids
    //--------------------------------------------------------------------------

    if (add->ztype->code == GB_BOOL_code)
    { 
        // Only the LAND, LOR, LXOR, and EQ monoids remain if z is
        // boolean.  MIN, MAX, PLUS, and TIMES are renamed.
        (*add_opcode) = GB_boolean_rename (*add_opcode) ;
    }

    //--------------------------------------------------------------------------
    // check the multiply operator
    //--------------------------------------------------------------------------

    if (!GB_binop_builtin (A->type, A_is_pattern, B->type, B_is_pattern,
        mult, flipxy, mult_opcode, xcode, ycode, zcode))
    { 
        return (false) ;
    }

    //--------------------------------------------------------------------------
    // rename to ANY_PAIR
    //--------------------------------------------------------------------------

    if ((*mult_opcode) == GB_PAIR_opcode)
    { 
        if (((*add_opcode) == GB_EQ_opcode) ||
            ((*add_opcode) == GB_LAND_opcode) ||
            ((*add_opcode) == GB_BAND_opcode) ||
            ((*add_opcode) == GB_LOR_opcode) ||
            ((*add_opcode) == GB_BOR_opcode) ||
            ((*add_opcode) == GB_MAX_opcode) ||
            ((*add_opcode) == GB_MIN_opcode) ||
            ((*add_opcode) == GB_TIMES_opcode))
        // rename to ANY_PAIR
        (*add_opcode) = GB_PAIR_opcode ;
    }

    return (true) ;
}

