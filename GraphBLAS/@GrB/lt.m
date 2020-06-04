function C = lt (A, B)
%A < B Less than.
% C = (A < B) is an element-by-element comparison of A and B.  One or
% both may be scalars.  Otherwise, A and B must have the same size.
%
% The input matrices may be either GraphBLAS and/or MATLAB matrices, in
% any combination.  C is returned as a logical GraphBLAS matrix.
%
% See also GrB/le, GrB/gt, GrB/ge, GrB/ne, GrB/eq.

% The pattern of C depends on the type of inputs:
% A scalar, B scalar:  C is scalar.
% A scalar, B matrix:  C is full if A<0, otherwise C is a subset of B.
% B scalar, A matrix:  C is full if B>0, otherwise C is a subset of A.
% A matrix, B matrix:  C has the pattern of the set union, A+B.

% SuiteSparse:GraphBLAS, Timothy A. Davis, (c) 2017-2020, All Rights
% Reserved. http://suitesparse.com.  See GraphBLAS/Doc/License.txt.

if (~isreal (A))
    A = real (A) ;
end

if (~isreal (B))
    B = real (B) ;
end

ctype = GrB.optype (A, B) ;

if (isscalar (A))
    if (isscalar (B))
        % both A and B are scalars
        C = GrB (gb_union_op ('<', gb (A), gb (B))) ;
    else
        % A is a scalar, B is a matrix
        [m, n] = size (B) ;
        if (gb_get_scalar (A) < 0)
            if (~GrB.issigned (B))
                % a < 0, and B has an unsigned type.  C is all true.
                C = true (m, n, 'like', GrB (true)) ;
            else
                % since a < 0, entries not present in B result in a true
                % value, so the result is dense.  Expand A to dense.
                % A (1:m,1:n) = A and cast ctype
                A = GrB.subassign (GrB (m, n, ctype), A) ;
                B = gb_full (B, ctype) ;
                C = GrB.emult (A, '<', B) ;
            end
        else
            % since a >= 0, entries not present in B result in a false
            % value, so the result is a sparse subset of B.  select all
            % entries in B > a, then convert to true.
            C = GrB.apply ('1.logical', GrB.select (B, '>', A)) ;
        end
    end
else
    if (isscalar (B))
        % A is a matrix, B is a scalar
        [m, n] = size (A) ;
        b = gb_get_scalar (B) ;
        if (b < 0 && ~GrB.issigned (A))
            % b is negative, and A has an unsigned type.  C is all false.
            C = GrB (m, n, 'logical') ;
        elseif (b > 0)
            % since b > 0, entries not present in A result in a true
            % value, so the result is dense.  Expand B to a dense matrix.
            % B (1:m,1:n) = B and cast to ctype
            B = GrB.subassign (GrB (m, n, ctype), B) ;
            A = gb_full (A, ctype) ;
            C = GrB.emult (A, '<', B) ;
        else
            % since b <= 0, entries not present in A result in a false
            % value, so the result is a sparse subset of A.  Select all
            % entries in A < b, then convert to true.
            C = GrB.apply ('1.logical', GrB.select (A, '<', B)) ;
        end
    else
        % both A and B are matrices.  C is the set union of A and B.
        C = GrB (gb_union_op ('<', gb (A), gb (B))) ;
    end
end

