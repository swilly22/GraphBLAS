//------------------------------------------------------------------------------
// GB_subassign_05: C(I,J)<M> = scalar ; no S
//------------------------------------------------------------------------------

// SuiteSparse:GraphBLAS, Timothy A. Davis, (c) 2017-2021, All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

//------------------------------------------------------------------------------

// Method 05: C(I,J)<M> = scalar ; no S

// M:           present
// Mask_comp:   false
// C_replace:   false
// accum:       NULL
// A:           scalar
// S:           none

// C: not bitmap
// M: any sparsity

#include "GB_subassign_methods.h"

GrB_Info GB_subassign_05
(
    GrB_Matrix C,
    // input:
    const GrB_Index *I,
    const int64_t nI,
    const int Ikind,
    const int64_t Icolon [3],
    const GrB_Index *J,
    const int64_t nJ,
    const int Jkind,
    const int64_t Jcolon [3],
    const GrB_Matrix M,
    const bool Mask_struct,
    const void *scalar,
    const GrB_Type atype,
    GB_Context Context
)
{

    //--------------------------------------------------------------------------
    // check inputs
    //--------------------------------------------------------------------------

    ASSERT (!GB_IS_BITMAP (C)) ;
    ASSERT (!GB_aliased (C, M)) ;   // NO ALIAS of C==M

    //--------------------------------------------------------------------------
    // get inputs
    //--------------------------------------------------------------------------

    GB_EMPTY_TASKLIST ;
    GB_MATRIX_WAIT_IF_JUMBLED (C) ;
    GB_MATRIX_WAIT_IF_JUMBLED (M) ;

    GB_GET_C ;      // C must not be bitmap
    int64_t zorig = C->nzombies ;
    const int64_t *restrict Ch = C->h ;
    const int64_t *restrict Cp = C->p ;
    const bool C_is_hyper = (Ch != NULL) ;
    const int64_t Cnvec = C->nvec ;
    GB_GET_MASK ;
    GB_GET_SCALAR ;
    GrB_BinaryOp accum = NULL ;

    //--------------------------------------------------------------------------
    // Method 05: C(I,J)<M> = scalar ; no S
    //--------------------------------------------------------------------------

    // Time: Close to Optimal:  the method must iterate over all entries in M,
    // so the time is Omega(nnz(M)).  For each entry M(i,j)=1, the
    // corresponding entry in C must be found and updated (inserted or
    // modified).  This method does this with a binary search of C(:,jC) or a
    // direct lookup if C(:,jC) is dense.  The time is thus O(nnz(M)*log(n)) in
    // the worst case, usually less than that since C(:,jC) often has O(1)
    // entries.  An additional time of O(|J|*log(Cnvec)) is added if C is
    // hypersparse.  There is no equivalent method that computes
    // C(I,J)<M>=scalar using the matrix S.

    // Method 05 and Method 07 are very similar.  Also compare with Method 06n.

    //--------------------------------------------------------------------------
    // Parallel: slice M into coarse/fine tasks (Method 05, 06n, 07)
    //--------------------------------------------------------------------------

    GB_SUBASSIGN_ONE_SLICE (M) ;    // M cannot be jumbled 

    //--------------------------------------------------------------------------
    // phase 1: undelete zombies, update entries, and count pending tuples
    //--------------------------------------------------------------------------

    #pragma omp parallel for num_threads(nthreads) schedule(dynamic,1) \
        reduction(+:nzombies)
    for (taskid = 0 ; taskid < ntasks ; taskid++)
    {

        //----------------------------------------------------------------------
        // get the task descriptor
        //----------------------------------------------------------------------

        GB_GET_TASK_DESCRIPTOR_PHASE1 ;

        //----------------------------------------------------------------------
        // compute all vectors in this task
        //----------------------------------------------------------------------

        for (int64_t k = kfirst ; k <= klast ; k++)
        {

            //------------------------------------------------------------------
            // get j, the kth vector of M
            //------------------------------------------------------------------

            int64_t j = GBH (Mh, k) ;
            GB_GET_VECTOR (pM, pM_end, pA, pA_end, Mp, k, Mvlen) ;
            int64_t mjnz = pM_end - pM ;
            if (mjnz == 0) continue ;

            //------------------------------------------------------------------
            // get jC, the corresponding vector of C
            //------------------------------------------------------------------

            GB_GET_jC ;
            int64_t cjnz = pC_end - pC_start ;
            bool cjdense = (cjnz == Cvlen) ;

            //------------------------------------------------------------------
            // C(I,jC)<M(:,j)> = scalar ; no S
            //------------------------------------------------------------------

            if (cjdense)
            {

                //--------------------------------------------------------------
                // C(:,jC) is dense so the binary search of C is not needed
                //--------------------------------------------------------------

                for ( ; pM < pM_end ; pM++)
                {

                    //----------------------------------------------------------
                    // update C(iC,jC), but only if M(iA,j) allows it
                    //----------------------------------------------------------

                    bool mij = GBB (Mb, pM) && GB_mcast (Mx, pM, msize) ;
                    if (mij)
                    { 
                        int64_t iA = GBI (Mi, pM, Mvlen) ;
                        GB_iC_DENSE_LOOKUP ;

                        // ----[C A 1] or [X A 1]-------------------------------
                        // [C A 1]: action: ( =A ): copy A into C, no accum
                        // [X A 1]: action: ( undelete ): zombie lives
                        GB_noaccum_C_A_1_scalar ;
                    }
                }

            }
            else
            {

                //--------------------------------------------------------------
                // C(:,jC) is sparse; use binary search for C
                //--------------------------------------------------------------

                for ( ; pM < pM_end ; pM++)
                {

                    //----------------------------------------------------------
                    // update C(iC,jC), but only if M(iA,j) allows it
                    //----------------------------------------------------------

                    bool mij = GBB (Mb, pM) && GB_mcast (Mx, pM, msize) ;
                    if (mij)
                    {
                        int64_t iA = GBI (Mi, pM, Mvlen) ;

                        // find C(iC,jC) in C(:,jC)
                        GB_iC_BINARY_SEARCH ;
                        if (cij_found)
                        { 
                            // ----[C A 1] or [X A 1]---------------------------
                            // [C A 1]: action: ( =A ): copy A into C, no accum
                            // [X A 1]: action: ( undelete ): zombie lives
                            GB_noaccum_C_A_1_scalar ;
                        }
                        else
                        { 
                            // ----[. A 1]--------------------------------------
                            // [. A 1]: action: ( insert )
                            task_pending++ ;
                        }
                    }
                }
            }
        }

        GB_PHASE1_TASK_WRAPUP ;
    }

    //--------------------------------------------------------------------------
    // phase 2: insert pending tuples
    //--------------------------------------------------------------------------

    GB_PENDING_CUMSUM ;
    zorig = C->nzombies ;

    #pragma omp parallel for num_threads(nthreads) schedule(dynamic,1) \
        reduction(&&:pending_sorted)
    for (taskid = 0 ; taskid < ntasks ; taskid++)
    {

        //----------------------------------------------------------------------
        // get the task descriptor
        //----------------------------------------------------------------------

        GB_GET_TASK_DESCRIPTOR_PHASE2 ;

        //----------------------------------------------------------------------
        // compute all vectors in this task
        //----------------------------------------------------------------------

        for (int64_t k = kfirst ; k <= klast ; k++)
        {

            //------------------------------------------------------------------
            // get j, the kth vector of M
            //------------------------------------------------------------------

            int64_t j = GBH (Mh, k) ;
            GB_GET_VECTOR (pM, pM_end, pA, pA_end, Mp, k, Mvlen) ;
            int64_t mjnz = pM_end - pM ;
            if (mjnz == 0) continue ;

            //------------------------------------------------------------------
            // get jC, the corresponding vector of C
            //------------------------------------------------------------------

            GB_GET_jC ;
            bool cjdense = ((pC_end - pC_start) == Cvlen) ;

            //------------------------------------------------------------------
            // C(I,jC)<M(:,j)> = scalar ; no S
            //------------------------------------------------------------------

            if (!cjdense)
            {

                //--------------------------------------------------------------
                // C(:,jC) is sparse; use binary search for C
                //--------------------------------------------------------------

                for ( ; pM < pM_end ; pM++)
                {

                    //----------------------------------------------------------
                    // update C(iC,jC), but only if M(iA,j) allows it
                    //----------------------------------------------------------

                    bool mij = GBB (Mb, pM) && GB_mcast (Mx, pM, msize) ;
                    if (mij)
                    {
                        int64_t iA = GBI (Mi, pM, Mvlen) ;

                        // find C(iC,jC) in C(:,jC)
                        GB_iC_BINARY_SEARCH ;
                        if (!cij_found)
                        { 
                            // ----[. A 1]--------------------------------------
                            // [. A 1]: action: ( insert )
                            GB_PENDING_INSERT (scalar) ;
                        }
                    }
                }
            }
        }

        GB_PHASE2_TASK_WRAPUP ;
    }

    //--------------------------------------------------------------------------
    // finalize the matrix and return result
    //--------------------------------------------------------------------------

//     GB_SUBASSIGN_WRAPUP ;
// FIXME


    if (pending_sorted)                                                     
    {                                                                       
        for (int taskid = 0 ; pending_sorted && taskid < ntasks ; taskid++)  
        {                                                                    
            int64_t n = Npending [taskid] ;                                  
            int64_t task_pending = Npending [taskid+1] - n ;                 
            n += npending_orig ;                                             
            if (task_pending > 0 && n > 0)                                   
            {                                                                
                /* (i,j) is the first pending tuple for this task; check */  
                /* with the pending tuple just before it                 */  
                ASSERT (n < npending_orig + nnew) ;                          
                int64_t i = Pending_i [n] ;                                  
                int64_t j = (Pending_j != NULL) ? Pending_j [n] : 0 ;        
                int64_t ilast = Pending_i [n-1] ;                            
                int64_t jlast = (Pending_j != NULL) ? Pending_j [n-1] : 0 ;  
                pending_sorted = pending_sorted &&                           
                    ((jlast < j) || (jlast == j && ilast <= i)) ;            
            }                                                                
        }                                                                    
    }                                                                        
    Pending->n += nnew ;                                                     
    Pending->sorted = pending_sorted ;                                       
    GB_FREE_ALL ;                                                            
    ASSERT_MATRIX_OK (C, "C with pending tuples :"__FILE__, GB_FLIP (GB0)) ; 
    return (GrB_SUCCESS) ;

}

