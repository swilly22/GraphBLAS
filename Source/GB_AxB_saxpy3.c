//------------------------------------------------------------------------------
// GB_AxB_saxpy3: compute C=A*B, C<M>=A*B, or C<!M>=A*B in parallel
//------------------------------------------------------------------------------

// SuiteSparse:GraphBLAS, Timothy A. Davis, (c) 2017-2020, All Rights Reserved.
// http://suitesparse.com   See GraphBLAS/Doc/License.txt for license.

//------------------------------------------------------------------------------

// GB_AxB_saxpy3 computes C=A*B, C<M>=A*B, or C<!M>=A*B in parallel.  If the
// mask matrix M has too many entries compared to the work to compute A*B, then
// it is not applied.  Instead, M is ignored and C=A*B is computed.  The mask
// is applied later, in GB_mxm.

// For simplicity, this discussion and all comments in this code assume that
// all matrices are in CSC format, but the algorithm is CSR/CSC agnostic.

// The matrix B is split into two kinds of tasks: coarse and fine.  A coarse
// task computes C(:,j1:j2) = A*B(:,j1:j2), for a unique set of vectors j1:j2.
// Those vectors are not shared with any other tasks.  A fine task works with a
// team of other fine tasks to compute C(:,j) for a single vector j.  Each fine
// task computes A*B(k1:k2,j) for a unique range k1:k2, and sums its results
// into C(:,j) via atomic operations.

// Each coarse or fine task uses either Gustavson's method [1] or the Hash
// method [2].  There are 4 kinds of tasks:

//      fine Gustavson task
//      fine hash task
//      coarse Gustason task
//      coarse hash task

// Each of the 4 kinds tasks are then subdivided into 3 variants, for C=A*B,
// C<M>=A*B, and C<!M>=A*B, giving a total of 12 different types of tasks.

// Fine tasks are used when there would otherwise be too much work for a single
// task to compute the single vector C(:,j).  Fine tasks share all of their
// workspace with the team of fine tasks computing C(:,j).  Coarse tasks are
// prefered since they require less synchronization, but fine tasks allow for
// better parallelization when B has only a few vectors.  If B consists of a
// single vector (for GrB_mxv if A is in CSC format and not transposed, or
// for GrB_vxm if A is in CSR format and not transpose), then the only way to
// get parallelism is via fine tasks.  If a single thread is used for this
// case, a single-vector coarse task is used.

// To select between the Hash method or Gustavson's method for each task, the
// hash table size is first found.  The hash table size for a hash task depends
// on the maximum flop count for any vector in that task (which is just one
// vector for the fine tasks).  It is set to twice the smallest power of 2 that
// is greater than the flop count to compute that vector (plus the # of entries
// in M(:,j) for tasks that compute C<M>=A*B or C<!M>=A*B).  This size ensures
// the results will fit in the hash table, and with ideally only a modest
// number of collisions.  If the hash table size exceeds a threshold (currently
// m/16 if C is m-by-n), then Gustavson's method is used instead, and the hash
// table size is set to m, to serve as the gather/scatter workspace for
// Gustavson's method.

// The workspace allocated depends on the type of task and the type of value.
// Let s be the hash table size for the task, and C is m-by-n (assuming all
// matrices are CSC; if CSR, then m is replaced with n).  See GB_AxB_saxpy3.h
// for a list of the hash entry types (the GB_hash_* typedefs).

// Additional workspace is allocated to construct the list of tasks, but this
// is freed before C is constructed.

// References:

// [1] Fred G. Gustavson. 1978. Two Fast Algorithms for Sparse Matrices:
// Multiplication and Permuted Transposition. ACM Trans. Math. Softw.  4, 3
// (Sept. 1978), 250–269. DOI:https://doi.org/10.1145/355791.355796

// [2] Yusuke Nagasaka, Satoshi Matsuoka, Ariful Azad, and Aydın Buluç. 2018.
// High-Performance Sparse Matrix-Matrix Products on Intel KNL and Multicore
// Architectures. In Proc. 47th Intl. Conf. on Parallel Processing (ICPP '18).
// Association for Computing Machinery, New York, NY, USA, Article 34, 1–10.
// DOI:https://doi.org/10.1145/3229710.3229720

//------------------------------------------------------------------------------

#include "GB_mxm.h"
#include "GB_AxB_saxpy3.h"
#include "GB_mkl.h"
#include "GB_Global.h"
#ifndef GBCOMPACT
#include "GB_AxB__include.h"
#endif

//------------------------------------------------------------------------------
// control parameters for generating parallel tasks
//------------------------------------------------------------------------------

#define GB_NTASKS_PER_THREAD 2
#define GB_COSTLY 1.2
#define GB_FINE_WORK 2
#define GB_MWORK_ALPHA 0.01
#define GB_MWORK_BETA 0.10

//------------------------------------------------------------------------------
// free workspace
//------------------------------------------------------------------------------

// This workspace is not needed in the GB_Asaxpy3B* worker functions.
#define GB_FREE_INITIAL_WORK                                                \
{                                                                           \
    GB_FREE (Bflops2) ;                                                     \
    GB_FREE (Coarse_Work) ;                                                 \
    GB_FREE (Coarse_initial) ;                                              \
    GB_FREE (Fine_slice) ;                                                  \
}

#define GB_FREE_WORK                                                        \
{                                                                           \
    GB_FREE_INITIAL_WORK ;                                                  \
    for (int taskid = 0 ; taskid < ntasks ; taskid++)                       \
    {                                                                       \
        GB_FREE (TaskList [taskid].H) ;                                     \
        GB_FREE (TaskList [taskid].Hx) ;                                    \
    }                                                                       \
    GB_FREE (TaskList) ;                                                    \
}

#define GB_FREE_ALL                                                         \
{                                                                           \
    GB_FREE_WORK ;                                                          \
    GB_Matrix_free (Chandle) ;                                              \
}

//------------------------------------------------------------------------------
// GB_hash_table_size
//------------------------------------------------------------------------------

