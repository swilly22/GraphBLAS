function C = offdiag (A)
%GRB.OFFDIAG removes diaogonal entries from the matrix G.
% C = GrB.offdiag (A) removes diagonal entries from G.
%
% See also GrB/tril, GrB/triu, GrB/diag, GrB.select.

% SuiteSparse:GraphBLAS, Timothy A. Davis, (c) 2017-2020, All Rights
% Reserved. http://suitesparse.com.  See GraphBLAS/Doc/License.txt.

if (isobject (A))
    A = A.opaque ;
end

C = GrB (gbselect ('offdiag', A, 0)) ;

