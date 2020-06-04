function C = conj (G)
%CONJ complex conjugate of a GraphBLAS matrix.
%
% See also GrB/real, GrB/imag.

% SuiteSparse:GraphBLAS, Timothy A. Davis, (c) 2017-2020, All Rights
% Reserved. http://suitesparse.com.  See GraphBLAS/Doc/License.txt.

Q = G.opaque ;

if (contains (gbtype (Q), 'complex')) ;
    C = GrB (gbapply ('conj', Q)) ;
else
    C = G ;
end