// flmax is the max flop count for computing A*B(:,j), for any vector j that
// this task computes.  If the mask M is present, flmax also includes the
// number of entries in M(:,j).  GB_hash_table_size determines the hash table
// size for this task, which is twice the smallest power of 2 larger than
// flmax.  If flmax is large enough, the hash_size is returned as cvlen, so
// that Gustavson's method will be used instead of the Hash method.

// By default, Gustavson vs Hash is selected automatically.  AxB_method can be
// selected via the descriptor or a global setting, as the non-default
// GxB_AxB_GUSTAVSON or GxB_AxB_HASH settings, to enforce the selection of
// either of those methods.  However, if Hash is selected by the hash table
// exceeds cvlen, then Gustavson's method is used instead.

static inline int64_t GB_hash_table_size
(
    int64_t flmax,      // max flop count for any vector computed by this task
    int64_t cvlen,      // vector length of C
    const GrB_Desc_Value AxB_method     // Default, Gustavson, or Hash
)
{
    // hash_size = 2 * (smallest power of 2 >= flmax)
    double hlog = log2 ((double) flmax) ;
    int64_t hash_size = ((int64_t) 2) << ((int64_t) floor (hlog) + 1) ;
    bool use_Gustavson ;

    if (AxB_method == GxB_AxB_GUSTAVSON)
    { 
        // always use Gustavson's method
        use_Gustavson = true ;
    }
    else if (AxB_method == GxB_AxB_HASH)
    { 
        // always use Hash method, unless the hash_size >= cvlen
        use_Gustavson = (hash_size >= cvlen) ;
    }
    else
    { 
        // default: auto selection:
        // use Gustavson's method if hash_size is too big
        use_Gustavson = (hash_size >= cvlen/16) ;
    }

    if (use_Gustavson)
    { 
        hash_size = cvlen ;
    }
    return (hash_size) ;
}

//------------------------------------------------------------------------------
// GB_create_coarse_task: create a single coarse task
//------------------------------------------------------------------------------

// Compute the max flop count for any vector in a coarse task, determine the
// hash table size, and construct the coarse task.

static inline void GB_create_coarse_task
(
    int64_t kfirst,     // coarse task consists of vectors kfirst:klast
    int64_t klast,
    GB_saxpy3task_struct *TaskList,
    int taskid,         // taskid for this coarse task
    int64_t *Bflops,    // size bnvec; cum sum of flop counts for vectors of B
    int64_t cvlen,      // vector length of B and C
    double chunk,
    int nthreads_max,
    int64_t *Coarse_Work,   // workspace for parallel reduction for flop count
    const GrB_Desc_Value AxB_method     // Default, Gustavson, or Hash
)
{
    // find the max # of flops for any vector in this task
    int64_t nk = klast - kfirst + 1 ;
    int nth = GB_nthreads (nk, chunk, nthreads_max) ;
    int64_t tid ;

    // each thread finds the max flop count for a subset of the vectors
    #pragma omp parallel for num_threads(nth) schedule(static)
    for (tid = 0 ; tid < nth ; tid++)
    {
        int64_t my_flmax = 1, istart, iend ;
        GB_PARTITION (istart, iend, nk, tid, nth) ;
        for (int64_t i = istart ; i < iend ; i++)
        {
            int64_t kk = kfirst + i ;
            int64_t fl = Bflops [kk+1] - Bflops [kk] ;
            my_flmax = GB_IMAX (my_flmax, fl) ;
        }
        Coarse_Work [tid] = my_flmax ;
    }

    // combine results from each thread
    int64_t flmax = 1 ;
    for (tid = 0 ; tid < nth ; tid++)
    {
        flmax = GB_IMAX (flmax, Coarse_Work [tid]) ;
    }

    // check the parallel computation
    #ifdef GB_DEBUG
    int64_t flmax2 = 1 ;
    for (int64_t kk = kfirst ; kk <= klast ; kk++)
    {
        int64_t fl = Bflops [kk+1] - Bflops [kk] ;
        flmax2 = GB_IMAX (flmax2, fl) ;
    }
    ASSERT (flmax == flmax2) ;
    #endif

    // define the coarse task
    TaskList [taskid].start   = kfirst ;
    TaskList [taskid].end     = klast ;
    TaskList [taskid].vector  = -1 ;
    TaskList [taskid].hsize   = GB_hash_table_size (flmax, cvlen, AxB_method) ;
    TaskList [taskid].H       = NULL ;      // assigned later
    TaskList [taskid].my_cjnz = 0 ;         // unused
    TaskList [taskid].flops   = Bflops [klast+1] - Bflops [kfirst] ;
    TaskList [taskid].master  = taskid ;    // coarse task its own master
    TaskList [taskid].team_size = 1 ;
}

//------------------------------------------------------------------------------
// GB_AxB_saxpy3: compute C=A*B, C<M>=A*B, or C<!M>=A*B in parallel
//------------------------------------------------------------------------------

