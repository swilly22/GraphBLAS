function C = gb_eadd (A, op, B)
%GB_EADD C = A+B, sparse matrix 'addition' using the given op.
% The pattern of C is the set union of A and B.  This method assumes the
% identity value of the op is zero.  That is, x+0 = x+0 = x.  The binary
% operator op is only applied to entries in the intersection of the
% pattern of A and B.
%
% The inputs A and B are MATLAB matrices or GraphBLAS structs (not GrB
% objects).  The result is a typically a GraphBLAS struct.
%
% See also GrB/plus, GrB/minus, GrB/bitxor, GrB/bitor, GrB/hypot.

% SuiteSparse:GraphBLAS, Timothy A. Davis, (c) 2017-2020, All Rights Reserved.
% http://suitesparse.com   See GraphBLAS/Doc/License.txt for license.

[am, an] = gbsize (A) ;
[bm, bn] = gbsize (B) ;
a_is_scalar = (am == 1) && (an == 1) ;
b_is_scalar = (bm == 1) && (bn == 1) ;

if (a_is_scalar)
    if (b_is_scalar)
        % both A and B are scalars.  Result is also a scalar.
        C = gbeadd (A, op, B) ;
    else
        % A is a scalar, B is a matrix.  Result is full, unless A == 0.
        if (gb_get_scalar (A) == 0)
            % C = 0+B is returned as a MATLAB matrix if B is a MATLAB matrix
            C = B ;
        else
            % expand A to a full matrix of type ctype
            ctype = gboptype (gbtype (A), gbtype (B)) ;
            A = gbsubassign (gbnew (bm, bn, ctype), A) ;
            C = gbeadd (A, op, B) ;
        end
    end
else
    if (b_is_scalar)
        % A is a matrix, B is a scalar.  Result is full, unless B == 0.
        if (gb_get_scalar (B) == 0)
            % C = A+0 is returned as a MATLAB matrix if A is a MATLAB matrix
            C = A ;
        else
            % expand B to a full matrix
            ctype = gboptype (gbtype (A), gbtype (B)) ;
            B = gbsubassign (gbnew (am, an, ctype), B) ;
            C = gbeadd (A, op, B) ;
        end
    else
        % both A and B are matrices.  Result is sparse.
        C = gbeadd (A, op, B) ;
    end
end

