function C = log1p (G)
%LOG1P natural logarithm of the entries of a GraphBLAS matrix
% C = log1p (G) computes the log(1+x) for each entry of a GraphBLAS
% matrix G.  If any entry in G is < -1, the result is complex.
%
% See also GrB/log, GrB/log2, GrB/log10, GrB/exp.

% SuiteSparse:GraphBLAS, Timothy A. Davis, (c) 2017-2020, All Rights
% Reserved. http://suitesparse.com.  See GraphBLAS/Doc/License.txt.

C = GrB (gb_trig ('log1p', G.opaque)) ;