GrB_Info GB_AxB_saxpy3              // C = A*B using Gustavson+Hash
(
    GrB_Matrix *Chandle,            // output matrix
    const GrB_Matrix M_input,       // optional mask matrix
    const bool Mask_comp_input,     // if true, use !M
    const bool Mask_struct,         // if true, use the only structure of M
    const GrB_Matrix A,             // input matrix A
    const GrB_Matrix B,             // input matrix B
    const GrB_Semiring semiring,    // semiring that defines C=A*B
    const bool flipxy,              // if true, do z=fmult(b,a) vs fmult(a,b)
    bool *mask_applied,             // if true, then mask was applied
    GrB_Desc_Value AxB_method,      // Default, Gustavson, or Hash
    GB_Context Context
)
{
double ttt = omp_get_wtime ( ) ;

    //--------------------------------------------------------------------------
    // check inputs
    //--------------------------------------------------------------------------

    GrB_Info info ;

    GrB_Matrix M = M_input ;        // use the mask M, until deciding otherwise
    bool Mask_comp = Mask_comp_input ;

    (*mask_applied) = false ;
    ASSERT (Chandle != NULL) ;
    ASSERT (*Chandle == NULL) ;

    ASSERT_MATRIX_OK_OR_NULL (M, "M for saxpy3 A*B", GB0) ;
    ASSERT (!GB_PENDING (M)) ;
    ASSERT (GB_JUMBLED_OK (M)) ;
    ASSERT (!GB_ZOMBIES (M)) ;

    ASSERT_MATRIX_OK (A, "A for saxpy3 A*B", GB0) ;
    ASSERT (!GB_PENDING (A)) ;
    ASSERT (GB_JUMBLED_OK (A)) ;
    ASSERT (!GB_ZOMBIES (A)) ;

    ASSERT_MATRIX_OK (B, "B for saxpy3 A*B", GB0) ;
    ASSERT (!GB_PENDING (B)) ;
    ASSERT (GB_JUMBLED_OK (B)) ;
    ASSERT (!GB_ZOMBIES (B)) ;

    ASSERT_SEMIRING_OK (semiring, "semiring for saxpy3 A*B", GB0) ;
    ASSERT (A->vdim == B->vlen) ;

    (*Chandle) = NULL ;

    //--------------------------------------------------------------------------
    // determine the # of threads to use, and the use_mkl flag
    //--------------------------------------------------------------------------

    GB_GET_NTHREADS_MAX (nthreads_max, chunk, Context) ;
    bool use_mkl = (Context == NULL) ? false : Context->use_mkl ;

    //--------------------------------------------------------------------------
    // use MKL_graph if it available and has this semiring
    //--------------------------------------------------------------------------

    // Note that GB_AxB_saxpy3 computes C=A*B where A and B treated as if CSC,
    // but MKL views the matrices as CSR.  So they are flipped below:

    #if GB_HAS_MKL_GRAPH

    if (use_mkl)
    {
        info = GB_AxB_saxpy3_mkl (
            Chandle,            // output matrix to construct
            M,                  // input mask M (may be NULL)
            Mask_comp,          // true if M is complemented
            Mask_struct,        // true if M is structural
            B,                  // first input matrix
            A,                  // second input matrix
            semiring,           // semiring that defines C=A*B
            !flipxy,            // true if multiply operator is flipped
            mask_applied,       // if true, then mask was applied
            Context) ;

        if (info != GrB_NO_VALUE)
        {
            // MKL_graph supports this semiring, and has ether computed C=A*B,
            // C<M>=A*B, or C<!M>=A*B, or has failed.
            return (info) ;
        }

        GBBURBLE ("(MKL tried) ") ;

        // If MKL_graph doesn't support this semiring, it returns GrB_NO_VALUE,
        // so fall through to use GraphBLAS, below.
    }
    #endif

    //--------------------------------------------------------------------------
    // define workspace
    //--------------------------------------------------------------------------

    int64_t *GB_RESTRICT Coarse_initial = NULL ;    // initial coarse tasks
    int64_t *GB_RESTRICT Coarse_Work = NULL ;       // workspace for flop counts
    GB_saxpy3task_struct *GB_RESTRICT TaskList = NULL ;
    int64_t *GB_RESTRICT Fine_slice = NULL ;
    int64_t *GB_RESTRICT Bflops2 = NULL ;

    int ntasks = 0 ;
    int ntasks_initial = 0 ;
    int64_t max_bjnz = 0 ;

    //--------------------------------------------------------------------------
    // get the semiring operators
    //--------------------------------------------------------------------------

    GrB_BinaryOp mult = semiring->multiply ;
    GrB_Monoid add = semiring->add ;
    ASSERT (mult->ztype == add->op->ztype) ;
    bool A_is_pattern, B_is_pattern ;
    GB_AxB_pattern (&A_is_pattern, &B_is_pattern, flipxy, mult->opcode) ;

    #ifdef GBCOMPACT
    bool is_any_pair_semiring = false ;
    #else
    GB_Opcode mult_opcode, add_opcode ;
    GB_Type_code xcode, ycode, zcode ;
    bool builtin_semiring = GB_AxB_semiring_builtin (A, A_is_pattern, B,
        B_is_pattern, semiring, flipxy, &mult_opcode, &add_opcode, &xcode,
        &ycode, &zcode) ;
    bool is_any_pair_semiring = builtin_semiring
        && (add_opcode == GB_ANY_opcode)
        && (mult_opcode == GB_PAIR_opcode) ;
    #endif

    //--------------------------------------------------------------------------
    // get A, and B
    //--------------------------------------------------------------------------

    const int64_t *GB_RESTRICT Ap = A->p ;
    const int64_t *GB_RESTRICT Ah = A->h ;
    const int64_t *GB_RESTRICT Ai = A->i ;
    const int64_t avlen = A->vlen ;
    const int64_t anvec = A->nvec ;
    const bool A_is_hyper = (Ah != NULL) ;

    const int64_t *GB_RESTRICT Bp = B->p ;
    const int64_t *GB_RESTRICT Bh = B->h ;
    const int64_t *GB_RESTRICT Bi = B->i ;
    const int64_t bvdim = B->vdim ;
    const int64_t bnz = GB_NNZ (B) ;
    const int64_t bnvec = B->nvec ;
    const int64_t bvlen = B->vlen ;
    const bool B_is_hyper = (Bh != NULL) ;

    //--------------------------------------------------------------------------
    // allocate C (just C->p and C->h, but not C->i or C->x)
    //--------------------------------------------------------------------------

    GrB_Type ctype = add->op->ztype ;
    size_t csize = ctype->size ;
    int64_t cvlen = avlen ;
    int64_t cvdim = bvdim ;
    int64_t cnvec = bnvec ;

    // calloc Cp so it can be used as the Bflops workspace
    info = GB_new (Chandle, ctype, cvlen, cvdim, GB_Ap_calloc, true,
        GB_SAME_HYPER_AS (B_is_hyper), B->hyper_ratio, cnvec, Context) ;
    if (info != GrB_SUCCESS)
    { 
        // out of memory
        GB_FREE_ALL ;
        return (info) ;
    }

    GrB_Matrix C = (*Chandle) ;

    int64_t *GB_RESTRICT Cp = C->p ;
    int64_t *GB_RESTRICT Ch = C->h ;
    if (B_is_hyper)
    { 
        // C has the same set of vectors as B
        int nth = GB_nthreads (cnvec, chunk, nthreads_max) ;
        GB_memcpy (Ch, Bh, cnvec * sizeof (int64_t), nth) ;
        C->nvec = bnvec ;
    }

    // C is constructed as sparse, not full.
    // TODO: create methods for mxm for sparse-times-full and full-times-full

ttt = omp_get_wtime ( ) - ttt ;
GB_Global_timing_add (3, ttt) ;
ttt = omp_get_wtime ( ) ;

    //==========================================================================
    // phase0: create parallel tasks
    //==========================================================================

    //--------------------------------------------------------------------------
    // compute flop counts for each vector of B and C
    //--------------------------------------------------------------------------

    int64_t Mwork = 0 ;
    int64_t *GB_RESTRICT Bflops = Cp ;  // Cp is used as workspace for Bflops
    GB_OK (GB_AxB_saxpy3_flopcount (&Mwork, Bflops, M, Mask_comp, A, B,
        Context)) ;
    int64_t total_flops = Bflops [bnvec] ;

ttt = omp_get_wtime ( ) - ttt ;
GB_Global_timing_add (4, ttt) ;
ttt = omp_get_wtime ( ) ;

    //--------------------------------------------------------------------------
    // determine if the mask M should be applied, or done later
    //--------------------------------------------------------------------------

    // If M is very large as compared to A*B, then it is too costly to apply
    // during the computation of A*B.  In this case, compute C=A*B, ignoring
    // the mask.  Tell the caller that the mask was not applied, so that it
    // will be applied later in GB_mxm.

    double axbflops = total_flops - Mwork ;
    GBBURBLE ("axbflops %g Mwork %g ", axbflops, (double) Mwork) ;
    int nth = GB_nthreads (bnvec, chunk, nthreads_max) ;

    bool M_is_dense = GB_is_dense (M) ;
    bool M_dense_in_place = false ;

    if (M_is_dense
       && (AxB_method == GxB_DEFAULT || AxB_method == GxB_AxB_SAXPY))
    { 

        // M is present but dense.  The work for M has not yet been added
        // to Bflops.

        // each vector M(:,j) has cvlen entries
        ASSERT (M != NULL) ;
        Mwork = cvlen * cvdim ;

        if (axbflops < (double) Mwork * GB_MWORK_BETA)
        { 
            // Use the hash method for all tasks.  Do not scatter the mask into
            // the H[*].f hash workspace.  The work for the mask is not
            // accounted for in Bflops, so the hash tables can be small.
            M_dense_in_place = true ;
            AxB_method = GxB_AxB_HASH ;
            GBBURBLE ("(use dense mask in place) ") ;
        }
        else
        { 
            // Use the Gustavson method for all tasks, and scatter M
            // into the fine Gustavson workspace.  The work for M is not
            // yet in the Bflops cumulative sum.  Add it now.
            AxB_method = GxB_AxB_GUSTAVSON ;

            int64_t kk ;
            #pragma omp parallel for num_threads(nth) schedule(static)
            for (kk = 0 ; kk <= bnvec ; kk++)
            { 
                Bflops [kk] += cvlen * (kk+1) ;
            }
            total_flops = Bflops [bnvec] ;
            GBBURBLE ("(use dense mask) ") ;
        }

    }
    else if ((M != NULL) && (axbflops < ((double) Mwork * GB_MWORK_ALPHA)))
    {

        // M is sparse but costly to use.  Do not use it during the computation
        // of A*B.  Instead, compute C=A*B and then apply the mask later.

        M = NULL ;
        Mask_comp = false ;

        int64_t kk ;
        // GB_AxB_saxpy3_flopcount requires Bflops be set to zero here
        #pragma omp parallel for num_threads(nth) schedule(static)
        for (kk = 0 ; kk <= bnvec ; kk++)
        { 
            Bflops [kk] = 0 ;
        }

        // redo the flop count analysis, without the mask
        GB_OK (GB_AxB_saxpy3_flopcount (&Mwork, Bflops, NULL, false, A, B,
            Context)) ;
        total_flops = Bflops [bnvec] ;
        GBBURBLE ("(discard mask) ") ;

    }
    else if (M != NULL)
    { 
        GBBURBLE ("(use mask) ") ;
    }

    //--------------------------------------------------------------------------
    // determine # of threads and # of initial coarse tasks
    //--------------------------------------------------------------------------

    int nthreads = GB_nthreads ((double) total_flops, chunk, nthreads_max) ;
    ntasks_initial = (nthreads == 1) ?  1 : (GB_NTASKS_PER_THREAD * nthreads) ;

    double target_task_size = ((double) total_flops) / ntasks_initial ;
    target_task_size = GB_IMAX (target_task_size, chunk) ;
    double target_fine_size = target_task_size / GB_FINE_WORK ;
    target_fine_size = GB_IMAX (target_fine_size, chunk) ;

    //--------------------------------------------------------------------------
    // determine # of parallel tasks
    //--------------------------------------------------------------------------

    int nfine = 0 ;         // # of fine tasks
    int ncoarse = 0 ;       // # of coarse tasks
    max_bjnz = 0 ;          // max (nnz (B (:,j))) of fine tasks

    // FUTURE: also use ultra-fine tasks that compute A(i1:i2,k)*B(k,j)

    if (ntasks_initial > 1)
    {

        //----------------------------------------------------------------------
        // construct initial coarse tasks
        //----------------------------------------------------------------------

        if (!GB_pslice (&Coarse_initial, Bflops, bnvec, ntasks_initial))
        { 
            // out of memory
            GB_FREE_ALL ;
            return (GrB_OUT_OF_MEMORY) ;
        }

        //----------------------------------------------------------------------
        // split the work into coarse and fine tasks
        //----------------------------------------------------------------------

        for (int taskid = 0 ; taskid < ntasks_initial ; taskid++)
        {
            // get the initial coarse task
            int64_t kfirst = Coarse_initial [taskid] ;
            int64_t klast  = Coarse_initial [taskid+1] ;
            int64_t task_ncols = klast - kfirst ;
            int64_t task_flops = Bflops [klast] - Bflops [kfirst] ;

            if (task_ncols == 0)
            { 
                // This coarse task is empty, having been squeezed out by
                // costly vectors in adjacent coarse tasks.
            }
            else if (task_flops > 2 * GB_COSTLY * target_task_size)
            {
                // This coarse task is too costly, because it contains one or
                // more costly vectors.  Split its vectors into a mixture of
                // coarse and fine tasks.

                int64_t kcoarse_start = kfirst ;

                for (int64_t kk = kfirst ; kk < klast ; kk++)
                {
                    // jflops = # of flops to compute a single vector A*B(:,j)
                    // where j == GBH (Bh, kk)
                    double jflops = Bflops [kk+1] - Bflops [kk] ;
                    // bjnz = nnz (B (:,j))
                    int64_t bjnz = (Bp == NULL) ? bvlen : (Bp [kk+1] - Bp [kk]);

                    if (jflops > GB_COSTLY * target_task_size && bjnz > 1)
                    {
                        // A*B(:,j) is costly; split it into 2 or more fine
                        // tasks.  First flush the prior coarse task, if any.
                        if (kcoarse_start < kk)
                        { 
                            // vectors kcoarse_start to kk-1 form a single
                            // coarse task
                            ncoarse++ ;
                        }

                        // next coarse task (if any) starts at kk+1
                        kcoarse_start = kk+1 ;

                        // vectors kk will be split into multiple fine tasks
                        max_bjnz = GB_IMAX (max_bjnz, bjnz) ;
                        int team_size = ceil (jflops / target_fine_size) ;
                        nfine += team_size ;
                    }
                }

                // flush the last coarse task, if any
                if (kcoarse_start < klast)
                { 
                    // vectors kcoarse_start to klast-1 form a single
                    // coarse task
                    ncoarse++ ;
                }

            }
            else
            { 
                // This coarse task is OK as-is.
                ncoarse++ ;
            }
        }
    }
    else
    {

        //----------------------------------------------------------------------
        // entire computation in a single fine or coarse task
        //----------------------------------------------------------------------

        if (bnvec == 1)
        { 
            // If B is a single vector, and is computed by a single thread,
            // then a single fine task is used.
            nfine = 1 ;
            ncoarse = 0 ;
        }
        else
        { 
            // One thread uses a single coarse task if B is not a vector.
            nfine = 0 ;
            ncoarse = 1 ;
        }
    }

    ntasks = ncoarse + nfine ;

    //--------------------------------------------------------------------------
    // allocate the tasks, and workspace to construct fine tasks
    //--------------------------------------------------------------------------

    TaskList    = GB_CALLOC (ntasks, GB_saxpy3task_struct) ;
    Coarse_Work = GB_MALLOC (nthreads_max, int64_t) ;
    if (max_bjnz > 0)
    { 
        // also allocate workspace to construct fine tasks
        Fine_slice = GB_MALLOC (ntasks+1  , int64_t) ;
        Bflops2    = GB_MALLOC (max_bjnz+1, int64_t) ;
    }

    if (TaskList == NULL || Coarse_Work == NULL ||
        (max_bjnz > 0 && (Fine_slice == NULL || Bflops2 == NULL)))
    { 
        // out of memory
        GB_FREE_ALL ;
        return (GrB_OUT_OF_MEMORY) ;
    }

    //--------------------------------------------------------------------------
    // create the tasks
    //--------------------------------------------------------------------------

    if (ntasks_initial > 1)
    {

        //----------------------------------------------------------------------
        // create the coarse and fine tasks
        //----------------------------------------------------------------------

        int nf = 0 ;        // fine tasks have task id 0:nfine-1
        int nc = nfine ;    // coarse task ids are nfine:ntasks-1

        for (int taskid = 0 ; taskid < ntasks_initial ; taskid++)
        {
            // get the initial coarse task
            int64_t kfirst = Coarse_initial [taskid] ;
            int64_t klast  = Coarse_initial [taskid+1] ;
            int64_t task_ncols = klast - kfirst ;
            int64_t task_flops = Bflops [klast] - Bflops [kfirst] ;

            if (task_ncols == 0)
            { 
                // This coarse task is empty, having been squeezed out by
                // costly vectors in adjacent coarse tasks.
            }
            else if (task_flops > 2 * GB_COSTLY * target_task_size)
            {
                // This coarse task is too costly, because it contains one or
                // more costly vectors.  Split its vectors into a mixture of
                // coarse and fine tasks.

                int64_t kcoarse_start = kfirst ;

                for (int64_t kk = kfirst ; kk < klast ; kk++)
                {
                    // jflops = # of flops to compute a single vector A*B(:,j)
                    double jflops = Bflops [kk+1] - Bflops [kk] ;
                    // bjnz = nnz (B (:,j))
                    int64_t bjnz = (Bp == NULL) ? bvlen : (Bp [kk+1] - Bp [kk]);

                    if (jflops > GB_COSTLY * target_task_size && bjnz > 1)
                    {
                        // A*B(:,j) is costly; split it into 2 or more fine
                        // tasks.  First flush the prior coarse task, if any.
                        if (kcoarse_start < kk)
                        { 
                            // kcoarse_start:kk-1 form a single coarse task
                            GB_create_coarse_task (kcoarse_start, kk-1,
                                TaskList, nc++, Bflops, cvlen,
                                chunk, nthreads_max, Coarse_Work, AxB_method) ;
                        }

                        // next coarse task (if any) starts at kk+1
                        kcoarse_start = kk+1 ;

                        // count the work for each entry B(k,j).  Do not
                        // include the work to scan M(:,j), since that will
                        // be evenly divided between all tasks in this team.
                        int64_t pB_start = GBP (Bp, kk, bvlen) ;
                        int nth = GB_nthreads (bjnz, chunk, nthreads_max) ;
                        int64_t s ;
                        #pragma omp parallel for num_threads(nth) \
                            schedule(static)
                        for (s = 0 ; s < bjnz ; s++)
                        {
                            // get B(k,j)
                            int64_t k = GBI (Bi, pB_start + s, bvlen) ;
                            // fl = flop count for just A(:,k)*B(k,j)
                            int64_t pA, pA_end ;
                            int64_t pleft = 0 ;
                            GB_lookup (A_is_hyper, Ah, Ap, avlen, &pleft,
                                anvec-1, k, &pA, &pA_end) ;
                            int64_t fl = pA_end - pA ;
                            Bflops2 [s] = fl ;
                            ASSERT (fl >= 0) ;
                        }

                        // cumulative sum of flops to compute A*B(:,j)
                        GB_cumsum (Bflops2, bjnz, NULL, nth) ;

                        // slice B(:,j) into fine tasks
                        int team_size = ceil (jflops / target_fine_size) ;
                        ASSERT (Fine_slice != NULL) ;
                        GB_pslice (&Fine_slice, Bflops2, bjnz, team_size) ;

                        // shared hash table for all fine tasks for A*B(:,j)
                        int64_t hsize = 
                            GB_hash_table_size (jflops, cvlen, AxB_method) ;

                        // construct the fine tasks for C(:,j)=A*B(:,j)
                        int master = nf ;
                        for (int fid = 0 ; fid < team_size ; fid++)
                        { 
                            int64_t pstart = Fine_slice [fid] ;
                            int64_t pend   = Fine_slice [fid+1] ;
                            int64_t fl = Bflops2 [pend] - Bflops2 [pstart] ;
                            TaskList [nf].start  = pB_start + pstart ;
                            TaskList [nf].end    = pB_start + pend - 1 ;
                            TaskList [nf].vector = kk ;
                            TaskList [nf].hsize  = hsize ;
                            TaskList [nf].H = NULL ;   // assigned later
                            TaskList [nf].my_cjnz = 0 ;
                            TaskList [nf].flops = fl ;
                            TaskList [nf].master = master ;
                            TaskList [nf].team_size = team_size ;
                            nf++ ;
                        }
                    }
                }

                // flush the last coarse task, if any
                if (kcoarse_start < klast)
                { 
                    // kcoarse_start:klast-1 form a single coarse task
                    GB_create_coarse_task (kcoarse_start, klast-1, TaskList,
                        nc++, Bflops, cvlen, chunk, nthreads_max, Coarse_Work,
                        AxB_method) ;
                }

            }
            else
            { 
                // This coarse task is OK as-is.
                GB_create_coarse_task (kfirst, klast-1, TaskList, nc++, Bflops,
                    cvlen, chunk, nthreads_max, Coarse_Work, AxB_method) ;
            }
        }

    }
    else
    {

        //----------------------------------------------------------------------
        // entire computation in a single fine or coarse task
        //----------------------------------------------------------------------

        // create a single coarse task
        GB_create_coarse_task (0, bnvec-1, TaskList, 0, Bflops, cvlen, 1, 1,
            Coarse_Work, AxB_method) ;

        if (bnvec == 1)
        { 
            // convert the single coarse task into a single fine task
            TaskList [0].start  = 0 ;           // first entry in B(:,0)
            TaskList [0].end    = bnz - 1 ;     // last entry in B(:,0)
            TaskList [0].vector = 0 ;
        }
    }

    //--------------------------------------------------------------------------
    // free workspace used to create the tasks
    //--------------------------------------------------------------------------

    // Frees Bflops2, Coarse_initial, Coarse_Work, and Fine_slice.  These do
    // not need to be freed in the GB_Asaxpy3B worker below.

    GB_FREE_INITIAL_WORK ;

    //--------------------------------------------------------------------------

    #if GB_BURBLE
    if (GB_Global_burble_get ( ))
    {
        int nfine_hash = 0 ;
        int nfine_gus = 0 ;
        int ncoarse_hash = 0 ;
        int ncoarse_gus = 0 ;
        for (int taskid = 0 ; taskid < ntasks ; taskid++)
        {
            int64_t hash_size = TaskList [taskid].hsize ;
            bool is_fine = (taskid < nfine) ;
            bool use_Gustavson = (hash_size == cvlen) ;
            if (is_fine)
            {
                // fine task
                if (use_Gustavson)
                {
                    // fine Gustavson task
                    nfine_gus++ ;
                }
                else
                {
                    // fine hash task
                    nfine_hash++ ;
                }
            }
            else
            {
                // coarse task
                int64_t kfirst = TaskList [taskid].start ;
                int64_t klast = TaskList [taskid].end ;
                if (use_Gustavson)
                {
                    // coarse Gustavson task
                    ncoarse_gus++ ;
                }
                else
                {
                    // hash task
                    ncoarse_hash++ ;
                }
            }
        }
        GBBURBLE ("nthreads %d ntasks %d coarse: (gus: %d hash: %d)"
            " fine: (gus: %d hash: %d) ", nthreads, ntasks,
            ncoarse_gus, ncoarse_hash, nfine_gus, nfine_hash) ;
    }
    #endif

    // Bflops is no longer needed as an alias for Cp
    Bflops = NULL ;

    //--------------------------------------------------------------------------
    // allocate the hash tables
    //--------------------------------------------------------------------------

    // If Gustavson's method is used (coarse tasks):
    //
    //      hash_size is cvlen.
    //      H [*].i is in the hash table entry.
    //      H [*].f and H [*].x are both of size hash_size.
    //
    //      (H [i].f == mark) is true if i is in the hash table.
    //      H [i].x is the value of C(i,j) during the numeric phase.
    //
    //      Gustavson's method is used if the hash_size for the Hash method
    //      is a significant fraction of cvlen. 
    //
    // If the Hash method is used (coarse tasks):
    //
    //      hash_size is 2 times the smallest power of 2 that is larger than
    //      the # of flops required for any column C(:,j) being computed.  This
    //      ensures that all entries have space in the hash table, and that the
    //      hash occupancy will never be more than 50%.  It is always smaller
    //      than cvlen (otherwise, Gustavson's method is used).
    //
    //      A hash function is used for the ith entry:
    //          hash = GB_HASH_FUNCTION (i) ; in range 0 to hash_size-1
    //      If a collision occurs, linear probing is used:
    //          hash = (hash + 1) & (hash_size-1)
    //
    //      (H [hash].f == mark) is true if the position is occupied.
    //      i = H [hash].i gives the row index i that occupies that position.
    //      H [hash].x is the value of C(i,j) during the numeric phase.
    //
    // For both coarse methods:
    //
    //      H [*].f starts out all zero (via calloc), and mark starts out as 1.
    //      To clear H [*].f, mark is incremented, so that all entries in H
    //      [*].f are not equal to mark.

    GB_Type_code ccode = (is_any_pair_semiring) ? GB_ignore_code : ctype->code ;
    bool ok = true ;

    #define GB_HSIZE(T) s = sizeof (T) ; break ;

    // allocate workspace for fine tasks
    for (int taskid = 0 ; taskid < nfine ; taskid++)
    {
        if (taskid != TaskList [taskid].master)
        { 
            // allocate a single shared hash table for all fine
            // tasks that compute a single C(:,j)
            continue ;
        }
        int64_t hash_size = TaskList [taskid].hsize ;
        bool use_Gustavson = (hash_size == cvlen) ;
        size_t s ;
        if (use_Gustavson)
        {
            // fine Gustavson using GB_HASH_FINEGUS
            switch (ccode)
            {
                case GB_BOOL_code   : GB_HSIZE (GB_hash_fineGus_bool) ;
                case GB_INT8_code   : GB_HSIZE (GB_hash_fineGus_int8_t) ;
                case GB_INT16_code  : GB_HSIZE (GB_hash_fineGus_int16_t) ;
                case GB_INT32_code  : GB_HSIZE (GB_hash_fineGus_int32_t) ;
                case GB_INT64_code  : GB_HSIZE (GB_hash_fineGus_int64_t) ;
                case GB_UINT8_code  : GB_HSIZE (GB_hash_fineGus_uint8_t) ;
                case GB_UINT16_code : GB_HSIZE (GB_hash_fineGus_uint16_t) ;
                case GB_UINT32_code : GB_HSIZE (GB_hash_fineGus_uint32_t) ;
                case GB_UINT64_code : GB_HSIZE (GB_hash_fineGus_uint64_t) ;
                case GB_FP32_code   : GB_HSIZE (GB_hash_fineGus_float) ;
                case GB_FP64_code   : GB_HSIZE (GB_hash_fineGus_double) ;
                case GB_FC32_code   : GB_HSIZE (GB_hash_fineGus_GxB_FC32_t) ;
                case GB_FC64_code   : GB_HSIZE (GB_hash_fineGus_GxB_FC64_t) ;
                default             : GB_HSIZE (GB_hash_fineGus_GB_void) ;
            }
        }
        else
        {
            // fine hash using GB_HASH_TYPE
            switch (ccode)
            {
                case GB_BOOL_code   : GB_HSIZE (GB_hash_bool) ;
                case GB_INT8_code   : GB_HSIZE (GB_hash_int8_t) ;
                case GB_INT16_code  : GB_HSIZE (GB_hash_int16_t) ;
                case GB_INT32_code  : GB_HSIZE (GB_hash_int32_t) ;
                case GB_INT64_code  : GB_HSIZE (GB_hash_int64_t) ;
                case GB_UINT8_code  : GB_HSIZE (GB_hash_uint8_t) ;
                case GB_UINT16_code : GB_HSIZE (GB_hash_uint16_t) ;
                case GB_UINT32_code : GB_HSIZE (GB_hash_uint32_t) ;
                case GB_UINT64_code : GB_HSIZE (GB_hash_uint64_t) ;
                case GB_FP32_code   : GB_HSIZE (GB_hash_float) ;
                case GB_FP64_code   : GB_HSIZE (GB_hash_double) ;
                case GB_FC32_code   : GB_HSIZE (GB_hash_GxB_FC32_t) ;
                case GB_FC64_code   : GB_HSIZE (GB_hash_GxB_FC64_t) ;
                default             : GB_HSIZE (GB_hash_GB_void) ;
            }
        }
        TaskList [taskid].H = (GB_void *) GB_calloc_memory (hash_size, s) ;
        ok = ok && (TaskList [taskid].H != NULL) ;
        if (ccode == GB_UDT_code)
        { 
            // allocate Hx for user-defined types
            TaskList [taskid].Hx = (GB_void *) GB_malloc_memory (hash_size,
                csize) ;
            ok = ok && (TaskList [taskid].Hx != NULL) ;
        }
    }

    // allocate workspace for coarse tasks
    for (int taskid = nfine ; taskid < ntasks ; taskid++)
    {
        int64_t hash_size = TaskList [taskid].hsize ;
        bool use_Gustavson = (hash_size == cvlen) ;
        ASSERT (taskid == TaskList [taskid].master) ;
        size_t s ;
        if (use_Gustavson)
        {
            // coarse Gustavson using GB_HASH_TYPE
            switch (ccode)
            {
                case GB_BOOL_code   : GB_HSIZE (GB_hash_bool) ;
                case GB_INT8_code   : GB_HSIZE (GB_hash_int8_t) ;
                case GB_INT16_code  : GB_HSIZE (GB_hash_int16_t) ;
                case GB_INT32_code  : GB_HSIZE (GB_hash_int32_t) ;
                case GB_INT64_code  : GB_HSIZE (GB_hash_int64_t) ;
                case GB_UINT8_code  : GB_HSIZE (GB_hash_uint8_t) ;
                case GB_UINT16_code : GB_HSIZE (GB_hash_uint16_t) ;
                case GB_UINT32_code : GB_HSIZE (GB_hash_uint32_t) ;
                case GB_UINT64_code : GB_HSIZE (GB_hash_uint64_t) ;
                case GB_FP32_code   : GB_HSIZE (GB_hash_float) ;
                case GB_FP64_code   : GB_HSIZE (GB_hash_double) ;
                case GB_FC32_code   : GB_HSIZE (GB_hash_GxB_FC32_t) ;
                case GB_FC64_code   : GB_HSIZE (GB_hash_GxB_FC64_t) ;
                default             : GB_HSIZE (GB_hash_GB_void) ;
            }
        }
        else
        {
            // coarse hash using GB_HASH_COARSE
            switch (ccode)
            {
                case GB_BOOL_code   : GB_HSIZE (GB_hash_coarse_bool) ;
                case GB_INT8_code   : GB_HSIZE (GB_hash_coarse_int8_t) ;
                case GB_INT16_code  : GB_HSIZE (GB_hash_coarse_int16_t) ;
                case GB_INT32_code  : GB_HSIZE (GB_hash_coarse_int32_t) ;
                case GB_INT64_code  : GB_HSIZE (GB_hash_coarse_int64_t) ;
                case GB_UINT8_code  : GB_HSIZE (GB_hash_coarse_uint8_t) ;
                case GB_UINT16_code : GB_HSIZE (GB_hash_coarse_uint16_t) ;
                case GB_UINT32_code : GB_HSIZE (GB_hash_coarse_uint32_t) ;
                case GB_UINT64_code : GB_HSIZE (GB_hash_coarse_uint64_t) ;
                case GB_FP32_code   : GB_HSIZE (GB_hash_coarse_float) ;
                case GB_FP64_code   : GB_HSIZE (GB_hash_coarse_double) ;
                case GB_FC32_code   : GB_HSIZE (GB_hash_coarse_GxB_FC32_t) ;
                case GB_FC64_code   : GB_HSIZE (GB_hash_coarse_GxB_FC64_t) ;
                default             : GB_HSIZE (GB_hash_coarse_GB_void) ;
            }
        }
        TaskList [taskid].H = (GB_void *) GB_calloc_memory (hash_size, s) ;
        ok = ok && (TaskList [taskid].H != NULL) ;
        if (ccode == GB_UDT_code)
        { 
            // allocate Hx for user-defined types
            TaskList [taskid].Hx = (GB_void *) GB_malloc_memory (hash_size,
                csize) ;
            ok = ok && (TaskList [taskid].Hx != NULL) ;
        }
    }

    if (!ok)
    { 
        // out of memory
        GB_FREE_ALL ;
        return (info) ;
    }

    //==========================================================================
    // phase1: symbolic analysis
    //==========================================================================

ttt = omp_get_wtime ( ) - ttt ;
GB_Global_timing_add (5, ttt) ;
ttt = omp_get_wtime ( ) ;

    GB_AxB_saxpy3_symbolic (C, M, Mask_comp, Mask_struct, M_dense_in_place,
        A, B, TaskList, ntasks, nfine, nthreads) ;

ttt = omp_get_wtime ( ) - ttt ;
GB_Global_timing_add (6, ttt) ;
ttt = omp_get_wtime ( ) ;

    //==========================================================================
    // C = A*B, via saxpy3 method and built-in semiring
    //==========================================================================

    bool done = false ;

#if 1
    #ifndef GBCOMPACT

        //----------------------------------------------------------------------
        // define the worker for the switch factory
        //----------------------------------------------------------------------

        #define GB_Asaxpy3B(add,mult,xname) \
            GB_Asaxpy3B_ ## add ## mult ## xname

        #define GB_AxB_WORKER(add,mult,xname)                               \
        {                                                                   \
            info = GB_Asaxpy3B (add,mult,xname) (C, M, Mask_comp,           \
                Mask_struct, M_dense_in_place, A, A_is_pattern,  B,         \
                B_is_pattern, TaskList, ntasks, nfine, nthreads, Context) ; \
            done = (info != GrB_NO_VALUE) ;                                 \
        }                                                                   \
        break ;

        //----------------------------------------------------------------------
        // launch the switch factory
        //----------------------------------------------------------------------

        if (builtin_semiring)
        { 
            #include "GB_AxB_factory.c"
        }

    #endif
#endif

    //==========================================================================
    // C = A*B, via the generic saxpy3 method, with typecasting
    //==========================================================================

    if (!done)
    { 
        info = GB_AxB_saxpy3_generic (C, M, Mask_comp, Mask_struct,
            M_dense_in_place, A, A_is_pattern, B, B_is_pattern, semiring,
            flipxy, TaskList, ntasks, nfine, nthreads, Context) ;
    }

    if (info != GrB_SUCCESS)
    { 
        // out of memory
        GB_FREE_ALL ;
        return (GrB_OUT_OF_MEMORY) ;
    }

    //==========================================================================
    // prune empty vectors, free workspace, and return result
    //==========================================================================

ttt = omp_get_wtime ( ) - ttt ;
GB_Global_timing_add (7, ttt) ;
ttt = omp_get_wtime ( ) ;

    GB_FREE_WORK ;
    info = GB_hypermatrix_prune (C, Context) ;
    if (info == GrB_SUCCESS) { ASSERT_MATRIX_OK (C, "saxpy3: output", GB0) ; }
    ASSERT (*Chandle == C) ;
    ASSERT (!GB_ZOMBIES (C)) ;
    ASSERT (!GB_PENDING (C)) ;
    (*mask_applied) = (M != NULL) ;

ttt = omp_get_wtime ( ) - ttt ;
GB_Global_timing_add (8, ttt) ;

    return (info) ;
}

